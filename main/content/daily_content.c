/*
 * daily_content.c
 *
 * Content orchestration: EPD display, AI pipeline, image+quote combined
 * rendering, the daily task, and button/Web "next" handling.  See
 * daily_content.h.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"

#include "epd.h"
#include "dither.h"

#include "wifi_manager.h"
#include "ai_client.h"

#include "image_decoder.h"
#include "image_processing.h"

#include "text_renderer.h"

#include "admin_server.h"

#include "ai_config.h"

#include "sd_card.h"
#include "config_store.h"
#include "image_library.h"
#include "quote_library.h"
#include "daily.h"
#include "current.h"

#include "daily_content.h"

static const char *TAG = "content";

static const thinkink_config_t *s_cfg = NULL;
static bool s_text_ok = false;   /* text renderer initialised */
static volatile bool s_busy = false;   /* true while a refresh is in progress */

/* ---- EPD display helpers ---- */

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

/* RGBA → preprocessing → 180° rotate → RGB565 → dither → EPD */
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

/* Generate a fresh AI image and composite `quote_text` (with byline) over it,
   then display.  Shared by run_ai_pipeline() and the "image only" action. */
static void ai_render_image(const char *quote_text, const char *author, const char *source) {
    char byline[320] = {0};
    if (source[0]) snprintf(byline, sizeof(byline), "—— %s（%s）", author, source);
    else if (author[0]) snprintf(byline, sizeof(byline), "—— %s", author);

    char *image_prompt = (char *)malloc(AI_CHAT_RESPONSE_MAX_LEN);
    if (!image_prompt) return;
    ai_build_image_prompt(NULL, image_prompt, AI_CHAT_RESPONSE_MAX_LEN);  /* quote unused — theme-random */

    ai_image_result_t *img_result = (ai_image_result_t *)calloc(1, sizeof(ai_image_result_t));
    if (!img_result) {
        free(image_prompt);
        return;
    }
    esp_err_t err = ai_generate_image(image_prompt, "854x512", img_result);
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

    if (s_text_ok && quote_text[0]) {
        text_render_overlay(rgba, 800, 480, quote_text, byline);
    } else {
        ESP_LOGI(TAG, "Text renderer not available — showing image without text");
    }

    display_rgba(rgba);
}

static void run_ai_pipeline(void) {
    ESP_LOGI(TAG, "========== AI Daily Art Pipeline ==========");

    if (!s_text_ok) {
        s_text_ok = (text_renderer_init(s_cfg->font) == ESP_OK);
        if (!s_text_ok) {
            ESP_LOGW(TAG, "text_renderer_init failed — no text overlay");
        }
    }

    char saved_quote[512] = {0};
    char saved_author[128] = {0};
    char saved_source[256] = {0};

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
    strncpy(saved_author, q->author, sizeof(saved_author) - 1);
    strncpy(saved_source, q->source, sizeof(saved_source) - 1);
    free(q);

    ai_render_image(saved_quote, saved_author, saved_source);

    /* Record the current AI content so the Web UI can show it. */
    current_content_t cur = {0};
    cur.mode = THINKINK_MODE_AI;
    strncpy(cur.quote_text, saved_quote, sizeof(cur.quote_text) - 1);
    strncpy(cur.quote_author, saved_author, sizeof(cur.quote_author) - 1);
    strncpy(cur.quote_source, saved_source, sizeof(cur.quote_source) - 1);
    current_save(&cur);

    ESP_LOGI(TAG, "========== AI Daily Art Pipeline complete ==========");
}

/* ---- image + quote combined rendering ---- */

static void ensure_text_renderer(void) {
    if (!s_text_ok) {
        s_text_ok = (text_renderer_init(s_cfg->font) == ESP_OK);
        if (!s_text_ok) ESP_LOGW(TAG, "text_renderer_init failed — no text");
    }
}

