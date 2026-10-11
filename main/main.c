/*
 * main.c — ThinkINK EPD low-power daily-art frame
 *
 * Battery-powered: the device sleeps most of the time and wakes once a day
 * (RTC timer at config.json's refresh_hour) or on a GPIO48 button press.
 *
 * Wakeup dispatch:
 *   ESP_SLEEP_WAKEUP_UNDEFINED (power-on) → daily task (or provisioning if no WiFi)
 *   ESP_SLEEP_WAKEUP_TIMER    (daily)      → daily task
 *   ESP_SLEEP_WAKEUP_GPIO     (button)     → connect WiFi + open admin backend 15 min
 *
 * After the task completes we enter light sleep via sleep_manager_enter(),
 * which on wake calls esp_restart() so the app boots clean each time.
 *
 * Daily task:
 *   AI mode → connect WiFi → generate once per day → disconnect
 *   SD mode → rotate to the next local image (no WiFi needed)
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_netif_sntp.h"
#include "nvs_flash.h"

/* Display subsystem */
#include "epd.h"
#include "dither.h"
#include "io.h"

/* Network subsystem */
#include "wifi_manager.h"
#include "ai_client.h"

/* Image subsystem */
#include "image_decoder.h"
#include "image_processing.h"

/* Text rendering */
#include "text_renderer.h"

/* Web */
#include "web_server.h"
#include "admin_server.h"

/* Config & data */
#include "ai_config.h"

/* Storage */
#include "sd_card.h"
#include "config_store.h"
#include "image_library.h"
#include "quote_library.h"
#include "daily.h"
#include "log_to_sd.h"
#include "ota_manager.h"

/* Power */
#include "sleep_manager.h"

/* Input */
#include "button.h"

static const char *TAG = "main";

static thinkink_config_t s_cfg;
static bool s_text_ok = false;   /* text renderer initialised (AI mode only) */
static volatile bool s_btn_short = false;   /* short-press flag set by button task */

/* ---- EPD display helpers ---- */

static void epd_show_provisioning_info(const char *ap_ssid) {
    printf("===================================\n");
    printf("  WiFi AP: %s\n", ap_ssid);
    printf("  Open browser: http://192.168.4.1\n");
    printf("===================================\n");
}

static esp_err_t epd_show_rgb565(const uint8_t *rgb565) {
    uint8_t *bw_buffer = (uint8_t *)calloc(1, BUFFER_SIZE);
    uint8_t *red_buffer = (uint8_t *)calloc(1, BUFFER_SIZE);

    if (!bw_buffer || !red_buffer) {
        ESP_LOGE(TAG, "Failed to allocate EPD buffers");
        free(bw_buffer);
        free(red_buffer);
        return ESP_ERR_NO_MEM;
    }

    dither_rgb565_to_epd(rgb565, bw_buffer, red_buffer);

    epdInit();
    epdWriteRED(red_buffer, BUFFER_SIZE);
    epdWriteKW(bw_buffer, BUFFER_SIZE);
    epdUpdate(false);
    epdDown();

    free(bw_buffer);
    free(red_buffer);
    return ESP_OK;
}

/* ---- Shared: RGBA → preprocessing → RGB565 → dither → EPD ---- */

static esp_err_t display_rgba(uint8_t *rgba) {
    image_apply_processing(rgba, 800, 480);

    /* Panel is mounted upside-down — rotate the whole frame (image + any
       overlaid text) 180° before dithering so both are upright together. */
    image_rgba_rotate_180(rgba, 800, 480);

    uint8_t *rgb565 = NULL;
    esp_err_t err = image_rgba_to_rgb565(rgba, 800, 480, &rgb565);
    free(rgba);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "RGBA→RGB565 failed — skipping EPD update");
        return err;
    }
    err = epd_show_rgb565(rgb565);
    free(rgb565);
    return err;
}

/* ---- AI pipeline ---- */

