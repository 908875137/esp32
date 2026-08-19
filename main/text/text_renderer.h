/*
 * text_renderer.h
 *
 * FreeType Chinese text overlay on an RGBA8 framebuffer.
 *
 * The font is loaded from a filesystem path (typically on the SD card,
 * e.g. /sdcard/fonts/NotoSansSC-Regular.ttf).  FreeType opens the file
 * by path and only glyph bitmaps enter RAM — the font file itself stays
 * on the storage medium.
 *
 * Tweaking layout — just edit the #defines.  No magic constants in .c.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

/* Default font path (overridable at init time / via config.json) */
#define TEXT_FONT_PATH_DEFAULT   "/sdcard/fonts/NotoSansCJKsc-Subset.otf"

/* ---- layout defaults (tunable) ---- */
#define TEXT_QUOTE_SIZE      40
#define TEXT_SOURCE_SIZE     21
#define TEXT_MARGIN_X        48
#define TEXT_BOTTOM_MARGIN   14
#define TEXT_LINE_SPACING    9
#define TEXT_SOURCE_GAP      14
#define TEXT_AREA_Y_START    300
#define TEXT_AREA_Y_END      480

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------
 * Initialisation
 * ------------------------------------------------------------------ */

/*
 * Initialise FreeType and open `font_path` (a filesystem path, e.g. on
 * the SD card).  Pass NULL to use TEXT_FONT_PATH_DEFAULT.
 *
 * The SD card must already be mounted when this is called, and must stay
 * mounted for the lifetime of the app.
 *
 * Returns ESP_OK on success.
 */
esp_err_t text_renderer_init(const char *font_path);

/*
 * Free all FreeType resources.
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
