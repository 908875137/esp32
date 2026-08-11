/*
 * dither.h — Stucki 3-color error-diffusion dither for UC8179 tri-color EPD.
 *
 * Tune IMAGE_CONTRAST / RED_* knobs below to adjust image quality.
 *
 * Pipeline (per pixel):
 *   1. Decode RGB565 → 8-bit RGB
 *   2. Apply luminance contrast stretch
 *   3. Add accumulated Stucki error from previous neighbours
 *   4. Clamp to [0, 255]
 *   5. Red candidate check (HSV + absolute red guards)
 *   6. Colour-distance quantize → {BLACK, WHITE, RED}
 *   7. Error diffusion to 12 neighbours (Stucki kernel, divisor 42)
 *   8. Pack into BW / RED bitmaps
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "epd.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------- user-configurable knobs ---------- */

#define IMAGE_CONTRAST             1.08f   /* luminance stretch; 1.0 = off */
#define RED_MIN_R                  120     /* absolute R floor for red candidate */
#define RED_MARGIN                 25      /* R must dominate G/B by this much */
#define RED_SATURATION_MIN         0.25f   /* HSV saturation floor */
#define RED_PENALTY                25.0f   /* extra distance cost for red */
#define RED_ERROR_DIFFUSION_SCALE  0.50f   /* red error bleed multiplier */

#define WEIGHT_R  0.30f
#define WEIGHT_G  0.59f
#define WEIGHT_B  0.11f

/* -------------------------------------------- */

void dither_rgb565_to_epd(const uint8_t *rgb565,
                          uint8_t *bw_buffer,
                          uint8_t *red_buffer);

#ifdef __cplusplus
}
#endif