static void run_ai_pipeline(void) {
    ESP_LOGI(TAG, "========== AI Daily Art Pipeline ==========");

    /* Lazy-init the text renderer — AI may be triggered from any path
       (daily task, or a web "switch to AI" while in the admin backend),
       and the font must be loaded before we can overlay the quote. */
    if (!s_text_ok) {
        s_text_ok = (text_renderer_init(s_cfg.font) == ESP_OK);
        if (!s_text_ok) {
            ESP_LOGW(TAG, "text_renderer_init failed — no text overlay");
        }
    }

    char saved_quote[512] = {0};
    char saved_source[512] = {0};

    daily_quote_t *q = (daily_quote_t *)calloc(1, sizeof(daily_quote_t));
    if (!q) return;
    esp_err_t err = ai_generate_daily_quote(q);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Daily quote failed — skipping image generation");
        free(q);
        return;
    }

    ESP_LOGI(TAG, "Daily quote: \"%s\" — %s (%s)", q->quote, q->author, q->source);

    strncpy(saved_quote, q->quote, sizeof(saved_quote) - 1);
    if (q->source[0]) {
        snprintf(saved_source, sizeof(saved_source), "—— %s（%s）", q->author, q->source);
    } else {
        snprintf(saved_source, sizeof(saved_source), "—— %s", q->author);
    }

    char *image_prompt = (char *)malloc(AI_CHAT_RESPONSE_MAX_LEN);
    if (!image_prompt) {
        free(q);
        return;
    }
    ai_build_image_prompt(q, image_prompt, AI_CHAT_RESPONSE_MAX_LEN);
    free(q);

    ai_image_result_t *img_result = (ai_image_result_t *)calloc(1, sizeof(ai_image_result_t));
    if (!img_result) {
        free(image_prompt);
        return;
    }
    err = ai_generate_image(image_prompt, "854x512", img_result);
    free(image_prompt);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Image generation failed — skipping download");
        free(img_result);
        return;
    }

    uint8_t *rgba = NULL;
    image_info_t img_info;
    err = image_download_and_decode_rgba(img_result->image_url, &rgba, &img_info);
    free(img_result);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Image download/decode failed — skipping EPD update");
        free(rgba);
        return;
    }

    if (s_text_ok) {
        text_render_overlay(rgba, 800, 480, saved_quote, saved_source);
    } else {
        ESP_LOGI(TAG, "Text renderer not available — showing image without text");
    }

    display_rgba(rgba);
    ESP_LOGI(TAG, "========== AI Daily Art Pipeline complete ==========");
}

/* ---- SD daily content: one image + one quote, fixed per day ---- */

/* Load, overlay and display one image + one quote.  Shared by the daily task
   and the button short-press (next image). */
static void display_combined(uint32_t image_id, uint32_t quote_id) {
    char path[256];
    image_library_get_path(image_id, path, sizeof(path));

    uint8_t *rgba = NULL;
    image_info_t info;
    if (image_file_decode_rgba(path, &rgba, &info) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to decode image id=%lu", (unsigned long)image_id);
        return;
    }

    /* Load the quote and overlay it. */
    bool have_quote = false;
    char quote_text[QUOTE_TEXT_MAX] = {0};
    char source[320] = {0};
    if (quote_id != 0) {
        quote_t q;
        if (quote_library_get_by_id(quote_id, &q) == ESP_OK) {
            strncpy(quote_text, q.text, sizeof(quote_text) - 1);
            if (q.source[0]) snprintf(source, sizeof(source), "—— %s（%s）", q.author, q.source);
            else if (q.author[0]) snprintf(source, sizeof(source), "—— %s", q.author);
            have_quote = true;
        }
    }

    if (s_text_ok && have_quote) {
        /* Portrait images are rotated 90° — overlay text on the side; landscape
           keeps the normal bottom overlay. */
        if (info.height > info.width) {
            text_render_side_overlay(rgba, 800, 480, quote_text, source);
        } else {
            text_render_overlay(rgba, 800, 480, quote_text, source);
        }
    }

    if (display_rgba(rgba) == ESP_OK) {
        if (image_id != 0) image_library_mark_used(image_id);
        if (have_quote && quote_id != 0) quote_library_mark_used(quote_id);
    }
}

