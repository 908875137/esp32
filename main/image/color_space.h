/*
 * color_space.h
 *
 * OKLab perceptual color space + nearest-color matching for e-paper dithering.
 *
 * Ported from the Rust reference (epaper-dithering-main, color_space.rs +
 * color_space_lab.rs), using single-precision float — the ESP32-S3 FPU is
 * single-precision, so f32 matches the hardware and runs much faster than f64.
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- OKLab color ---- */
typedef struct {
    float l;
    float a;
    float b;
} oklab_t;

/*
 * Convert sRGB [0,255] → linear [0,1] (IEC 61966-2-1 gamma).
 * A 256-entry LUT is built once and used in the hot path to avoid powf per pixel.
 */
void color_space_build_lut(void);

/*
 * Read the sRGB→linear LUT for a byte value.  Call color_space_build_lut()
 * once before use.  Returns linear [0,1].
 */
float color_space_lut_value(uint8_t v);

/*
 * Convert linear RGB [0,1] → OKLab (Ottosson's combined matrix).
 */
oklab_t rgb_linear_to_oklab(float r, float g, float b);

/*
 * Weighted Cartesian OKLab distance to the nearest palette color.
 * Returns the palette index.
 *
 *   dist² = dL² + wab²·(da² + db²),  wab = 1.5
 *
 * wab > 1 boosts chromatic axes relative to lightness, so saturated colors
 * map to the red ink instead of collapsing to black/white. (Tuned by the
 * reference project's regression fixture set.)
 */
int match_pixel_oklab(oklab_t pixel, const oklab_t *palette, int palette_len);

#ifdef __cplusplus
}
#endif
