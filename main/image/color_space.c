/*
 * color_space.c
 *
 * OKLab color space + weighted nearest-color matching.
 * Ported from the Rust reference (single-precision f32 for the ESP32-S3 FPU).
 */
#include <math.h>
#include "color_space.h"

/* ---- Ottosson's combined matrices (fold sRGB→XYZ→LMS into one) ---- */

/* Linear sRGB → LMS */
static const float M_RGB_LMS[3][3] = {
    {0.4122214708f, 0.5363325363f, 0.0514459929f},
    {0.2119034982f, 0.6806995451f, 0.1073969566f},
    {0.0883024619f, 0.2817188376f, 0.6299787005f},
};

/* cbrt(LMS) → OKLab */
static const float M2[3][3] = {
    {0.2104542553f,  0.7936177850f, -0.0040720468f},
    {1.9779984951f, -2.4285922050f,  0.4505937099f},
    {0.0259040371f,  0.7827717662f, -0.8086757660f},
};

/* Chromatic-axis weight for match_pixel_oklab (see header). */
#define WAB  1.5f

/* ---- sRGB → linear LUT ---- */

static float s_lut[256];
static int s_lut_ready = 0;

static float srgb_fraction_to_linear(float v) {
    if (v <= 0.04045f) {
        return v / 12.92f;
    }
    return powf((v + 0.055f) / 1.055f, 2.4f);
}

void color_space_build_lut(void) {
    if (s_lut_ready) return;
    for (int i = 0; i < 256; i++) {
        s_lut[i] = srgb_fraction_to_linear((float)i / 255.0f);
    }
    s_lut_ready = 1;
}

float color_space_lut_value(uint8_t v) {
    return s_lut[v];
}

/* ---- linear RGB → OKLab ---- */

oklab_t rgb_linear_to_oklab(float r, float g, float b) {
    float l = M_RGB_LMS[0][0]*r + M_RGB_LMS[0][1]*g + M_RGB_LMS[0][2]*b;
    float m = M_RGB_LMS[1][0]*r + M_RGB_LMS[1][1]*g + M_RGB_LMS[1][2]*b;
    float s = M_RGB_LMS[2][0]*r + M_RGB_LMS[2][1]*g + M_RGB_LMS[2][2]*b;

    float l_ = cbrtf(l);
    float m_ = cbrtf(m);
    float s_ = cbrtf(s);

    oklab_t out;
    out.l = M2[0][0]*l_ + M2[0][1]*m_ + M2[0][2]*s_;
    out.a = M2[1][0]*l_ + M2[1][1]*m_ + M2[1][2]*s_;
    out.b = M2[2][0]*l_ + M2[2][1]*m_ + M2[2][2]*s_;
    return out;
}

/* ---- weighted nearest palette color ---- */

int match_pixel_oklab(oklab_t pixel, const oklab_t *palette, int palette_len) {
    int best_idx = 0;
    float best_dist = 1e30f;
    const float wab_sq = WAB * WAB;

    for (int i = 0; i < palette_len; i++) {
        float dl = pixel.l - palette[i].l;
        float da = pixel.a - palette[i].a;
        float db = pixel.b - palette[i].b;
        float dist = dl*dl + wab_sq * (da*da + db*db);
        if (dist < best_dist) {
            best_dist = dist;
            best_idx = i;
        }
    }
    return best_idx;
}
