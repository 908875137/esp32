/*
 * text_renderer.h
 *
 * FreeType Chinese text overlay on RGBA framebuffer.
 *
 * Font is stored in SPIFFS at TEXT_FONT_PATH so FreeType reads it from
 * flash on demand — no 8MB in-memory load.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

/* ---- font path (SPIFFS) ---- */
#define TEXT_FONT_PATH      "/spiffs/fonts/NotoSansCJKsc-Subset.otf"

/* ---- layout defaults (tunable) ---- */
#define TEXT_QUOTE_SIZE      32
#define TEXT_SOURCE_SIZE     18
#define TEXT_MARGIN_X        40
#define TEXT_BOTTOM_MARGIN   24
#define TEXT_LINE_SPACING    8
#define TEXT_SOURCE_GAP      10
#define TEXT_AREA_Y_START    380
#define TEXT_AREA_Y_END      480

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------
 * Initialisation
 * ------------------------------------------------------------------ */

/*
 * Mount SPIFFS and initialise FreeType.
 * Call once at startup.  Must be called before any render function.
 */
esp_err_t text_renderer_init(void);

/*
 * Free all FreeType resources.  SPIFFS remains mounted.
 */
void text_renderer_deinit(void);

/* ------------------------------------------------------------------
 * Rendering
 * ------------------------------------------------------------------ */

/*
 * Render `quote` and `source` directly into an existing RGBA8 framebuffer.
 *
 *   rgba     — RGBA8 pixel data (W × H × 4), modified in-place
 *   W, H     — framebuffer dimensions (854 × 512)
 *
 * The renderer automatically:
 *   - detects background luminance to choose BLACK or WHITE text
 *   - wraps long lines based on glyph widths
 *   - places text at the bottom of the framebuffer
 *
 * Memory: only temporary glyph bitmap buffers are allocated (from DRAM or
 * PSRAM depending on size).  All are freed before returning.
 */
esp_err_t text_render_overlay(uint8_t *rgba, int W, int H,
                              const char *quote,
                              const char *source);

#ifdef __cplusplus
}
#endif