static void display_combined(uint32_t image_id, uint32_t quote_id) {
    char path[256];
    image_library_get_path(image_id, path, sizeof(path));

    uint8_t *rgba = NULL;
    image_info_t info;
    if (image_file_decode_rgba(path, &rgba, &info) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to decode image id=%lu", (unsigned long)image_id);
        return;
    }

    bool have_quote = false;
    char quote_text[QUOTE_TEXT_MAX] = {0};
    char quote_author[QUOTE_AUTHOR_MAX] = {0};
    char quote_source[QUOTE_SOURCE_MAX] = {0};
    char source[320] = {0};
    if (quote_id != 0) {
        quote_t q;
        if (quote_library_get_by_id(quote_id, &q) == ESP_OK) {
            strncpy(quote_text, q.text, sizeof(quote_text) - 1);
            strncpy(quote_author, q.author, sizeof(quote_author) - 1);
            strncpy(quote_source, q.source, sizeof(quote_source) - 1);
            if (q.source[0]) snprintf(source, sizeof(source), "—— %s（%s）", q.author, q.source);
            else if (q.author[0]) snprintf(source, sizeof(source), "—— %s", q.author);
            have_quote = true;
        }
    }

    if (s_text_ok && have_quote) {
        if (info.height > info.width) {
            text_render_side_overlay(rgba, 800, 480, quote_text, source);
        } else {
            text_render_overlay(rgba, 800, 480, quote_text, source);
        }
    }

    if (display_rgba(rgba) == ESP_OK) {
        if (image_id != 0) image_library_mark_used(image_id);
        if (have_quote && quote_id != 0) quote_library_mark_used(quote_id);

        /* Record the current SD content for the Web UI. */
        current_content_t cur = {0};
        cur.mode = THINKINK_MODE_SD;
        cur.image_id = image_id;
        if (have_quote && quote_id != 0) {
            cur.quote_id = quote_id;
            strncpy(cur.quote_text, quote_text, sizeof(cur.quote_text) - 1);
            strncpy(cur.quote_author, quote_author, sizeof(cur.quote_author) - 1);
            strncpy(cur.quote_source, quote_source, sizeof(cur.quote_source) - 1);
        }
        current_save(&cur);
    }
}

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

    ensure_text_renderer();

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

/* ---- SNTP + WiFi ---- */

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

bool content_ensure_wifi(void) {
    if (wifi_manager_is_connected()) return true;
    if (!wifi_manager_has_config()) return false;
    esp_err_t err = wifi_manager_connect();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "WiFi connect failed: %s", esp_err_to_name(err));
        return false;
    }
    sntp_sync_once();
    return true;
}

/* ---- daily task / next ---- */

static void run_daily_task(void) {
    ESP_LOGI(TAG, "Daily task: mode=%s", s_cfg->mode == THINKINK_MODE_AI ? "ai" : "sd");

    if (s_cfg->mode == THINKINK_MODE_AI) {
        if (!content_ensure_wifi()) {
            ESP_LOGW(TAG, "No WiFi in AI mode — nothing to do");
            return;
        }

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
        run_daily_content();
    }
}

