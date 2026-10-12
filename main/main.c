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
 * Content orchestration (image+quote selection, AI pipeline, daily task) lives
 * in content/daily_content.c; this file is boot + wakeup dispatch + the admin
 * backend loop + provisioning.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "nvs_flash.h"

#include "wifi_manager.h"
#include "web_server.h"
#include "admin_server.h"

#include "sd_card.h"
#include "config_store.h"
#include "image_library.h"
#include "log_to_sd.h"
#include "ota_manager.h"

#include "sleep_manager.h"
#include "button.h"
#include "daily_content.h"

static const char *TAG = "main";

static thinkink_config_t s_cfg;
static volatile bool s_btn_short = false;   /* short-press flag set by button task */

static void epd_show_provisioning_info(const char *ap_ssid) {
    printf("===================================\n");
    printf("  WiFi AP: %s\n", ap_ssid);
    printf("  Open browser: http://192.168.4.1\n");
    printf("===================================\n");
}

/* Button task callback — just set a flag; the heavy work runs in the main
   loop (the button task stack is only 2KB). */
static void on_btn_short_press(void) {
    s_btn_short = true;
}

/* ---- Admin backend: WiFi + mDNS + HTTP, auto-off after 15 min ---- */

static void run_admin_backend(void) {
    ESP_LOGI(TAG, "===== Admin backend (button wakeup) =====");

    if (!content_ensure_wifi()) {
        ESP_LOGW(TAG, "No WiFi — can't start admin backend");
        return;
    }

    admin_server_set_mode(s_cfg.mode);
    if (admin_server_start() != ESP_OK) {
        ESP_LOGW(TAG, "Admin server start failed");
        return;
    }

    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(ADMIN_TIMEOUT_SEC * 1000);
    while (wifi_manager_is_connected() && xTaskGetTickCount() < deadline) {
        int req = admin_server_take_request();
        if (req != ADMIN_REQ_NONE) {
            content_handle_request(req);
            deadline = xTaskGetTickCount() + pdMS_TO_TICKS(ADMIN_TIMEOUT_SEC * 1000);
        }
        if (s_btn_short) {
            s_btn_short = false;
            content_show_next();
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

    content_run_daily_task();
}

/* ---- Main ---- */

int app_main(void) {
    sleep_wake_cause_t wc = sleep_manager_get_wake_cause();
    esp_reset_reason_t rr = esp_reset_reason();

    printf("\n========================================\n");
    printf("  ThinkINK EPD — low-power daily art\n");
    printf("  wakeup cause = %d\n", (int)wc);
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

    /* If config.json provides WiFi credentials, push them to NVS. */
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

    /* ---- Init content (config) ---- */
    content_init(&s_cfg);

    /* ---- Boot self-test passed: confirm the running firmware is valid ---- */
    ota_mark_app_valid();

    /* ---- Dispatch on wakeup cause ---- */
    if (wc == SLEEP_WAKE_GPIO) {
        /* Button wakeup → open the admin backend (no daily task). */
        run_admin_backend();
    } else if (wc == SLEEP_WAKE_TIMER) {
        /* Timer wakeup → daily task only (no admin, to save power). */
        if (!wifi_manager_has_config()) {
            run_provisioning();
        } else {
            content_run_daily_task();
        }
    } else {
        /* Power-on → daily task, then the admin backend for 15 min. */
        if (!wifi_manager_has_config()) {
            run_provisioning();
        } else {
            content_run_daily_task();
            run_admin_backend();
        }
    }

    /* ---- Enter light sleep until next wakeup ---- */
    sleep_manager_enter(s_cfg.refresh_hour);
    return 0;   /* unreachable — sleep_manager_enter() restarts on wake */
}
