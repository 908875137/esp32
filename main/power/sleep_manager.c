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
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "sleep_manager.h"
#include "wifi_manager.h"

static const char *TAG = "sleep";

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

    /* ---- 4. Let the log flush before sleeping ---- */
    vTaskDelay(pdMS_TO_TICKS(100));

    ESP_LOGI(TAG, "Entering light sleep...");
    esp_light_sleep_start();

    /* We only get here if sleep was entered and returned (e.g. wakeup),
       or if sleep setup failed.  Reboot clean either way. */
    ESP_LOGW(TAG, "Woke from light sleep — restarting clean");
    esp_restart();
    return ESP_OK;   /* unreachable */
}