static void show_next_content(void) {
    ESP_LOGI(TAG, "===== Next content =====");

    if (s_cfg->mode == THINKINK_MODE_AI) {
        if (!content_ensure_wifi()) {
            ESP_LOGW(TAG, "No WiFi — can't regenerate AI content");
            return;
        }
        run_ai_pipeline();
        return;
    }

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

/* ---- "swap image only" / "delete current image" ---- */

/* "YYYY-MM-DD" for the current local time, or empty if no trustworthy clock. */
static void get_today(char *out, size_t len) {
    out[0] = '\0';
    time_t now = time(NULL);
    if (now > 946684800L) {   /* 2000-01-01 */
        struct tm tmv;
        localtime_r(&now, &tmv);
        snprintf(out, len, "%04d-%02d-%02d",
                 tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday);
    }
}

/* Keep the current quote, show a fresh image (AI + SD). */
static void show_image_only(void) {
    if (s_cfg->mode == THINKINK_MODE_AI) {
        current_content_t cur;
        if (current_load(&cur) != ESP_OK || cur.mode != THINKINK_MODE_AI || cur.quote_text[0] == '\0') {
            ESP_LOGW(TAG, "No current AI quote to reuse — running full AI pipeline");
            run_ai_pipeline();
            return;
        }
        if (!content_ensure_wifi()) {
            ESP_LOGW(TAG, "No WiFi — can't regenerate AI image");
            return;
        }
        ai_render_image(cur.quote_text, cur.quote_author, cur.quote_source);
        current_save(&cur);   /* text unchanged; image refreshed (mode already AI) */
        return;
    }

    if (!sd_card_is_mounted()) {
        ESP_LOGW(TAG, "SD card not mounted");
        return;
    }

    current_content_t cur;
    if (current_load(&cur) != ESP_OK || cur.quote_id == 0) {
        ESP_LOGW(TAG, "No current quote — falling back to next content");
        show_next_content();
        return;
    }

    uint32_t new_id = 0;
    if (image_library_get_random_id(&new_id) != ESP_OK) {
        ESP_LOGW(TAG, "No images in library");
        return;
    }
    ensure_text_renderer();
    display_combined(new_id, cur.quote_id);

    char today[40] = {0};
    get_today(today, sizeof(today));
    if (today[0]) daily_update(today, new_id, cur.quote_id);
}

/* Delete the currently shown image (SD only), then swap to another. */
static void delete_current_image(void) {
    if (s_cfg->mode != THINKINK_MODE_SD) return;

    if (!sd_card_is_mounted()) {
        ESP_LOGW(TAG, "SD card not mounted");
        return;
    }

    current_content_t cur;
    if (current_load(&cur) != ESP_OK || cur.image_id == 0) {
        ESP_LOGW(TAG, "No current image to delete");
        return;
    }

    image_library_remove(cur.image_id);

    uint32_t new_id = 0;
    if (image_library_get_random_id(&new_id) != ESP_OK) {
        ESP_LOGW(TAG, "No images left in library");
        return;
    }
    ensure_text_renderer();
    display_combined(new_id, cur.quote_id);

    char today[40] = {0};
    get_today(today, sizeof(today));
    if (today[0]) daily_update(today, new_id, cur.quote_id);
}

/* ---- public API ---- */

void content_init(const thinkink_config_t *cfg) {
    s_cfg = cfg;
}

void content_run_daily_task(void) {
    s_busy = true;
    run_daily_task();
    s_busy = false;
}

void content_show_next(void) {
    s_busy = true;
    show_next_content();
    s_busy = false;
}

bool content_is_busy(void) {
    return s_busy;
}

void content_handle_request(int req) {
    s_busy = true;
    switch (req) {
    case ADMIN_REQ_NEXT:
    case ADMIN_REQ_PREV:
    case ADMIN_REQ_REFRESH:
        if (s_cfg->mode == THINKINK_MODE_SD) {
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
        if (s_cfg->mode != THINKINK_MODE_AI) {
            /* NOTE: mode is mutable via the admin backend; s_cfg points at the
               main config struct, so writing through it updates the caller's. */
            ((thinkink_config_t *)s_cfg)->mode = THINKINK_MODE_AI;
            config_store_save_mode(THINKINK_MODE_AI);
            config_store_save_mode_to_file(THINKINK_MODE_AI);
            admin_server_set_mode(THINKINK_MODE_AI);
            run_ai_pipeline();
        }
        break;
    case ADMIN_REQ_MODE_SD:
        if (s_cfg->mode != THINKINK_MODE_SD) {
            ((thinkink_config_t *)s_cfg)->mode = THINKINK_MODE_SD;
            config_store_save_mode(THINKINK_MODE_SD);
            config_store_save_mode_to_file(THINKINK_MODE_SD);
            admin_server_set_mode(THINKINK_MODE_SD);
            show_next_content();
        }
        break;
    case ADMIN_REQ_IMAGE_ONLY:
        show_image_only();
        break;
    case ADMIN_REQ_DELETE_IMAGE:
        delete_current_image();
        break;
    default:
        break;
    }
    s_busy = false;
}
