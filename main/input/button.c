/*
 * button.c
 *
 * Button polling task.
 *
 * Long press detection:
 *   - Poll every 100ms
 *   - If pressed (active low) for ≥ LONG_PRESS_TICKS * 100ms → callback fires once
 *   - Callback fires on release after long press
 */
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "button.h"

static const char *TAG = "button";

#define POLL_PERIOD_MS      100
#define LONG_PRESS_TICKS    (3000 / POLL_PERIOD_MS)  /* 3 seconds */

typedef struct {
    int                     pin;
    button_long_press_cb_t  callback;
    int                     press_ticks;
    bool                    long_fired;  /* prevent repeated fire */
} button_ctx_t;

static button_ctx_t s_btn = {0};

static void button_task(void *arg) {
    button_ctx_t *ctx = (button_ctx_t *)arg;
    int press_ticks = 0;
    bool long_fired = false;

    ESP_LOGI(TAG, "Button task started, polling GPIO%d every %d ms", ctx->pin, POLL_PERIOD_MS);

    while (1) {
        int level = gpio_get_level(ctx->pin);
        if (level == 0) {  /* active low — button pressed */
            press_ticks++;
            if (press_ticks >= LONG_PRESS_TICKS && !long_fired) {
                long_fired = true;
                ESP_LOGI(TAG, "Long press detected! (%d ticks)", press_ticks);
                if (ctx->callback) {
                    ctx->callback();
                }
            }
        } else {  /* released */
            if (press_ticks > 0) {
                if (press_ticks >= LONG_PRESS_TICKS) {
                    ESP_LOGI(TAG, "Released after long press (%d ticks)", press_ticks);
                } else {
                    ESP_LOGI(TAG, "Short press (%d ticks), ignored", press_ticks);
                }
            }
            press_ticks = 0;
            long_fired = false;
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_PERIOD_MS));
    }
}

esp_err_t button_init(int pin, button_long_press_cb_t cb) {
    /* Configure GPIO */
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << pin),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);

    s_btn.pin       = pin;
    s_btn.callback  = cb;
    s_btn.press_ticks = 0;
    s_btn.long_fired  = false;

    /* Create polling task */
    BaseType_t ret = xTaskCreate(
        button_task,
        "btn_task",
        2048,
        &s_btn,
        5,   /* priority — low */
        NULL
    );
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create button task");
        return ESP_FAIL;
    }

    return ESP_OK;
}
