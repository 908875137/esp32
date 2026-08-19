/*
 * image_decoder.h
 *
 * Unified image input layer:
 *   URL  → download PNG → decode → crop → resize → RGBA8
 *   SD   → fread PNG    → decode → crop → resize → RGBA8
 *
 * Both paths share the same lodepng decode + center-crop + bilinear-resize
 * core, and both output a TARGET_W×TARGET_H RGBA8 framebuffer ready for
 * text overlay and RGB565 conversion.
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
