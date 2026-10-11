/*
 * epaper_dithering_reference.h
 *
 * 电子墨水屏抖动算法参考实现（C 版）。
 *
 * 数据流（每个像素）：
 *   sRGB [0,255]  →  线性 RGB [0,1]  →  OKLab  →  加权最近色匹配
 *        →  得到调色板索引  →  在 sRGB 空间算量化误差  →  按核扩散给邻居
 *
 * 对应 Rust 版：reference/epaper_dithering_reference.rs
 * 分层：
 *   1. sRGB ↔ 线性伽马转换（OKLab 前置）
 *   2. OKLab 色彩空间 + 最近色匹配
 *   3. 误差扩散核（权重表）
 *   4. 误差扩散主循环（serpentine 蛇形扫描）
 *   5. 目标调色板（墨色 sRGB 值）
 *
 * 移植注意：本文件用 double 全程运算，RAM 需求见各函数注释。
 * 在 ESP32 上建议把误差缓冲区 buf 放入 PSRAM（见 dither 函数注释）。
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- OKLab 坐标 ---- */
typedef struct {
    double l; /* 明度 [0,1] */
    double a; /* 绿-红轴（负=绿，正=红） */
    double b; /* 蓝-黄轴（负=蓝，正=黄） */
} oklab_t;

/* ---- 调色板（墨色 sRGB 值）---- */
typedef struct {
    const uint8_t (*colors)[3]; /* 每个墨色的 sRGB [R,G,B] */
    uint8_t color_count;        /* 墨色数量（≤ 256，索引用 u8） */
    uint8_t accent_idx;         /* 强调色索引（不参与匹配，供下游元数据用） */
} dither_palette_t;

/* ---- 误差扩散核 ---- */
typedef struct {
    int32_t dx;    /* 相对当前像素横向偏移（正=右） */
    int32_t dy;    /* 相对当前像素纵向偏移（正=下） */
    double weight; /* 误差分配比例（预除好） */
} kernel_offset_t;

typedef struct {
    const kernel_offset_t *offsets;
    uint8_t offset_count;
} dither_kernel_t;

/*
 * 预设配色方案。整数值是固件线协议契约，勿改（Grayscale8=9 是库本地值）。
 */
typedef enum {
    DITHER_SCHEME_MONO = 0,
    DITHER_SCHEME_BWR = 1,
    DITHER_SCHEME_BWY = 2,
    DITHER_SCHEME_BWRY = 3,
    DITHER_SCHEME_BWGBRY = 4,
    DITHER_SCHEME_GRAYSCALE4 = 5,
    DITHER_SCHEME_GRAYSCALE16 = 6,
    DITHER_SCHEME_SEVEN_COLOR = 7,
    DITHER_SCHEME_BWGBRY_SPLIT = 8,
    DITHER_SCHEME_GRAYSCALE8 = 9,
} dither_scheme_t;

/* ---- 第 1 层：sRGB ↔ 线性伽马转换 ---- */

/* sRGB [0-255] → 线性 [0,1]。IEC 61966-2-1 分段传递函数。 */
double dither_srgb_channel_to_linear(uint8_t value);

/* ---- 第 2 层：OKLab ---- */

/* 线性 RGB → OKLab。流程：RGB → LMS → cbrt → OKLab。 */
oklab_t dither_rgb_to_oklab(double r, double g, double b);

/* OKLab → 线性 RGB（逆变换）。输出钳制到 [0,1]。 */
void dither_oklab_to_rgb(oklab_t lab, double out_rgb[3]);

/*
 * 返回与 pixel 最近的调色板颜色索引，用加权笛卡尔 OKLab 距离：
 *   dist² = (ΔL)² + wab²·(Δa² + Δb²)
 *
 * 【参数】wab —— 影响巨大：
 *   = 1.0 → 纯欧氏 OKLab 距离；
 *   > 1.0 → 放大色度差权重，饱和像素倾向用彩色墨而非塌缩到黑/白/灰；
 *   < 1.0 → 明度主导，输出偏灰。
 *   推荐用 DITHER_WAB = 1.5（源项目在回归集上扫描得到的最优值）。
 *
 * palette_lab 是预先转好 OKLab 坐标的调色板（长度 = palette->color_count）。
 */
