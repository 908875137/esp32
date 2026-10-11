/*
 * epaper_dithering_reference.c
 *
 * 电子墨水屏抖动算法参考实现（C 版）。
 * 对应头文件 epaper_dithering_reference.h，等价于 Rust 版
 * reference/epaper_dithering_reference.rs。
 *
 * 分层：
 *   1. sRGB ↔ 线性伽马转换
 *   2. OKLab 色彩空间 + 最近色匹配
 *   3. 误差扩散核（权重表）
 *   4. 误差扩散主循环
 *   5. 目标调色板
 */

#include "epaper_dithering_reference.h"

#include <math.h>   /* cbrt, pow, floor */
#include <stdlib.h> /* malloc, free */
#include <string.h> /* memcpy */

/* ============================================================================
 * 第 1 层：sRGB ↔ 线性伽马转换
 * ==========================================================================*/

/*
 * 连续 sRGB 分数 [0,1] → 线性 [0,1]。IEC 61966-2-1 分段传递函数。
 * 近黑区（≤0.04045）走线性段，其余走 2.4 次幂。
 */
static double srgb_fraction_to_linear(double value)
{
    if (value <= 0.04045) {
        return value / 12.92;
    }
    return pow((value + 0.055) / 1.055, 2.4);
}

double dither_srgb_channel_to_linear(uint8_t value)
{
    return srgb_fraction_to_linear(value / 255.0);
}

/* ============================================================================
 * 第 2 层：OKLab 色彩空间 + 最近色匹配
 * ==========================================================================*/

/*
 * 线性 sRGB → LMS（Ottosson 合并矩阵）。把 sRGB→XYZ 和 XYZ→LMS 两步合成
 * 一个 3×3，每个像素少做一次矩阵乘法，也避免两步来源不同造成的漂移。
 */
static const double M_RGB_LMS[3][3] = {
    { 0.4122214708, 0.5363325363, 0.0514459929 },
    { 0.2119034982, 0.6806995451, 0.1073969566 },
    { 0.0883024619, 0.2817188376, 0.6299787005 },
};

/* cbrt(LMS) → OKLab（Ottosson 的 M2 矩阵）。 */
static const double M2[3][3] = {
    { 0.2104542553, 0.7936177850, -0.0040720468 },
    { 1.9779984951, -2.4285922050, 0.4505937099 },
    { 0.0259040371, 0.7827717662, -0.8086757660 },
};

/* OKLab → LMS^(1/3)（M2 的逆）。 */
static const double M2_INV[3][3] = {
    { 1.0, 0.3963377774, 0.2158037573 },
    { 1.0, -0.1055613458, -0.0638541728 },
    { 1.0, -0.0894841775, -1.2914855480 },
};

/* LMS → 线性 sRGB（M_RGB_LMS 的逆）。 */
static const double M_LMS_RGB[3][3] = {
    { 4.0767416621, -3.3077115913, 0.2309699292 },
    { -1.2684380046, 2.6097574011, -0.3413193965 },
    { -0.0041960863, -0.7034186147, 1.7076147010 },
};

/* 色度轴权重。见头文件 dither_match_pixel_oklab 的说明。 */
#define DITHER_WAB 1.5

oklab_t dither_rgb_to_oklab(double r, double g, double b)
{
    double l = M_RGB_LMS[0][0] * r + M_RGB_LMS[0][1] * g + M_RGB_LMS[0][2] * b;
    double m = M_RGB_LMS[1][0] * r + M_RGB_LMS[1][1] * g + M_RGB_LMS[1][2] * b;
    double s = M_RGB_LMS[2][0] * r + M_RGB_LMS[2][1] * g + M_RGB_LMS[2][2] * b;

    double l_ = cbrt(l);
    double m_ = cbrt(m);
    double s_ = cbrt(s);

    oklab_t out;
    out.l = M2[0][0] * l_ + M2[0][1] * m_ + M2[0][2] * s_;
    out.a = M2[1][0] * l_ + M2[1][1] * m_ + M2[1][2] * s_;
    out.b = M2[2][0] * l_ + M2[2][1] * m_ + M2[2][2] * s_;
    return out;
}

/* 辅助：把 double 钳制到 [0,1]。 */
static double clamp01(double v)
{
    return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);
}

