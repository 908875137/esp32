/*
 * image_decoder.h
 *
 * Download PNG → Decode → Center-crop → RGB565 → return for dithering.
 *
 * Uses lodepng (single-file, no dependencies) for PNG decoding.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

/*
 * Download PNG from `url`, decode, center-crop to PANEL_WIDTH×PANEL_HEIGHT,
 * output RGB565 buffer (800×480×2 = 768000 bytes).
 *
 * Caller MUST free *rgb565_out with free().
 *
 * Returns ESP_OK on success.
 */
esp_err_t image_download_and_decode(const char *url,
                                    uint8_t **rgb565_out);