/* Lazy-init the text renderer (shared by all image+quote display paths). */
static void ensure_text_renderer(void) {
    if (!s_text_ok) {
        s_text_ok = (text_renderer_init(s_cfg.font) == ESP_OK);
        if (!s_text_ok) ESP_LOGW(TAG, "text_renderer_init failed — no text");
    }
}

/* Pick a random quote and show it together with `image_id`. */
static void show_image_with_quote(uint32_t image_id) {
    uint32_t quote_id = 0;
    quote_t q;
    if (quote_library_get_random(&q) == ESP_OK) quote_id = q.id;
    display_combined(image_id, quote_id);
}

static void run_daily_content(void) {
    ESP_LOGI(TAG, "========== SD Daily Content ==========");

    if (!sd_card_is_mounted()) {
        ESP_LOGE(TAG, "SD card not mounted — nothing to display");
        return;
    }

    /* Lazy-init the text renderer (same as the AI path). */
    if (!s_text_ok) {
        s_text_ok = (text_renderer_init(s_cfg.font) == ESP_OK);
        if (!s_text_ok) ESP_LOGW(TAG, "text_renderer_init failed — no text");
    }

    /* Trust wall-clock only after SNTP sync; otherwise we just show a
       fresh pair without claiming it is "today's". */
    time_t now = time(NULL);
    bool have_time = (now > 946684800L);   /* 2000-01-01 */
    char today[40] = {0};
    if (have_time) {
        struct tm tmv;
        localtime_r(&now, &tmv);
        snprintf(today, sizeof(today), "%04d-%02d-%02d",
                 tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday);
    }

    uint32_t image_id = 0;
    uint32_t quote_id = 0;

    if (have_time && daily_get(today, &image_id, &quote_id) == ESP_OK) {
        ESP_LOGI(TAG, "%s already fixed: image %lu + quote %lu",
                 today, (unsigned long)image_id, (unsigned long)quote_id);
    } else {
        /* Pick a random image and the least-used quote. */
        if (image_library_get_random_id(&image_id) != ESP_OK) {
            ESP_LOGW(TAG, "No images in library — nothing to display");
            return;
        }
        quote_t q;
        if (quote_library_choose(&q) == ESP_OK) quote_id = q.id;
        if (have_time) daily_set(today, image_id, quote_id);
    }

    display_combined(image_id, quote_id);
    ESP_LOGI(TAG, "========== SD Daily Content complete ==========");
}

/* Button short-press / Web next-prev — show a fresh random image + quote. */
static bool ensure_wifi_connected(void);

static void show_next_content(void) {
    ESP_LOGI(TAG, "===== Next content =====");

    if (s_cfg.mode == THINKINK_MODE_AI) {
        /* AI mode: regenerate the image + quote */
        if (!ensure_wifi_connected()) {
            ESP_LOGW(TAG, "No WiFi — can't regenerate AI content");
            return;
        }
        run_ai_pipeline();
        return;
    }

    /* SD mode: random image + quote */
    if (!sd_card_is_mounted()) {
        ESP_LOGW(TAG, "SD card not mounted");
        return;
    }

    ensure_text_renderer();

    uint32_t image_id = 0;
    if (image_library_get_random_id(&image_id) != ESP_OK) {
        ESP_LOGW(TAG, "No images in library");
        return;
    }

    show_image_with_quote(image_id);
}

/* Button task callback — just set a flag; the heavy work runs in the main
   loop (the button task stack is only 2KB). */
static void on_btn_short_press(void) {
    s_btn_short = true;
}

/* ---- SNTP time sync (best effort, ~3s) ---- */

static void sntp_sync_once(void) {
    static bool synced = false;
    if (synced) return;

    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    if (esp_netif_sntp_init(&cfg) != ESP_OK) {
        ESP_LOGW(TAG, "SNTP init failed");
        return;
    }
    esp_netif_sntp_sync_wait(pdMS_TO_TICKS(5000));
    esp_netif_sntp_deinit();
    synced = true;
}

/* ---- WiFi connect helper (best effort) ---- */

