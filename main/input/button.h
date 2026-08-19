/*
 * button.h
 *
 * Hardware button input with three press levels:
 *   - short press      (< 1.5s)  → e.g. next image
 *   - long press       (1.5–5s)  → e.g. toggle AI/SD mode
 *   - very-long press  (≥ 5s)     → e.g. enter WiFi provisioning
 *
 * Uses FreeRTOS task for polling — no ISR-level logic.
 * Button pin: GPIO0 (boot button, active low, internal pull-up).
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*button_short_press_cb_t)(void);
typedef void (*button_long_press_cb_t)(void);
typedef void (*button_very_long_press_cb_t)(void);

/**
 * Initialise the button GPIO and start the polling task.
 *
 * @param pin            GPIO number
 * @param short_cb       called on short press (optional, may be NULL)
 * @param long_cb        called on long press  (optional, may be NULL)
 * @param very_long_cb   called on very-long press (optional, may be NULL)
 */
esp_err_t button_init(int pin,
                      button_short_press_cb_t short_cb,
                      button_long_press_cb_t long_cb,
                      button_very_long_press_cb_t very_long_cb);

#ifdef __cplusplus
}
#endif
