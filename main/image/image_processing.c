/*
 * image_processing.c
 *
 * Pre-processing node chain on the RGBA8 framebuffer, applied before
 * dithering.  Each node is a separable, order-independent filter that
 * operates on linear-ish float RGB and writes back to the buffer.
 *
 * Fixed defaults (Phase 1):
 *   Exposure   1.15
 *   Contrast   1.10
 *   Saturation 1.05
 *   Shadows   +0.10
 *   Highlights -0.10
 *   Tone + gamut → auto (luminance-driven lift/compress, no manual params)
 */
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "image_processing.h"

static const char *TAG = "proc";

#define EXPOSURE   1.0f
#define CONTRAST   1.0f
#define SATURATION 1.0f
#define SHADOWS    0.0f
#define HIGHLIGHTS 0.0f

static inline float clampf(float v) {
    if (v < 0.0f) return 0.0f;
    if (v > 1.0f) return 1.0f;
    return v;
}

/* ---- Node: exposure (multiplicative in linear space) ---- */
static void node_exposure(float *r, float *g, float *b) {
    *r = clampf(*r * EXPOSURE);
    *g = clampf(*g * EXPOSURE);
    *b = clampf(*b * EXPOSURE);
}

/* ---- Node: contrast (pivot around 0.5) ---- */
static void node_contrast(float *r, float *g, float *b) {
    *r = clampf((*r - 0.5f) * CONTRAST + 0.5f);
    *g = clampf((*g - 0.5f) * CONTRAST + 0.5f);
    *b = clampf((*b - 0.5f) * CONTRAST + 0.5f);
}

/* ---- Node: saturation (luminance-preserving mix toward luma) ---- */
static void node_saturation(float *r, float *g, float *b) {
    float luma = 0.299f * *r + 0.587f * *g + 0.114f * *b;
    *r = clampf(luma + (*r - luma) * SATURATION);
    *g = clampf(luma + (*g - luma) * SATURATION);
    *b = clampf(luma + (*b - luma) * SATURATION);
}

/* ---- Node: shadows / highlights (lift dark, tame bright) ---- */
static void node_shadows_highlights(float *r, float *g, float *b) {
    /* shadows: raise the low end via a smooth curve; highlights: compress top */
    float s_r = *r + SHADOWS * (1.0f - *r) * (1.0f - *r);
    float s_g = *g + SHADOWS * (1.0f - *g) * (1.0f - *g);
    float s_b = *b + SHADOWS * (1.0f - *b) * (1.0f - *b);

    *r = clampf(s_r + HIGHLIGHTS * (*r) * (*r));
    *g = clampf(s_g + HIGHLIGHTS * (*g) * (*g));
    *b = clampf(s_b + HIGHLIGHTS * (*b) * (*b));
}

/* ---- Node: tone + gamut (DISABLED — no-op, kept for structure) ---- */
static void node_tone_gamut(float *r, float *g, float *b) {
    (void)r; (void)g; (void)b;
}

esp_err_t image_apply_processing(uint8_t *rgba, int W, int H) {
    if (!rgba || W <= 0 || H <= 0) return ESP_ERR_INVALID_ARG;

    ESP_LOGI(TAG, "Applying preprocessing chain (exposure/contrast/sat/...): %d×%d", W, H);

    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            int idx = (y * W + x) * 4;
            float r = rgba[idx + 0] / 255.0f;
            float g = rgba[idx + 1] / 255.0f;
            float b = rgba[idx + 2] / 255.0f;

            node_exposure(&r, &g, &b);
            node_contrast(&r, &g, &b);
            node_saturation(&r, &g, &b);
            node_shadows_highlights(&r, &g, &b);
            node_tone_gamut(&r, &g, &b);

            rgba[idx + 0] = (uint8_t)(clampf(r) * 255.0f + 0.5f);
            rgba[idx + 1] = (uint8_t)(clampf(g) * 255.0f + 0.5f);
            rgba[idx + 2] = (uint8_t)(clampf(b) * 255.0f + 0.5f);
            /* alpha untouched */
        }
        if ((y & 0x0F) == 0) vTaskDelay(1);  /* feed watchdog */
    }

    ESP_LOGI(TAG, "Preprocessing complete");
    return ESP_OK;
}