void dither_oklab_to_rgb(oklab_t lab, double out_rgb[3])
{
    double l_ = M2_INV[0][0] * lab.l + M2_INV[0][1] * lab.a + M2_INV[0][2] * lab.b;
    double m_ = M2_INV[1][0] * lab.l + M2_INV[1][1] * lab.a + M2_INV[1][2] * lab.b;
    double s_ = M2_INV[2][0] * lab.l + M2_INV[2][1] * lab.a + M2_INV[2][2] * lab.b;

    double l = l_ * l_ * l_;
    double m = m_ * m_ * m_;
    double s = s_ * s_ * s_;

    out_rgb[0] = clamp01(M_LMS_RGB[0][0] * l + M_LMS_RGB[0][1] * m + M_LMS_RGB[0][2] * s);
    out_rgb[1] = clamp01(M_LMS_RGB[1][0] * l + M_LMS_RGB[1][1] * m + M_LMS_RGB[1][2] * s);
    out_rgb[2] = clamp01(M_LMS_RGB[2][0] * l + M_LMS_RGB[2][1] * m + M_LMS_RGB[2][2] * s);
}

uint8_t dither_match_pixel_oklab(oklab_t pixel, const oklab_t *palette_lab,
                                 uint8_t color_count, double wab)
{
    uint8_t best_idx = 0;
    double best_dist = 1.0 / 0.0; /* +inf */
    double wab_sq = wab * wab;

    for (uint8_t i = 0; i < color_count; i++) {
        double dl = pixel.l - palette_lab[i].l;
        double da = pixel.a - palette_lab[i].a;
        double db = pixel.b - palette_lab[i].b;
        double dist = dl * dl + wab_sq * (da * da + db * db);
        if (dist < best_dist) {
            best_dist = dist;
            best_idx = i;
        }
    }
    return best_idx;
}

/* ============================================================================
 * 第 3 层：误差扩散核（权重表）
 * ==========================================================================*/

/*
 * Burkes 核。7 个权重从总和/范围/不对称三个维度决定效果，见头文件说明。
 */
static const kernel_offset_t BURKES_OFFSETS[7] = {
    { 1, 0, 8.0 / 32.0 },  /* 正右 */
    { 2, 0, 4.0 / 32.0 },  /* 右 2 格 */
    { -2, 1, 2.0 / 32.0 }, /* 下一行 左 2 格 */
    { -1, 1, 4.0 / 32.0 }, /* 下一行 左 */
    { 0, 1, 8.0 / 32.0 },  /* 下一行 正下 */
    { 1, 1, 4.0 / 32.0 },  /* 下一行 右 */
    { 2, 1, 2.0 / 32.0 },  /* 下一行 右 2 格 */
};

const dither_kernel_t DITHER_KERNEL_BURKES = { BURKES_OFFSETS, 7 };

static const kernel_offset_t FLOYD_STEINBERG_OFFSETS[4] = {
    { 1, 0, 7.0 / 16.0 },
    { -1, 1, 3.0 / 16.0 },
    { 0, 1, 5.0 / 16.0 },
    { 1, 1, 1.0 / 16.0 },
};

const dither_kernel_t DITHER_KERNEL_FLOYD_STEINBERG = { FLOYD_STEINBERG_OFFSETS, 4 };

static const kernel_offset_t ATKINSON_OFFSETS[6] = {
    { 1, 0, 1.0 / 8.0 },
    { 2, 0, 1.0 / 8.0 },
    { -1, 1, 1.0 / 8.0 },
    { 0, 1, 1.0 / 8.0 },
    { 1, 1, 1.0 / 8.0 },
    { 0, 2, 1.0 / 8.0 },
};

const dither_kernel_t DITHER_KERNEL_ATKINSON = { ATKINSON_OFFSETS, 6 };

static const kernel_offset_t STUCKI_OFFSETS[12] = {
    { 1, 0, 8.0 / 42.0 },
    { 2, 0, 4.0 / 42.0 },
    { -2, 1, 2.0 / 42.0 },
    { -1, 1, 4.0 / 42.0 },
    { 0, 1, 8.0 / 42.0 },
    { 1, 1, 4.0 / 42.0 },
    { 2, 1, 2.0 / 42.0 },
    { -2, 2, 1.0 / 42.0 },
    { -1, 2, 2.0 / 42.0 },
    { 0, 2, 4.0 / 42.0 },
    { 1, 2, 2.0 / 42.0 },
    { 2, 2, 1.0 / 42.0 },
};

const dither_kernel_t DITHER_KERNEL_STUCKI = { STUCKI_OFFSETS, 12 };

static const kernel_offset_t SIERRA_LITE_OFFSETS[3] = {
    { 1, 0, 2.0 / 4.0 },
    { -1, 1, 1.0 / 4.0 },
    { 0, 1, 1.0 / 4.0 },
};

const dither_kernel_t DITHER_KERNEL_SIERRA_LITE = { SIERRA_LITE_OFFSETS, 3 };

/* ============================================================================
 * 第 4 层：误差扩散主循环
 * ==========================================================================*/

