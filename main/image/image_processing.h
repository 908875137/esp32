/*
 * image_processing.h
 *
 * Pre-processing node chain applied to the 800×480 RGBA8 framebuffer
 * after decode/crop/resize, before dithering.
 *
 * Each node is independent and applied in a fixed order:
 *   exposure → contrast → saturation → shadows → highlights → tone → gamut
 *
 * Values are fixed for now (Phase 1); the structure keeps each node
 * separable so they can later be driven by config.json or the web API.
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Apply the full pre-processing chain to an RGBA8 framebuffer (W×H×4).
 * Only RGB channels are modified; alpha is left untouched.
 *
 * Memory: operates in-place, no extra framebuffer allocation.
 * Feed the watchdog periodically — this is a full-frame float pass.
 */
esp_err_t image_apply_processing(uint8_t *rgba, int W, int H);

#ifdef __cplusplus
}
#endif
