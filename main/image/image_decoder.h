/*
 * image_decoder.h
 *
 * Download PNG → Decode → RGBA → (text overlay) → RGB565.
 *
 * Split into two functions so text rendering can happen between decode
 * and RGB565 conversion, on the full-colour RGBA framebuffer.
 *
 * Uses lodepng (single-file, no dependencies) for PNG decoding.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

/* ---- Image info (returned by the decoder) ---- */
typedef struct {
    unsigned width;
    unsigned height;
} image_info_t;

/*
 * Download PNG from `url`, decode, center-crop, bilinear-scale to
 * TARGET_W×TARGET_H, and return an RGBA8 framebuffer.
 *
 * Caller MUST free *rgba_out with free().
 */
esp_err_t image_download_and_decode_rgba(const char *url,
                                          uint8_t **rgba_out,
                                          image_info_t *info);

/*
 * Convert an RGBA8 framebuffer (W×H×4) to RGB565 (W×H×2).
 *
 * `rgba` is NOT freed by this function — caller retains ownership.
 * Caller MUST free *rgb565_out with free().
 */
esp_err_t image_rgba_to_rgb565(const uint8_t *rgba,
                                int W, int H,
                                uint8_t **rgb565_out);
