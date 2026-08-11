/*
 * dither.c
 *
 * RGB565 → 3-color Stucki dither for UC8179 tri-color EPD.
 *
 * Memory: ~57 KB (three rows of float error buffers — no full-frame allocation).
 * PSRAM is NOT required.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "dither.h"

/* ============================================================
 * Pixel types
 * ============================================================ */

typedef enum {
    PIXEL_WHITE = 0,
    PIXEL_BLACK = 1,
    PIXEL_RED   = 2
} epd_color_t;

/* ============================================================
 * Utility
 * ============================================================ */

static inline float clamp_float(float v, float lo, float hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static inline int clamp_int(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

/* ============================================================
 * RGB -> HSV  (inline helper — used by red_candidate)
 * ============================================================ */

static void rgb_to_hsv(uint8_t r8, uint8_t g8, uint8_t b8,
                       float *h, float *s, float *v) {
    float r = r8 / 255.0f;
    float g = g8 / 255.0f;
    float b = b8 / 255.0f;
    float maxv = fmaxf(r, fmaxf(g, b));
    float minv = fminf(r, fminf(g, b));
    float delta = maxv - minv;
    *v = maxv;
    if (maxv <= 0.00001f) { *s = 0.0f; *h = 0.0f; return; }
    *s = delta / maxv;
    if (delta <= 0.00001f) { *h = 0.0f; return; }
    if (maxv == r) {
        *h = 60.0f * fmodf((g - b) / delta, 6.0f);
        if (*h < 0) *h += 360.0f;
    } else if (maxv == g) {
        *h = 60.0f * ((b - r) / delta + 2.0f);
    } else {
        *h = 60.0f * ((r - g) / delta + 4.0f);
    }
}

/* ============================================================
 * Red candidate check
 * ============================================================ */

static bool red_candidate(uint8_t r, uint8_t g, uint8_t b) {
    if (r < RED_MIN_R) return false;
    if ((int)r < (int)g + RED_MARGIN) return false;
    if ((int)r < (int)b + RED_MARGIN) return false;
    float h, s, v;
    rgb_to_hsv(r, g, b, &h, &s, &v);
    if (s < RED_SATURATION_MIN) return false;
    if (!(h <= 25.0f || h >= 335.0f)) return false;
    return true;
}

/* ============================================================
 * Color distance  (weighted perceptual)
 * ============================================================ */

static float color_dist(uint8_t r, uint8_t g, uint8_t b,
                         uint8_t tr, uint8_t tg, uint8_t tb) {
    float dr = (float)r - (float)tr;
    float dg = (float)g - (float)tg;
    float db = (float)b - (float)tb;
    return WEIGHT_R * dr * dr + WEIGHT_G * dg * dg + WEIGHT_B * db * db;
}

/* ============================================================
 * Quantize (float input — preserves diffused error)
 * ============================================================ */

static epd_color_t quantize_color(float r, float g, float b) {
    int ir = clamp_int((int)(r + 0.5f), 0, 255);
    int ig = clamp_int((int)(g + 0.5f), 0, 255);
    int ib = clamp_int((int)(b + 0.5f), 0, 255);

    float dist_black = color_dist((uint8_t)ir, (uint8_t)ig, (uint8_t)ib, 0, 0, 0);
    float dist_white = color_dist((uint8_t)ir, (uint8_t)ig, (uint8_t)ib, 255, 255, 255);

    float dist_red;
    if (red_candidate((uint8_t)ir, (uint8_t)ig, (uint8_t)ib)) {
        dist_red = color_dist((uint8_t)ir, (uint8_t)ig, (uint8_t)ib, 255, 0, 0)
                   + RED_PENALTY * RED_PENALTY;
    } else {
        dist_red = 1e30f;
    }

    if (dist_red < dist_black && dist_red < dist_white)
        return PIXEL_RED;
    if (dist_black < dist_white)
        return PIXEL_BLACK;
    return PIXEL_WHITE;
}

/* ============================================================
 * EPD color → RGB  (for computing quantisation error)
 * ============================================================ */

static void color_to_rgb(epd_color_t c, uint8_t *r, uint8_t *g, uint8_t *b) {
    switch (c) {
    case PIXEL_BLACK: *r = 0;   *g = 0;   *b = 0;   break;
    case PIXEL_RED:   *r = 255; *g = 0;   *b = 0;   break;
    default:          *r = 255; *g = 255; *b = 255; break;
    }
}

/* ============================================================
 * Set pixel in BW / RED bitmaps
 * ============================================================ */

static void set_pixel(uint8_t *bw, uint8_t *red, int x, int y, epd_color_t c) {
    int byte_idx = y * (PANEL_WIDTH / 8) + x / 8;
    int bit_pos  = 7 - (x % 8);
    if (c == PIXEL_BLACK) bw[byte_idx]  |= (1 << bit_pos);
    if (c == PIXEL_RED)   red[byte_idx] |= (1 << bit_pos);
}

/* ============================================================
 * Stucki kernel (sliding-window, 3-row float buffers)
 *
 *        X    8   4
 *   2    4    8   4   2
 *   1    2    4   2   1
 *
 * denominator = 42
 *
 * We keep THREE float rows (row 0 = current, row 1 = y+1, row 2 = y+2).
 * Row width is W+4 floats so kernel offsets x±2 are always valid.
 * Total memory: 3 × (800+4) × 4 bytes × 3 channels ≈ 29 KB.
 * ============================================================ */

#define ROW_W  ((PANEL_WIDTH) + 4)

static inline void add_err_row(float *row, int x, float e) {
    row[x + 2] += e;                       /* offset by +2 so index 0..W+3 is always safe */
}

static void stucki_diffuse(float *c0, float *c1, float *c2,          /* one colour channel */
                           int W, int x, float e) {
    /* row +0 */
    add_err_row(c0, x + 1, e *  8.0f / 42.0f);
    add_err_row(c0, x + 2, e *  4.0f / 42.0f);

    /* row +1 */
    add_err_row(c1, x - 2, e *  2.0f / 42.0f);
    add_err_row(c1, x - 1, e *  4.0f / 42.0f);
    add_err_row(c1, x,     e *  8.0f / 42.0f);
    add_err_row(c1, x + 1, e *  4.0f / 42.0f);
    add_err_row(c1, x + 2, e *  2.0f / 42.0f);

    /* row +2 */
    add_err_row(c2, x - 2, e *  1.0f / 42.0f);
    add_err_row(c2, x - 1, e *  2.0f / 42.0f);
    add_err_row(c2, x,     e *  4.0f / 42.0f);
    add_err_row(c2, x + 1, e *  2.0f / 42.0f);
    add_err_row(c2, x + 2, e *  1.0f / 42.0f);
}

/* ============================================================
 * Main entry point
 * ============================================================ */

void dither_rgb565_to_epd(const uint8_t *rgb565,
                          uint8_t *bw_buffer,
                          uint8_t *red_buffer) {
    memset(bw_buffer,  0, BUFFER_SIZE);
    memset(red_buffer, 0, BUFFER_SIZE);

    const int W = PANEL_WIDTH;
    const int H = PANEL_HEIGHT;

    /* 3 rows × 3 channels × (W+4) floats  ≈ 29 KB  (no PSRAM required) */
    float *r0 = (float *)calloc(ROW_W * 3, sizeof(float));   /* row y+0 */
    float *r1 = (float *)calloc(ROW_W * 3, sizeof(float));   /* row y+1 */
    float *r2 = (float *)calloc(ROW_W * 3, sizeof(float));   /* row y+2 */

    if (!r0 || !r1 || !r2) {
        printf("ERROR: dither alloc failed\n");
        free(r0); free(r1); free(r2);
        return;
    }

    for (int y = 0; y < H; y++) {
        /* clear row y+2 */
        memset(r2, 0, ROW_W * 3 * sizeof(float));

        /* Feed watchdog every row (800 pixels × 3 channels × Stucki = 5 sec total @ 240 MHz) */
        if ((y & 0x0F) == 0) {
            vTaskDelay(1);
        }

        for (int x = 0; x < W; x++) {
            /* ---- Decode RGB565 ---- */
            int idx = (y * W + x) * 2;
            uint16_t v = rgb565[idx] | (rgb565[idx + 1] << 8);
            int r5 = (v >> 11) & 0x1F;
            int g6 = (v >> 5)  & 0x3F;
            int b5 =  v        & 0x1F;

            /* ---- Luminance contrast ---- */
            float r8f = (float)r5 * 255.0f / 31.0f;
            float g8f = (float)g6 * 255.0f / 63.0f;
            float b8f = (float)b5 * 255.0f / 31.0f;
            float lum = 0.299f * r8f + 0.587f * g8f + 0.114f * b8f;
            float adj = (lum - 128.0f) * IMAGE_CONTRAST + 128.0f;
            float sc  = (lum > 0.001f) ? adj / lum : 1.0f;
            int r8 = clamp_int((int)(r8f * sc), 0, 255);
            int g8 = clamp_int((int)(g8f * sc), 0, 255);
            int b8 = clamp_int((int)(b8f * sc), 0, 255);

            /* ---- Apply accumulated error ---- */
            float rf = clamp_float((float)r8 + r0[ROW_W * 0 + x + 2], 0.0f, 255.0f);
            float gf = clamp_float((float)g8 + r0[ROW_W * 1 + x + 2], 0.0f, 255.0f);
            float bf = clamp_float((float)b8 + r0[ROW_W * 2 + x + 2], 0.0f, 255.0f);

            epd_color_t result = quantize_color(rf, gf, bf);
            set_pixel(bw_buffer, red_buffer, x, y, result);

            uint8_t qr, qg, qb;
            color_to_rgb(result, &qr, &qg, &qb);

            float er = rf - (float)qr;
            float eg = gf - (float)qg;
            float eb = bf - (float)qb;

            if (result == PIXEL_RED) {
                er *= RED_ERROR_DIFFUSION_SCALE;
                eg *= RED_ERROR_DIFFUSION_SCALE;
                eb *= RED_ERROR_DIFFUSION_SCALE;
            }

            stucki_diffuse(r0 + ROW_W * 0, r1 + ROW_W * 0, r2 + ROW_W * 0, W, x, er);
            stucki_diffuse(r0 + ROW_W * 1, r1 + ROW_W * 1, r2 + ROW_W * 1, W, x, eg);
            stucki_diffuse(r0 + ROW_W * 2, r1 + ROW_W * 2, r2 + ROW_W * 2, W, x, eb);
        }

        /* Rotate row pointers */
        float *t = r0; r0 = r1; r1 = r2; r2 = t;
    }

    free(r0); free(r1); free(r2);
}
