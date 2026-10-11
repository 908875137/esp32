/*
 * dither.h — Burkes error-diffusion + OKLab matching for UC8179 tri-color EPD.
 *
 * Algorithm (ported from epaper-dithering-main):
 *   per pixel:
 *     1. sRGB [0,255] → linear via 256-entry LUT
 *     2. linear RGB → OKLab
 *     3. match_pixel_oklab → nearest ink (weighted Cartesian, WAB=1.5)
 *     4. quantization error computed in sRGB space
 *     5. diffuse error via Burkes kernel (7 entries, serpentine scan)
 *     6. pack into BW / RED bitmaps
 *
 * Palette is the measured BWR ink colors (sRGB), not ideal black/white/red.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "epd.h"

#ifdef __cplusplus
extern "C" {
#endif

void dither_rgb565_to_epd(const uint8_t *rgb565,
                          uint8_t *bw_buffer,
                          uint8_t *red_buffer);

#ifdef __cplusplus
}
#endif