/* 预生成 LUT：u8 sRGB → 线性 f64，避免内层循环每个像素三次 powf。 */
static void build_linear_lut(double lut[256])
{
    for (int v = 0; v < 256; v++) {
        lut[v] = dither_srgb_channel_to_linear((uint8_t)v);
    }
}

uint8_t *dither_error_diffusion(const uint8_t *pixels, uint32_t width,
                                uint32_t height, const dither_palette_t *palette,
                                const dither_kernel_t *kernel, bool serpentine)
{
    uint32_t total = (uint32_t)width * (uint32_t)height;

    /* 1) 调色板 → 线性 RGB → OKLab（匹配用） */
    oklab_t *palette_lab = dither_palette_to_oklab(palette);
    if (!palette_lab) {
        return NULL;
    }

    /* 2) 调色板 sRGB 浮点值（误差计算用，注意误差在 sRGB 空间算，不在线性空间） */
    double palette_srgb_f[256][3]; /* 最多 256 色 */
    for (uint8_t i = 0; i < palette->color_count; i++) {
        palette_srgb_f[i][0] = palette->colors[i][0];
        palette_srgb_f[i][1] = palette->colors[i][1];
        palette_srgb_f[i][2] = palette->colors[i][2];
    }

    /* 3) 工作缓冲：sRGB 浮点 [0,255]，累积扩散进来的误差。
     *    800×480×3 double ≈ 9.2 MB —— ESP32 上务必用 PSRAM。
     *    如需 PSRAM，把下面 malloc 换成：
     *      heap_caps_malloc(total*3*sizeof(double), MALLOC_CAP_SPIRAM);
     */
    double *buf = (double *)malloc((size_t)total * 3 * sizeof(double));
    if (!buf) {
        free(palette_lab);
        return NULL;
    }
    for (uint32_t i = 0; i < total * 3; i++) {
        buf[i] = (double)pixels[i];
    }

    /* 4) LUT */
    double lut[256];
    build_linear_lut(lut);

    uint8_t *out = (uint8_t *)malloc((size_t)total);
    if (!out) {
        free(buf);
        free(palette_lab);
        return NULL;
    }

    for (uint32_t y = 0; y < height; y++) {
        /* 蛇形：奇数行从右往左 */
        bool reverse = serpentine && (y % 2 == 1);

        for (uint32_t xi = 0; xi < width; xi++) {
            uint32_t x = reverse ? (width - 1 - xi) : xi;
            uint32_t idx = (y * width + x) * 3;

            /* 钳制到 [0,255] */
            double rs = buf[idx];
            double gs = buf[idx + 1];
            double bs = buf[idx + 2];
            if (rs < 0.0) rs = 0.0; else if (rs > 255.0) rs = 255.0;
            if (gs < 0.0) gs = 0.0; else if (gs > 255.0) gs = 255.0;
            if (bs < 0.0) bs = 0.0; else if (bs > 255.0) bs = 255.0;

            /* sRGB → 线性 → OKLab → 加权最近色 */
            oklab_t lab = dither_rgb_to_oklab(
                lut[(int)(rs + 0.5)],
                lut[(int)(gs + 0.5)],
                lut[(int)(bs + 0.5)]);
            uint8_t best = dither_match_pixel_oklab(lab, palette_lab,
                                                    palette->color_count, DITHER_WAB);
            out[y * width + x] = best;

            /* 量化误差（sRGB 空间） */
            double err_r = rs - palette_srgb_f[best][0];
            double err_g = gs - palette_srgb_f[best][1];
            double err_b = bs - palette_srgb_f[best][2];

            /* 按核扩散给邻居 */
            for (uint8_t k = 0; k < kernel->offset_count; k++) {
                const kernel_offset_t *off = &kernel->offsets[k];
                /* 蛇形扫描时把 dx 反向，保证误差仍相对「未扫描」方向扩散 */
                int32_t effective_dx = reverse ? -off->dx : off->dx;

                int64_t nx = (int64_t)x + effective_dx;
                int64_t ny = (int64_t)y + off->dy;

                if (nx >= 0 && nx < (int64_t)width &&
                    ny >= 0 && ny < (int64_t)height) {
                    uint32_t ni = (uint32_t)((ny * (int64_t)width + nx) * 3);
                    buf[ni] += err_r * off->weight;
                    buf[ni + 1] += err_g * off->weight;
                    buf[ni + 2] += err_b * off->weight;
                }
            }
        }
    }

    free(buf);
    free(palette_lab);
    return out;
}

uint8_t *dither_burkes(const uint8_t *pixels, uint32_t width, uint32_t height,
                       const dither_palette_t *palette, bool serpentine)
{
    return dither_error_diffusion(pixels, width, height, palette,
                                  &DITHER_KERNEL_BURKES, serpentine);
}

