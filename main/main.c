/*
 * main.c — ThinkINK EPD Phase 3: DeepSeek + Hunyuan two-stage AI pipeline
 *
 * Flow:
 *   1. Init hardware (SPI I/O, EPD, button)
 *   2. Init WiFi manager
 *   3. Check NVS for saved WiFi credentials
 *   4. No config → SoftAP provisioning
 *   5. Has config → STA connect
 *   6. Connected → DeepSeek daily quote → Hunyuan image → download → decode → dither → EPD
 *   7. Button long-press → enter provisioning mode
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

/* Input subsystem */
#include "button.h"

/* Web config */
#include "web_server.h"

/* Config & data */
#include "ai_config.h"
#include "image_data.h"

static const char *TAG = "main";

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

static void epd_show_default_image(void) {
    epd_show_rgb565(image_bin_start);
}

/* Two-stage AI pipeline */
static void run_ai_pipeline(void) {
    ESP_LOGI(TAG, "========== AI Daily Art Pipeline ==========");

    /* ---- Stage 1: DeepSeek daily quote ---- */
    daily_quote_t *quote = (daily_quote_t *)calloc(1, sizeof(daily_quote_t));
    if (!quote) {
        ESP_LOGE(TAG, "Failed to alloc daily_quote_t");
        return;
    }
    esp_err_t err = ai_generate_daily_quote(quote);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Daily quote failed — skipping image generation");
        free(quote);
        return;
    }

    ESP_LOGI(TAG, "Daily quote: \"%s\" — %s (%s)", quote->quote, quote->author, quote->source);

    /* ---- Build image prompt from quote ---- */
    char *image_prompt = (char *)malloc(AI_CHAT_RESPONSE_MAX_LEN);
    if (!image_prompt) {
        ESP_LOGE(TAG, "Failed to alloc image prompt");
        free(quote);
        return;
    }
    ai_build_image_prompt(quote, image_prompt, AI_CHAT_RESPONSE_MAX_LEN);
    free(quote);

    /* ---- Stage 2: Hunyuan image generation ---- */
    ai_image_result_t *img_result = (ai_image_result_t *)calloc(1, sizeof(ai_image_result_t));
    if (!img_result) {
        ESP_LOGE(TAG, "Failed to alloc ai_image_result_t");
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
    if (img_result->revised_prompt[0]) {
        ESP_LOGI(TAG, "Revised prompt: %.200s...", img_result->revised_prompt);
    }

    /* ---- Stage 3: Download → Decode → Crop → Scale → RGB565 ---- */
    uint8_t *rgb565 = NULL;
    err = image_download_and_decode(img_result->image_url, &rgb565);
    free(img_result);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Image pipeline failed — skipping EPD update");
        free(rgb565);
        return;
    }

    ESP_LOGI(TAG, "RGB565 buffer ready (%d bytes)", 800 * 480 * 2);

    /* ---- Stage 4: Dither + EPD display ---- */
    epd_show_rgb565(rgb565);

    free(rgb565);
    ESP_LOGI(TAG, "========== AI Daily Art Pipeline complete ==========");
}

/* ---- HTTP test: fetch baidu.com ---- */

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

        char *buf = malloc(4096);
        if (buf) {
            int read = esp_http_client_read(client, buf, 4095);
            if (read > 0) {
                buf[read] = '\0';
                ESP_LOGI(TAG, "Response body (%d bytes):\n%.500s...", read, buf);
            }
            free(buf);
        }
    } else {
        ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
    ESP_LOGI(TAG, "=== HTTP test done ===");
}

/* ---- Button callback ---- */

static void on_button_long_press(void) {
    ESP_LOGI(TAG, "Button long-press → entering provisioning mode");

    wifi_manager_start_provisioning();
    web_server_start();

    const char *ap_ssid = wifi_manager_get_ap_ssid();
    epd_show_provisioning_info(ap_ssid);
}

/* ---- Main app ---- */

int app_main(void) {
    esp_reset_reason_t rst_reason = esp_reset_reason();
    if (rst_reason == ESP_RST_BROWNOUT) {
        printf("Brownout detected!\n");
        return 1;
    }

    printf("\n========================================\n");
    printf("  ThinkINK EPD — Phase 3: Daily Art\n");
    printf("========================================\n\n");

    /* ---- 1. Init WiFi manager (includes NVS + netif + event loop) ---- */
    ESP_LOGI(TAG, "Initialising WiFi manager...");
    esp_err_t err = wifi_manager_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi_manager_init failed: %s", esp_err_to_name(err));
        return 1;
    }

    /* ---- 2. Init button (GPIO0, long-press = enter provisioning) ---- */
    ESP_LOGI(TAG, "Initialising button (GPIO0)...");
    button_init(0, on_button_long_press);

    /* ---- 3. WiFi decision ---- */
    if (wifi_manager_has_config()) {
        ESP_LOGI(TAG, "WiFi config found in NVS, connecting...");
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

    /* ---- 4. Main loop ---- */
    bool pipeline_done = false;

    while (1) {
        /* Check if provisioning form was submitted */
        if (web_server_provision_done()) {
            char ssid[65] = {0};
            char pass[65] = {0};
            web_server_provision_get(ssid, sizeof(ssid), pass, sizeof(pass));
            web_server_provision_reset();

            ESP_LOGI(TAG, "Provisioning complete: SSID=\"%s\"", ssid);

            wifi_manager_save_config(ssid, pass);
            wifi_manager_stop_provisioning();
            web_server_stop();

            ESP_LOGI(TAG, "Connecting with new credentials...");
            err = wifi_manager_connect();
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "WiFi connected after provisioning!");
            } else {
                ESP_LOGE(TAG, "WiFi connect failed after provisioning: %s",
                         esp_err_to_name(err));
            }
        }

        /* Once connected, run the AI art pipeline (once) */
        if (wifi_manager_is_connected() && !pipeline_done) {
            pipeline_done = true;
            ESP_LOGI(TAG, "WiFi connected! Starting daily art pipeline...");

            /* Quick HTTP test first to verify connectivity */
            http_test_baidu();

            /* Then do the full two-stage AI pipeline */
            run_ai_pipeline();
        }

        vTaskDelay(pdMS_TO_TICKS(500));
    }

    return 0;
}
