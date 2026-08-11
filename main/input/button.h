/*
 * button.h
 *
 * Hardware button input with long-press detection.
 * Uses FreeRTOS task for polling — no ISR-level logic.
 *
 * Button pin: GPIO0 (boot button on most ESP32-S3 dev boards).
 *
 * Long press (≥3s): enter WiFi provisioning mode.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Callback type — called when long-press detected */
typedef void (*button_long_press_cb_t)(void);

/**
 * Initialise the button GPIO and start the polling task.
 *
 * @param pin           GPIO number
 * @param cb            Callback invoked on long-press (≥3s)
 */
esp_err_t button_init(int pin, button_long_press_cb_t cb);

#ifdef __cplusplus
}
#endif