static bool ensure_wifi_connected(void) {
    if (wifi_manager_is_connected()) return true;
    if (!wifi_manager_has_config()) return false;
    esp_err_t err = wifi_manager_connect();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "WiFi connect failed: %s", esp_err_to_name(err));
        return false;
    }
    /* After connecting, try to sync the clock for the next wakeup schedule */
    sntp_sync_once();
    return true;
}

/* ---- Daily task: AI generate (once/day) or SD rotate ---- */

static void run_daily_task(void) {
    ESP_LOGI(TAG, "Daily task: mode=%s", s_cfg.mode == THINKINK_MODE_AI ? "ai" : "sd");

    if (s_cfg.mode == THINKINK_MODE_AI) {
        if (!ensure_wifi_connected()) {
            ESP_LOGW(TAG, "No WiFi in AI mode — nothing to do");
            return;
        }

        /* Once-per-day dedup: skip if we already generated today */
        time_t now = time(NULL);
        uint32_t today = (now > 0) ? (uint32_t)(now / 86400) : 0;
        uint32_t last = config_store_load_last_gen_day();
        if (today != 0 && last == today) {
            ESP_LOGI(TAG, "Already generated today (day %lu) — skipping", (unsigned long)today);
            return;
        }

        run_ai_pipeline();
        if (today != 0) config_store_save_last_gen_day(today);
    } else {
        /* SD mode: one image + one quote, fixed per day */
        run_daily_content();
    }
}

/* ---- Admin backend: WiFi + mDNS + HTTP, auto-off after 15 min ---- */

static void handle_admin_request(int req) {
    switch (req) {
    case ADMIN_REQ_NEXT:
    case ADMIN_REQ_PREV:
    case ADMIN_REQ_REFRESH:
        if (s_cfg.mode == THINKINK_MODE_SD) {
            show_next_content();
        }
        break;
    case ADMIN_REQ_DISPLAY_FILE: {
        const char *fn = admin_server_get_request_filename();
        uint32_t id = image_library_id_from_name(fn);
        if (id == 0) {
            ESP_LOGW(TAG, "Web: invalid image id '%s'", fn);
            break;
        }
        ESP_LOGI(TAG, "Web: display image id=%lu", (unsigned long)id);
        ensure_text_renderer();
        show_image_with_quote(id);
        break;
    }
    case ADMIN_REQ_MODE_AI:
        if (s_cfg.mode != THINKINK_MODE_AI) {
            s_cfg.mode = THINKINK_MODE_AI;
            config_store_save_mode(s_cfg.mode);
            config_store_save_mode_to_file(s_cfg.mode);
            admin_server_set_mode(s_cfg.mode);
            run_ai_pipeline();
        }
        break;
    case ADMIN_REQ_MODE_SD:
        if (s_cfg.mode != THINKINK_MODE_SD) {
            s_cfg.mode = THINKINK_MODE_SD;
            config_store_save_mode(s_cfg.mode);
            config_store_save_mode_to_file(s_cfg.mode);
            admin_server_set_mode(s_cfg.mode);
            show_next_content();
        }
        break;
    default:
        break;
    }
}

