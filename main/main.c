/*
 * main.c — ThinkINK EPD Phase 3: AI / SD dual-mode image system
 *
 * Flow:
 *   1. Init NVS
 *   2. Init SD card (persistent mount) + config (NVS mode + config.json)
 *   3. Init text renderer (FreeType font from SD card)
 *   4. Init WiFi manager + button
 *   5. Dispatch by mode:
 *        AI → DeepSeek quote → Hunyuan image → download → decode RGBA
 *        SD → next PNG in /sdcard/images → decode RGBA
 *   6. Text overlay (AI mode) → RGB565 → dither → EPD
 *   7. Button: short = next image (SD), long = toggle AI/SD,
 *      very-long = enter provisioning
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_http_client.h"
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

/* Text rendering */
#include "text_renderer.h"

/* Input subsystem */
#include "button.h"

/* Web config */
#include "web_server.h"

/* Config & data */
#include "ai_config.h"
#include "image_data.h"

/* Storage */
#include "sd_card.h"
#include "config_store.h"

static const char *TAG = "main";

/* ---- Runtime state ---- */
static bool s_text_ok = false;            /* text renderer initialised */
static thinkink_config_t s_cfg;
static int s_sd_index = 0;                /* current SD image index */

/* Button requests — callbacks only set these flags; the heavy work
 * (decode/dither/WiFi/provisioning) runs in the main loop, which has a
 * much larger stack than the 2KB button task. */
static volatile bool s_req_next_image = false;
static volatile bool s_req_toggle_mode = false;
static volatile bool s_req_provisioning = false;

/* ---- EPD display helpers ---- */

static void epd_show_provisioning_info(const char *ap_ssid) {
    printf("===================================\n");
    printf("  WiFi AP: %s\n", ap_ssid);
    printf("  Open browser: http://192.168.4.1\n");
    printf("===================================\n");
}

static void epd_show_rgb565(const uint8_t *rgb565) {
    uint8_t *bw_buffer = (uint8_t *)calloc(1, BUFFER_SIZE);
    uint8_t *red_buffer = (uint8_t *)calloc(1, BUFFER_SIZE);

    if (!bw_buffer || !red_buffer) {
        ESP_LOGE(TAG, "Failed to allocate EPD buffers");
        free(bw_buffer);
        free(red_buffer);
        return;
    }

    ESP_LOGI(TAG, "Dithering to EPD...");
    dither_rgb565_to_epd(rgb565, bw_buffer, red_buffer);

    ESP_LOGI(TAG, "Writing to EPD...");
    epdInit();
    epdWriteRED(red_buffer, BUFFER_SIZE);
    epdWriteKW(bw_buffer, BUFFER_SIZE);
    epdUpdate(false);
    epdDown();
    ESP_LOGI(TAG, "EPD display complete");

    free(bw_buffer);
    free(red_buffer);
}

/* ---- Shared: RGBA framebuffer → RGB565 → dither → EPD ---- */

static void display_rgba(uint8_t *rgba) {
    uint8_t *rgb565 = NULL;
    esp_err_t err = image_rgba_to_rgb565(rgba, 800, 480, &rgb565);
    free(rgba);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "RGBA→RGB565 failed — skipping EPD update");
        return;
    }
    ESP_LOGI(TAG, "RGB565 buffer ready (%d bytes)", 800 * 480 * 2);
    epd_show_rgb565(rgb565);
    free(rgb565);
}

/* ---- AI pipeline ---- */

static void run_ai_pipeline(void) {
    ESP_LOGI(TAG, "========== AI Daily Art Pipeline ==========");

    char saved_quote[512] = {0};
    char saved_source[512] = {0};

    /* Stage 1: DeepSeek daily quote */
    daily_quote_t *q = (daily_quote_t *)calloc(1, sizeof(daily_quote_t));
    if (!q) {
        ESP_LOGE(TAG, "Failed to alloc daily_quote_t");
        return;
    }
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

    /* Stage 2: build image prompt + generate */
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

    ESP_LOGI(TAG, "Image URL: %.100s...", img_result->image_url);

    /* Stage 3: download + decode to RGBA */
    uint8_t *rgba = NULL;
    image_info_t img_info;
    err = image_download_and_decode_rgba(img_result->image_url, &rgba, &img_info);
    free(img_result);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Image download/decode failed — skipping EPD update");
        free(rgba);
        return;
    }

    ESP_LOGI(TAG, "RGBA8 framebuffer ready (%d bytes)", 800 * 480 * 4);

    /* Stage 4: text overlay */
    if (s_text_ok) {
        err = text_render_overlay(rgba, 800, 480, saved_quote, saved_source);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Text overlay failed — showing image without text");
        }
    } else {
        ESP_LOGI(TAG, "Text renderer not available — showing image without text");
    }

    /* Stage 5: display */
    display_rgba(rgba);
    ESP_LOGI(TAG, "========== AI Daily Art Pipeline complete ==========");
}