/* ============================================================================
 * 第 5 层：目标调色板
 * ==========================================================================*/

static const uint8_t MONO[2][3] = { { 0, 0, 0 }, { 255, 255, 255 } };
static const uint8_t BWR[3][3] = { { 0, 0, 0 }, { 255, 255, 255 }, { 255, 0, 0 } };
static const uint8_t BWY[3][3] = { { 0, 0, 0 }, { 255, 255, 255 }, { 255, 255, 0 } };
static const uint8_t BWRY[4][3] = {
    { 0, 0, 0 }, { 255, 255, 255 }, { 255, 255, 0 }, { 255, 0, 0 },
};
static const uint8_t BWGBRY[6][3] = {
    { 0, 0, 0 }, { 255, 255, 255 }, { 255, 255, 0 },
    { 255, 0, 0 }, { 0, 0, 255 }, { 0, 255, 0 },
};
static const uint8_t SEVEN_COLOR[7][3] = {
    { 0, 0, 0 }, { 255, 255, 255 }, { 255, 255, 0 },
    { 255, 0, 0 }, { 0, 0, 255 }, { 0, 255, 0 },
    { 255, 128, 0 },
};
static const uint8_t GRAYSCALE4[4][3] = {
    { 0, 0, 0 }, { 85, 85, 85 }, { 170, 170, 170 }, { 255, 255, 255 },
};
static const uint8_t GRAYSCALE16[16][3] = {
    { 0, 0, 0 }, { 17, 17, 17 }, { 34, 34, 34 }, { 51, 51, 51 },
    { 68, 68, 68 }, { 85, 85, 85 }, { 102, 102, 102 }, { 119, 119, 119 },
    { 136, 136, 136 }, { 153, 153, 153 }, { 170, 170, 170 }, { 187, 187, 187 },
    { 204, 204, 204 }, { 221, 221, 221 }, { 238, 238, 238 }, { 255, 255, 255 },
};
static const uint8_t GRAYSCALE8[8][3] = {
    { 0, 0, 0 }, { 36, 36, 36 }, { 73, 73, 73 }, { 109, 109, 109 },
    { 146, 146, 146 }, { 182, 182, 182 }, { 219, 219, 219 }, { 255, 255, 255 },
};

static const dither_palette_t P_MONO = { MONO, 2, 0 };
static const dither_palette_t P_BWR = { BWR, 3, 2 };
static const dither_palette_t P_BWY = { BWY, 3, 2 };
static const dither_palette_t P_BWRY = { BWRY, 4, 3 };
static const dither_palette_t P_BWGBRY = { BWGBRY, 6, 3 };
static const dither_palette_t P_SEVEN_COLOR = { SEVEN_COLOR, 7, 3 };
static const dither_palette_t P_GRAYSCALE4 = { GRAYSCALE4, 4, 0 };
static const dither_palette_t P_GRAYSCALE16 = { GRAYSCALE16, 16, 0 };
static const dither_palette_t P_GRAYSCALE8 = { GRAYSCALE8, 8, 0 };

const dither_palette_t *dither_scheme_palette(dither_scheme_t scheme)
{
    switch (scheme) {
    case DITHER_SCHEME_MONO: return &P_MONO;
    case DITHER_SCHEME_BWR: return &P_BWR;
    case DITHER_SCHEME_BWY: return &P_BWY;
    case DITHER_SCHEME_BWRY: return &P_BWRY;
    case DITHER_SCHEME_BWGBRY: return &P_BWGBRY;
    case DITHER_SCHEME_GRAYSCALE4: return &P_GRAYSCALE4;
    case DITHER_SCHEME_GRAYSCALE16: return &P_GRAYSCALE16;
    case DITHER_SCHEME_SEVEN_COLOR: return &P_SEVEN_COLOR;
    case DITHER_SCHEME_BWGBRY_SPLIT: return &P_BWGBRY; /* 同墨色，仅固件端打包不同 */
    case DITHER_SCHEME_GRAYSCALE8: return &P_GRAYSCALE8;
    }
    return &P_MONO; /* 不应到达 */
}

oklab_t *dither_palette_to_oklab(const dither_palette_t *palette)
{
    oklab_t *out = (oklab_t *)malloc((size_t)palette->color_count * sizeof(oklab_t));
    if (!out) {
        return NULL;
    }
    for (uint8_t i = 0; i < palette->color_count; i++) {
        double r = dither_srgb_channel_to_linear(palette->colors[i][0]);
        double g = dither_srgb_channel_to_linear(palette->colors[i][1]);
        double b = dither_srgb_channel_to_linear(palette->colors[i][2]);
        out[i] = dither_rgb_to_oklab(r, g, b);
    }
    return out;
}
