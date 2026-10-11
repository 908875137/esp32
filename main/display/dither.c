/*
 * dither.c — Burkes error diffusion + OKLab matching for UC8179 tri-color EPD.
 *
 * Ported from epaper-dithering-main (Rust), single-precision float.
 *
 * Pipeline per pixel:
 *   RGB565 → 8-bit sRGB → + accumulated error → clamp
 *   → linear (LUT) → OKLab → match_pixel_oklab (WAB=1.5) → nearest ink
 *   → pack bit into BW (black) or RED (red) bitmap
 *   → quantization error in sRGB space → diffuse via Burkes (serpentine)
 *
 * Memory: two sliding error rows (~19 KB), no full-frame float buffer.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "dither.h"
#include "color_space.h"

/* ---- Measured BWR palette (sRGB) ----
 *   black = ( 34,  34,  34)
 *   white = (158, 163, 166)
 *   red   = (118,  25,  18)
 */
#define PALETTE_N  3
static const uint8_t PAL_SRGB[PALETTE_N][3] = {
    { 34,  34,  34},   /* 0 = black */
    {158, 163, 166},   /* 1 = white */
    {118,  25,  18},   /* 2 = red (accent) */
};

/* Burkes kernel: {dx, dy, weight} */
typedef struct { int dx, dy; float w; } kern_t;
static const kern_t BURKES[] = {
    { 1, 0, 8.0f/32.0f},
    { 2, 0, 4.0f/32.0f},
    {-2, 1, 2.0f/32.0f},
    {-1, 1, 4.0f/32.0f},
    { 0, 1, 8.0f/32.0f},
    { 1, 1, 4.0f/32.0f},
    { 2, 1, 2.0f/32.0f},
};
#define BURKES_N  (sizeof(BURKES) / sizeof(BURKES[0]))

/* Row error buffer width: W + 4 so kernel dx∈[-2,2] never goes out of bounds. */
#define ROW_W  (PANEL_WIDTH + 4)
#define ROW_OFFSET 2   /* index 0..W+3 maps to x-2 .. x+1, safe */

static oklab_t s_pal_lab[PALETTE_N];

static void build_palette_lab(void) {
    for (int i = 0; i < PALETTE_N; i++) {
        float r = color_space_lut_value(PAL_SRGB[i][0]);
        float g = color_space_lut_value(PAL_SRGB[i][1]);
        float b = color_space_lut_value(PAL_SRGB[i][2]);
        s_pal_lab[i] = rgb_linear_to_oklab(r, g, b);
    }
}

static inline void set_pixel(uint8_t *bw, uint8_t *red, int x, int y, int idx) {
    int byte_idx = y * (PANEL_WIDTH / 8) + x / 8;
    int bit_pos  = 7 - (x % 8);
    if (idx == 0) bw[byte_idx]  |= (1 << bit_pos);      /* black */
    else if (idx == 2) red[byte_idx] |= (1 << bit_pos); /* red */
    /* white (1) → no bit */
}

void dither_rgb565_to_epd(const uint8_t *rgb565,
                          uint8_t *bw_buffer,
                          uint8_t *red_buffer) {
    memset(bw_buffer, 0, BUFFER_SIZE);
    memset(red_buffer, 0, BUFFER_SIZE);

    color_space_build_lut();
    build_palette_lab();

    /* Two sliding error rows (current + next). */
    float *err0 = (float *)calloc(ROW_W * 3, sizeof(float));  /* row y   */
    float *err1 = (float *)calloc(ROW_W * 3, sizeof(float));  /* row y+1 */
    if (!err0 || !err1) {
        free(err0); free(err1);
        printf("ERROR: dither alloc failed\n");
        return;
    }

    const int W = PANEL_WIDTH;
    const int H = PANEL_HEIGHT;

    for (int y = 0; y < H; y++) {
        bool reverse = (y & 1) != 0;   /* serpentine: odd rows scan right-to-left */

        for (int xi = 0; xi < W; xi++) {
            int x = reverse ? (W - 1 - xi) : xi;
            int eidx = (x + ROW_OFFSET) * 3;

            /* ---- decode RGB565 → 8-bit ---- */
            int pidx = (y * W + x) * 2;
            uint16_t v = (uint16_t)(rgb565[pidx] | (rgb565[pidx + 1] << 8));
            int r5 = (v >> 11) & 0x1F;
            int g6 = (v >> 5)  & 0x3F;
            int b5 =  v        & 0x1F;

            /* ---- working sRGB (with accumulated error) ---- */
            float wr = (float)r5 * 255.0f / 31.0f + err0[eidx + 0];
            float wg = (float)g6 * 255.0f / 63.0f + err0[eidx + 1];
            float wb = (float)b5 * 255.0f / 31.0f + err0[eidx + 2];
            err0[eidx + 0] = 0.0f;
            err0[eidx + 1] = 0.0f;
            err0[eidx + 2] = 0.0f;

            /* clamp to [0,255] */
            int ir = (int)(wr + 0.5f); if (ir < 0) ir = 0; if (ir > 255) ir = 255;
            int ig = (int)(wg + 0.5f); if (ig < 0) ig = 0; if (ig > 255) ig = 255;
            int ib = (int)(wb + 0.5f); if (ib < 0) ib = 0; if (ib > 255) ib = 255;

            /* ---- OKLab match ---- */
            oklab_t lab = rgb_linear_to_oklab(
                color_space_lut_value((uint8_t)ir),
                color_space_lut_value((uint8_t)ig),
                color_space_lut_value((uint8_t)ib));
            int best = match_pixel_oklab(lab, s_pal_lab, PALETTE_N);

            set_pixel(bw_buffer, red_buffer, x, y, best);

            /* ---- quantization error in sRGB space ---- */
            float er = (float)ir - (float)PAL_SRGB[best][0];
            float eg = (float)ig - (float)PAL_SRGB[best][1];
            float eb = (float)ib - (float)PAL_SRGB[best][2];

            /* ---- diffuse via Burkes (mirror dx on reverse scan) ---- */
            for (size_t k = 0; k < BURKES_N; k++) {
                int dx = reverse ? -BURKES[k].dx : BURKES[k].dx;
                int dy = BURKES[k].dy;
                float w = BURKES[k].w;
                int nx = x + dx;
                if (nx < 0 || nx >= W) continue;
                float *target = (dy == 0) ? err0 : err1;
                int ti = (nx + ROW_OFFSET) * 3;
                target[ti + 0] += er * w;
                target[ti + 1] += eg * w;
                target[ti + 2] += eb * w;
            }
        }

        /* Feed watchdog every 4 rows */
        if ((y & 0x03) == 0) vTaskDelay(1);

        /* Rotate error rows: err0 ← err1, err1 ← zeros */
        float *t = err0; err0 = err1; err1 = t;
        memset(err1, 0, ROW_W * 3 * sizeof(float));
    }

    free(err0);
    free(err1);
}