/* ---- SD pipeline ---- */

static void run_sd_pipeline(void) {
    ESP_LOGI(TAG, "========== SD Image Pipeline ==========");

    if (!sd_card_is_mounted()) {
        ESP_LOGE(TAG, "SD card not mounted — nothing to display");
        return;
    }

    int count = sd_card_count_images();
    if (count <= 0) {
        ESP_LOGW(TAG, "No images found in %s", s_cfg.image_dir);
        return;
    }

    /* Wrap the index */
    if (s_sd_index >= count) s_sd_index = 0;
    if (s_sd_index < 0) s_sd_index = 0;

    char path[256];
    /* Use config image_dir if set, else default */
    const char *img_dir = s_cfg.image_dir[0] ? s_cfg.image_dir : SD_IMAGES_DIR;

    /* Enumerate the n-th image path */
    esp_err_t err = sd_card_get_image_path(s_sd_index, path, sizeof(path));
    if (err != ESP_OK) {
        /* Fall back to constructing the path from index if enumeration fails */
        ESP_LOGE(TAG, "Failed to get image %d: %s", s_sd_index, esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG, "Loading image [%d/%d]: %s", s_sd_index + 1, count, path);

    uint8_t *rgba = NULL;
    image_info_t img_info;
    err = image_file_decode_rgba(path, &rgba, &img_info);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to decode image: %s", path);
        return;
    }

    ESP_LOGI(TAG, "Decoded %ux%u → 800×480 RGBA", img_info.width, img_info.height);

    display_rgba(rgba);
    ESP_LOGI(TAG, "========== SD Image Pipeline complete ==========");
}

/* ---- Button callbacks — set flags only, never run heavy work here ---- */

static void on_short_press(void) {
    s_req_next_image = true;
    ESP_LOGI(TAG, "Short press → request next image");
}

static void on_long_press(void) {
    s_req_toggle_mode = true;
    ESP_LOGI(TAG, "Long press → request mode toggle");
}

static void on_very_long_press(void) {
    s_req_provisioning = true;
    ESP_LOGI(TAG, "Very-long press → request provisioning");
}

/* ---- HTTP connectivity test (debug) ---- */

static void http_test_baidu(void) {
    ESP_LOGI(TAG, "=== HTTP test: GET http://baidu.com ===");

    esp_http_client_config_t cfg = {
        .url = "http://www.baidu.com",
        .method = HTTP_METHOD_GET,
        .timeout_ms = 10000,
        .buffer_size = 4096,
        .buffer_size_tx = 512,
        .user_agent = "ThinkINK-ESP32/1.0",
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGE(TAG, "Failed to init HTTP client");
        return;
    }

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        int status = esp_http_client_get_status_code(client);
        int content_len = esp_http_client_get_content_length(client);
        ESP_LOGI(TAG, "HTTP GET baidu.com: status=%d, content_length=%d",
                 status, content_len);
    } else {
        ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
    ESP_LOGI(TAG, "=== HTTP test done ===");
}

/* ---- Main app ---- */

