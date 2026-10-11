/*
 * image_decoder.h
 *
 * Unified image input layer:
 *   URL  → download PNG → decode → crop → resize → RGBA8
 *   SD   → fread PNG    → decode → crop → resize → RGBA8
 *
 * Both paths share the same lodepng decode + center-crop + bilinear-resize
 * core, and both output a TARGET_W×TARGET_H RGBA8 framebuffer ready for
 * preprocessing, text overlay and RGB565 conversion.
 *
 * Portrait images (taller than wide) are rotated 90° to fill the
 * landscape screen.
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
 * Download a PNG from `url`, decode, center-crop, bilinear-scale to
 * 800×480, return an RGBA8 framebuffer.
 *
 * Caller MUST free *rgba_out with free().
 */
esp_err_t image_download_and_decode_rgba(const char *url,
                                          uint8_t **rgba_out,
                                          image_info_t *info);

/*
 * Read a PNG from a local file path (e.g. /sdcard/images/001.png),
 * decode, center-crop, bilinear-scale to 800×480, return RGBA8.
 *
 * Shares the same decode core as the URL path — only the byte source
 * differs.  The file is read into PSRAM then freed before decoding.
 *
 * Caller MUST free *rgba_out with free().
 */
esp_err_t image_file_decode_rgba(const char *path,
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

/*
 * Rotate an RGBA8 framebuffer 180° in place.
 *
 * The panel is mounted upside-down, so the whole frame (image + overlaid
 * text) must be flipped before dithering.  In-place, no extra buffer.
 */
void image_rgba_rotate_180(uint8_t *rgba, int W, int H);
