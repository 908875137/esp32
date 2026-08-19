/*
 * button.c
 *
 * Three-level button polling task:
 *   - short press      (< 1.5s)  → short_cb
 *   - long press       (1.5–5s)  → long_cb
 *   - very-long press  (≥ 5s)     → very_long_cb
 *
 * The very-long threshold (5s) deliberately sits above the previous 3s
 * provisioning threshold so the provisioning flow keeps working.
 *
 * Each level fires once per press, on release.
 */
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "button.h"

static const char *TAG = "button";

#define POLL_PERIOD_MS        100
#define SHORT_PRESS_TICKS     (1500 / POLL_PERIOD_MS)  /* 1.5 s */
#define LONG_PRESS_TICKS      (5000 / POLL_PERIOD_MS)  /* 5 s */

typedef struct {
    int                          pin;
    button_short_press_cb_t      short_cb;
    button_long_press_cb_t       long_cb;
    button_very_long_press_cb_t  very_long_cb;
} button_ctx_t;

static button_ctx_t s_btn = {0};

static void button_task(void *arg) {
    button_ctx_t *ctx = (button_ctx_t *)arg;
    int press_ticks = 0;
    bool long_fired = false;
    bool very_long_fired = false;

    ESP_LOGI(TAG, "Button task started, polling GPIO%d every %d ms", ctx->pin, POLL_PERIOD_MS);

    while (1) {
        int level = gpio_get_level(ctx->pin);
        if (level == 0) {  /* active low — pressed */
            press_ticks++;
            if (press_ticks >= LONG_PRESS_TICKS && !very_long_fired) {
                very_long_fired = true;
                ESP_LOGI(TAG, "Very-long press detected (%d ticks)", press_ticks);
                if (ctx->very_long_cb) ctx->very_long_cb();
            }
        } else {  /* released */
            if (press_ticks > 0) {
                if (very_long_fired) {
                    ESP_LOGI(TAG, "Released after very-long press (%d ticks)", press_ticks);
                } else if (press_ticks >= SHORT_PRESS_TICKS) {
                    ESP_LOGI(TAG, "Long press (%d ticks)", press_ticks);
                    if (ctx->long_cb) ctx->long_cb();
                } else {
                    ESP_LOGI(TAG, "Short press (%d ticks)", press_ticks);
                    if (ctx->short_cb) ctx->short_cb();
                }
            }
            press_ticks = 0;
            long_fired = false;
            very_long_fired = false;
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_PERIOD_MS));
    }
}

esp_err_t button_init(int pin,
                      button_short_press_cb_t short_cb,
                      button_long_press_cb_t long_cb,
                      button_very_long_press_cb_t very_long_cb) {
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << pin),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);

    s_btn.pin         = pin;
    s_btn.short_cb    = short_cb;
    s_btn.long_cb     = long_cb;
    s_btn.very_long_cb = very_long_cb;

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