int app_main(void) {
    esp_reset_reason_t rst_reason = esp_reset_reason();
    if (rst_reason == ESP_RST_BROWNOUT) {
        printf("Brownout detected!\n");
        return 1;
    }

    printf("\n========================================\n");
    printf("  ThinkINK EPD — Phase 3: AI/SD Dual\n");
    printf("========================================\n\n");

    /* ---- 1. Init NVS ---- */
    ESP_LOGI(TAG, "Initialising NVS...");
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS truncated, erasing...");
        nvs_flash_erase();
        nvs_flash_init();
    }

    /* ---- 2. Init SD card (persistent mount) ---- */
    ESP_LOGI(TAG, "Initialising SD card...");
    sd_card_init();  /* non-fatal — SD may be absent */

    /* ---- 3. Load config (NVS mode + config.json) ---- */
    config_store_load(&s_cfg);

    /* ---- 4. Init text renderer (font from SD) ---- */
    ESP_LOGI(TAG, "Initialising text renderer...");
    if (text_renderer_init(s_cfg.font) == ESP_OK) {
        s_text_ok = true;
    } else {
        ESP_LOGE(TAG, "text_renderer_init failed — text overlay unavailable");
    }

    /* ---- 5. Init WiFi manager ---- */
    ESP_LOGI(TAG, "Initialising WiFi manager...");
    esp_err_t err = wifi_manager_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi_manager_init failed: %s", esp_err_to_name(err));
        return 1;
    }

    /* ---- 6. Init button (three levels) ---- */
    ESP_LOGI(TAG, "Initialising button (GPIO0)...");
    button_init(0, on_short_press, on_long_press, on_very_long_press);

    /* ---- 7. Initial mode dispatch ---- */
    if (s_cfg.mode == THINKINK_MODE_SD) {
        ESP_LOGI(TAG, "Mode: SD — displaying local image");
        run_sd_pipeline();
    } else {
        ESP_LOGI(TAG, "Mode: AI — connecting WiFi then generate");
        if (wifi_manager_has_config()) {
            err = wifi_manager_connect();
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "WiFi connected!");
            } else {
                ESP_LOGW(TAG, "WiFi connect failed (%s), starting provisioning",
                         esp_err_to_name(err));
                wifi_manager_start_provisioning();
                web_server_start();
                const char *ap_ssid = wifi_manager_get_ap_ssid();
                epd_show_provisioning_info(ap_ssid);
            }
        } else {
            ESP_LOGI(TAG, "No WiFi config in NVS, starting provisioning");
            wifi_manager_start_provisioning();
            web_server_start();
            const char *ap_ssid = wifi_manager_get_ap_ssid();
            epd_show_provisioning_info(ap_ssid);
        }
    }

    /* ---- 8. Main loop ---- */
    bool ai_pipeline_done = false;

    while (1) {
        /* Provisioning form submitted */
        if (web_server_provision_done()) {
            char ssid[65] = {0};
            char pass[65] = {0};
            web_server_provision_get(ssid, sizeof(ssid), pass, sizeof(pass));
            web_server_provision_reset();

            ESP_LOGI(TAG, "Provisioning complete: SSID=\"%s\"", ssid);

            wifi_manager_save_config(ssid, pass);
            wifi_manager_stop_provisioning();
            web_server_stop();

            err = wifi_manager_connect();
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "WiFi connected after provisioning!");
            } else {
                ESP_LOGE(TAG, "WiFi connect failed after provisioning: %s",
                         esp_err_to_name(err));
            }
        }

        /* ---- Button: short press → next image (SD mode only) ---- */
        if (s_req_next_image) {
            s_req_next_image = false;
            if (s_cfg.mode == THINKINK_MODE_SD) {
                s_sd_index++;
                ESP_LOGI(TAG, "Next SD image (%d)", s_sd_index);
                run_sd_pipeline();
            }
        }

        /* ---- Button: long press → toggle AI/SD mode ---- */
        if (s_req_toggle_mode) {
            s_req_toggle_mode = false;
            s_cfg.mode = (s_cfg.mode == THINKINK_MODE_AI) ? THINKINK_MODE_SD : THINKINK_MODE_AI;
            config_store_save_mode(s_cfg.mode);
            ai_pipeline_done = false;
            ESP_LOGI(TAG, "Mode switched to %s",
                     s_cfg.mode == THINKINK_MODE_AI ? "AI" : "SD");
            if (s_cfg.mode == THINKINK_MODE_SD) {
                run_sd_pipeline();
            } else if (wifi_manager_has_config()) {
                wifi_manager_connect();
            }
        }

        /* ---- Button: very-long press → enter provisioning ---- */
        if (s_req_provisioning) {
            s_req_provisioning = false;
            ESP_LOGI(TAG, "Entering provisioning mode...");
            wifi_manager_start_provisioning();
            web_server_start();
            const char *ap_ssid = wifi_manager_get_ap_ssid();
            epd_show_provisioning_info(ap_ssid);
        }

        /* AI mode: once connected, run the AI pipeline (once) */
        if (s_cfg.mode == THINKINK_MODE_AI &&
            wifi_manager_is_connected() && !ai_pipeline_done) {
            ai_pipeline_done = true;
            ESP_LOGI(TAG, "WiFi connected! Starting AI pipeline...");
            http_test_baidu();
            run_ai_pipeline();
        }

        vTaskDelay(pdMS_TO_TICKS(500));
    }

    return 0;
}