uint8_t dither_match_pixel_oklab(oklab_t pixel, const oklab_t *palette_lab,
                                 uint8_t color_count, double wab);

/* ---- 第 3 层：误差扩散核（权重表）---- */

/*
 * Burkes 核（默认算法）。标准矩阵（* 为当前像素）：
 *
 *         *    8/32  4/32
 *  2/32  4/32  8/32  4/32  2/32
 *
 * 7 个权重从三个维度决定效果（改任意一个输出纹理都会变）：
 *   1) 总和 = 1.0 → 误差 100% 传递，色调精确还原；
 *      总和 < 1 → 噪声少但暗部大色块偏亮/失真。
 *   2) 范围 2 行 → 平滑度/虫纹折中；
 *      范围越大越平滑但越慢、越易出虫纹；越小越锐利但易出条纹。
 *   3) 不对称（8/32 给正右、正下）→ 方向性拖尾，需 serpentine 抵消。
 */
extern const dither_kernel_t DITHER_KERNEL_BURKES;

/* Floyd–Steinberg 核：最经典，总和=1，范围 1 行，棋盘状伪影略多。 */
extern const dither_kernel_t DITHER_KERNEL_FLOYD_STEINBERG;

/* Atkinson 核：总和=6/8（丢 1/4），对比度保留好、暗部噪声少，偏艺术。 */
extern const dither_kernel_t DITHER_KERNEL_ATKINSON;

/* Stucki 核：跨 3 行 12 点，总和=1，最平滑但最慢、最易出虫纹。 */
extern const dither_kernel_t DITHER_KERNEL_STUCKI;

/* Sierra Lite 核：仅 3 点，总和=1，最快但易出结构化条纹。 */
extern const dither_kernel_t DITHER_KERNEL_SIERRA_LITE;

/* ---- 第 4 层：误差扩散主循环 ---- */

/*
 * 误差扩散抖动。
 *
 * 入参：
 *   pixels     RGB 平铺字节（长度 = width*height*3）
 *   width/height 图像尺寸
 *   palette    目标调色板
 *   kernel     误差扩散核
 *   serpentine true=奇数行从右往左扫（蛇形，抵消方向性拖尾，默认推荐）；
 *              false=每行都从左往右扫（光栅）
 *
 * 出参：调色板索引（长度 = width*height），调用者用 malloc 分配后可 free。
 *
 * 内存：内部工作缓冲 buf 为 width*height*3 个 double。800×480 时约
 *   9.2 MB，务必放入 PSRAM（用 heap_caps_malloc(MALLOC_CAP_SPIRAM)）。
 */
uint8_t *dither_error_diffusion(const uint8_t *pixels, uint32_t width,
                                uint32_t height, const dither_palette_t *palette,
                                const dither_kernel_t *kernel, bool serpentine);

/* Burkes 便捷封装（serpentine 建议传 true）。 */
uint8_t *dither_burkes(const uint8_t *pixels, uint32_t width, uint32_t height,
                       const dither_palette_t *palette, bool serpentine);

/* ---- 第 5 层：目标调色板 ---- */

/*
 * 获取预设方案的调色板（返回静态数据，勿 free）。
 * 墨色顺序与固件线协议一致。
 */
const dither_palette_t *dither_scheme_palette(dither_scheme_t scheme);

/*
 * 把调色板预转成 OKLab 坐标数组（供 dither_match_pixel_oklab 复用）。
 * 输出长度 = palette->color_count，调用者负责释放 out（malloc）。
 */
oklab_t *dither_palette_to_oklab(const dither_palette_t *palette);

#ifdef __cplusplus
}
#endif