static void run_admin_backend(void) {
    ESP_LOGI(TAG, "===== Admin backend (button wakeup) =====");

    if (!ensure_wifi_connected()) {
        ESP_LOGW(TAG, "No WiFi — can't start admin backend");
        return;
    }

    admin_server_set_mode(s_cfg.mode);
    if (admin_server_start() != ESP_OK) {
        ESP_LOGW(TAG, "Admin server start failed");
        return;
    }

    /* Keep serving until ADMIN_TIMEOUT_SEC of inactivity, or WiFi drops */
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(ADMIN_TIMEOUT_SEC * 1000);
    while (wifi_manager_is_connected() && xTaskGetTickCount() < deadline) {
        int req = admin_server_take_request();
        if (req != ADMIN_REQ_NONE) {
            handle_admin_request(req);
            /* Activity resets the 15-minute timer */
            deadline = xTaskGetTickCount() + pdMS_TO_TICKS(ADMIN_TIMEOUT_SEC * 1000);
        }
        if (s_btn_short) {
            s_btn_short = false;
            show_next_content();
            /* Button activity also resets the 15-minute timer */
            deadline = xTaskGetTickCount() + pdMS_TO_TICKS(ADMIN_TIMEOUT_SEC * 1000);
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    ESP_LOGI(TAG, "Admin backend timeout — stopping");
    admin_server_stop();
}

/* ---- Provisioning (first boot, no WiFi saved) ---- */

static void run_provisioning(void) {
    ESP_LOGI(TAG, "===== Provisioning (no WiFi config) =====");
    wifi_manager_start_provisioning();
    web_server_start();
    const char *ap_ssid = wifi_manager_get_ap_ssid();
    epd_show_provisioning_info(ap_ssid);

    /* Block until the user submits credentials (no sleep while provisioning) */
    while (!web_server_provision_done()) {
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    char ssid[65] = {0}, pass[65] = {0};
    web_server_provision_get(ssid, sizeof(ssid), pass, sizeof(pass));
    web_server_provision_reset();

    wifi_manager_save_config(ssid, pass);
    wifi_manager_stop_provisioning();
    web_server_stop();

    ESP_LOGI(TAG, "Provisioning complete: SSID=\"%s\"", ssid);

    /* After provisioning, run the daily task immediately so the user sees it */
    run_daily_task();
}

/* ---- Main ---- */

int app_main(void) {
    uint32_t causes = esp_sleep_get_wakeup_causes();
    esp_reset_reason_t rr = esp_reset_reason();

    printf("\n========================================\n");
    printf("  ThinkINK EPD — low-power daily art\n");
    printf("  wakeup causes = 0x%lx\n", (unsigned long)causes);
    printf("  reset reason = %d\n", (int)rr);
    printf("========================================\n\n");

    /* ---- Init NVS ---- */
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    /* ---- Init SD + config ---- */
    sd_card_init();
    if (sd_card_is_mounted()) {
        image_library_init();
        log_to_sd_init();
    }
    config_store_load(&s_cfg);

    /* Record the reset reason into the SD log too (log_to_sd is now active). */
    ESP_LOGI(TAG, "reset reason = %d", (int)rr);

    /* If config.json provides WiFi credentials, push them to NVS so the
       WiFi manager (which reads NVS) uses the SD-card values. */
    if (s_cfg.wifi_ssid[0] != '\0') {
        wifi_manager_save_config(s_cfg.wifi_ssid, s_cfg.wifi_pass);
        ESP_LOGI(TAG, "WiFi credentials from config.json: SSID=\"%s\"", s_cfg.wifi_ssid);
    }

    /* ---- Init WiFi manager ---- */
    if (wifi_manager_init() != ESP_OK) {
        ESP_LOGE(TAG, "wifi_manager_init failed");
        return 1;
    }

    /* ---- Init button: short press = next image while awake ---- */
    button_init(SLEEP_BUTTON_GPIO, on_btn_short_press, NULL, NULL);

    /* ---- Boot self-test passed: confirm the running firmware is valid
       (cancels OTA rollback if this was a freshly-updated image) ---- */
    ota_mark_app_valid();

    /* ---- Dispatch on wakeup cause ---- */
    if (causes & (1 << ESP_SLEEP_WAKEUP_GPIO)) {
        /* Button press → open admin backend (WiFi + mDNS + HTTP) */
        run_admin_backend();
    } else if (!wifi_manager_has_config()) {
        /* First boot with no saved WiFi → provision */
        run_provisioning();
    } else {
        /* Power-on or timer → daily task (AI lazily loads its font) */
        run_daily_task();

        /* On POWER-ON (not timer), also open the admin backend for 15 min so
           the user can reach thinkink.local right after plugging in / boot. */
        if (causes & (1 << ESP_SLEEP_WAKEUP_UNDEFINED)) {
            run_admin_backend();
        }
    }

    /* ---- Enter light sleep until next wakeup ---- */
    sleep_manager_enter(s_cfg.refresh_hour);
    return 0;   /* unreachable — sleep_manager_enter() restarts on wake */
}
