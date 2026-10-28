/*
 * sleep_manager.c
 *
 * Low-power sleep orchestration (light sleep + RTC-timer/GPIO wakeup).
 */
#include <time.h>
#include <sys/time.h>
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "sleep_manager.h"
#include "wifi_manager.h"

static const char *TAG = "sleep";

#define WAKE_CAUSE_NVS_NAMESPACE  "thinkink"
#define WAKE_CAUSE_NVS_KEY        "wake_cause"

sleep_wake_cause_t sleep_manager_get_wake_cause(void) {
    /* Persisted in NVS (flash) before esp_restart(), so it survives the soft
       reset a light-sleep wake triggers.  RTC_DATA_ATTR does NOT survive
       esp_restart() on ESP32-S3/IDF v6 — hence NVS.  The key is erased on
       read, so a plain power-on sees NONE. */
    sleep_wake_cause_t wc = SLEEP_WAKE_NONE;

    nvs_handle_t h;
    if (nvs_open(WAKE_CAUSE_NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        return wc;
    }

    uint8_t v = (uint8_t)SLEEP_WAKE_NONE;
    if (nvs_get_u8(h, WAKE_CAUSE_NVS_KEY, &v) == ESP_OK) {
        wc = (sleep_wake_cause_t)v;
        nvs_erase_key(h, WAKE_CAUSE_NVS_KEY);
        nvs_commit(h);
    }
    nvs_close(h);
    return wc;
}

uint64_t sleep_manager_us_until_next_hour(uint32_t now, int hour) {
    /* Hour-of-day from unix time (UTC).  A full timezone-aware daily scheduler
       is out of scope for v1 — refresh_hour is treated as UTC for now. */
    uint32_t day = now / 86400;
    uint32_t seconds_today = now % 86400;
    uint32_t target = (uint32_t)hour * 3600;

    uint32_t delta;
    if (target > seconds_today) {
        delta = target - seconds_today;        /* later today */
    } else {
        delta = (86400 - seconds_today) + target;  /* tomorrow */
    }
    return (uint64_t)delta * 1000000ULL;
}

esp_err_t sleep_manager_enter(int refresh_hour) {
    /* ---- 1. Stop WiFi so it doesn't keep drawing current in sleep ---- */
    wifi_manager_disconnect();

    /* ---- 2. Timer wakeup ---- */
    struct timeval tv;
    uint64_t timer_us = 0;
    if (gettimeofday(&tv, NULL) == 0 && tv.tv_sec > 1000000000UL) {
        /* RTC has a plausible time base (post-2001) — schedule to refresh_hour */
        timer_us = sleep_manager_us_until_next_hour((uint32_t)tv.tv_sec, refresh_hour);
        ESP_LOGI(TAG, "Timer wakeup in %.1f hours (refresh_hour=%d)",
                 (double)timer_us / 3600e6, refresh_hour);
    } else {
        /* No time base — fall back to a fixed 24h timer */
        timer_us = 24ULL * 3600ULL * 1000000ULL;
        ESP_LOGW(TAG, "No RTC time base — falling back to fixed 24h timer");
    }
    esp_sleep_enable_timer_wakeup(timer_us);

    /* ---- 3. GPIO48 wakeup (button, active low) ---- */
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << SLEEP_BUTTON_GPIO),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    gpio_wakeup_enable(SLEEP_BUTTON_GPIO, GPIO_INTR_LOW_LEVEL);
    esp_sleep_enable_gpio_wakeup();

    /* Keep GPIO48 pulled up during light sleep.  The default sleep config
       isolates GPIOs (drops internal pull-ups), which leaves the button pin
       floating and falsely triggers a LOW_LEVEL wakeup. */
    gpio_sleep_set_direction(SLEEP_BUTTON_GPIO, GPIO_MODE_INPUT);
    gpio_sleep_set_pull_mode(SLEEP_BUTTON_GPIO, GPIO_PULLUP_ONLY);
    gpio_sleep_sel_en(SLEEP_BUTTON_GPIO);

    /* ---- 4. Let the log flush before sleeping ---- */
    vTaskDelay(pdMS_TO_TICKS(100));

    ESP_LOGI(TAG, "GPIO48 level before sleep = %d", gpio_get_level(SLEEP_BUTTON_GPIO));
    ESP_LOGI(TAG, "Entering light sleep...");
    esp_light_sleep_start();

    /* Wakeup happened — read the real cause NOW (s_light_sleep_wakeup is still
       set at this point), stash it in NVS, then reboot clean. */
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    sleep_wake_cause_t wc;
    if (cause == ESP_SLEEP_WAKEUP_GPIO) {
        wc = SLEEP_WAKE_GPIO;
    } else if (cause == ESP_SLEEP_WAKEUP_TIMER) {
        wc = SLEEP_WAKE_TIMER;
    } else {
        wc = SLEEP_WAKE_NONE;
    }

    /* Persist to NVS (flash) — RTC_DATA_ATTR does not survive esp_restart(). */
    nvs_handle_t h;
    if (nvs_open(WAKE_CAUSE_NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, WAKE_CAUSE_NVS_KEY, (uint8_t)wc);
        nvs_commit(h);
        nvs_close(h);
    }

    ESP_LOGW(TAG, "Woke from light sleep (cause=%d) — restarting clean", (int)cause);
    esp_restart();
    return ESP_OK;   /* unreachable */
}
