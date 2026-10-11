//! ============================================================================
//! 电子墨水屏抖动算法参考实现（合并自 epaper-dithering 项目）
//!
//! 本文件把 epaper-dithering 里三个正交分层的模块合并成一份可独立阅读 / 移植的参考：
//!
//!   ┌─────────────────────────────────────────────────────────────┐
//!   │ 第 1 层  color_space   sRGB ↔ 线性 RGB 伽马转换（OKLab 的前置）│
//!   │ 第 2 层  oklab         OKLab 色彩空间 + 最近色匹配            │
//!   │ 第 3 层  kernel        Burkes 等误差扩散核（权重表）           │
//!   │ 第 4 层  dither        误差扩散主循环（serpentine 蛇形扫描）   │
//!   │ 第 5 层  palette       目标调色板（墨色 sRGB 值）              │
//!   └─────────────────────────────────────────────────────────────┘
//!
//! 数据流（每个像素）：
//!   sRGB [0,255]  →  线性 RGB [0,1]  →  OKLab  →  加权最近色匹配
//!        →  得到调色板索引  →  在 sRGB 空间算量化误差  →  按核扩散给邻居
//!
//! 依赖：无外部 crate（去掉了 rayon / approx），可直接编译，也可逐行翻译成 C。
//! ============================================================================

// ════════════════════════════════════════════════════════════════════════════
// 第 1 层：sRGB ↔ 线性 RGB 伽马转换
// ════════════════════════════════════════════════════════════════════════════

/// sRGB [0-255] → 线性 [0.0-1.0]。IEC 61966-2-1 分段传递函数。
///
/// 【参数/为什么重要】sRGB 是伽马编码的（暗部压缩、亮部拉伸）。若跳过这一步、
/// 直接在 sRGB 数值上算颜色距离，明度会被严重扭曲，导致暗部匹配错误。
/// 必须先转线性，再进 OKLab。
pub fn srgb_channel_to_linear(value: u8) -> f64 {
    srgb_fraction_to_linear(value as f64 / 255.0)
}

/// 连续 sRGB 分数 [0,1] → 线性 [0,1]。与上面同一伽马曲线，但输入是连续值。
fn srgb_fraction_to_linear(value: f64) -> f64 {
    if value <= 0.04045 {
        value / 12.92                       // 近黑区：线性段
    } else {
        ((value + 0.055) / 1.055).powf(2.4) // 其余：2.4 次幂
    }
}

// ════════════════════════════════════════════════════════════════════════════
// 第 2 层：OKLab 色彩空间 + 最近色匹配
// ════════════════════════════════════════════════════════════════════════════

/// 线性 sRGB → LMS（Ottosson 合并矩阵）。把 sRGB→XYZ 和 XYZ→LMS 两步合成一个 3×3，
/// 每个像素少做一次矩阵乘法，也避免两步来源不同造成的漂移。
const M_RGB_LMS: [[f64; 3]; 3] = [
    [0.412_221_470_8, 0.536_332_536_3, 0.051_445_992_9],
    [0.211_903_498_2, 0.680_699_545_1, 0.107_396_956_6],
    [0.088_302_461_9, 0.281_718_837_6, 0.629_978_700_5],
];

/// cbrt(LMS) → OKLab（Ottosson 的 M2 矩阵）。
const M2: [[f64; 3]; 3] = [
    [0.210_454_255_3, 0.793_617_785_0, -0.004_072_046_8],
    [1.977_998_495_1, -2.428_592_205_0, 0.450_593_709_9],
    [0.025_904_037_1, 0.782_771_766_2, -0.808_675_766_0],
];

/// OKLab 逆变换矩阵（M2 的逆）：OKLab → LMS^(1/3)。
const M2_INV: [[f64; 3]; 3] = [
    [1.0, 0.396_337_777_4, 0.215_803_757_3],
    [1.0, -0.105_561_345_8, -0.063_854_172_8],
    [1.0, -0.089_484_177_5, -1.291_485_548_0],
];

/// LMS → 线性 sRGB（M_RGB_LMS 的逆）。
const M_LMS_RGB: [[f64; 3]; 3] = [
    [4.076_741_662_1, -3.307_711_591_3, 0.230_969_929_2],
    [-1.268_438_004_6, 2.609_757_401_1, -0.341_319_396_5],
    [-0.004_196_086_3, -0.703_418_614_7, 1.707_614_701_0],
];

/// 色度轴权重，用于 `match_pixel_oklab`。
///
/// 【参数 —— 影响巨大】匹配距离公式：
///   dist² = (ΔL)² + wab²·(Δa² + Δb²)
///
///   wab = 1.0 → 纯欧氏 OKLab 距离（本身已是感知均匀）。
///   wab > 1.0 → 放大「色度差」（色相/饱和度）相对「明度差」的权重，
///               让饱和像素更倾向用彩色墨（黄/红/蓝/绿），而不是塌缩到黑/白/灰。
///               避免「无彩色吸引子」问题（鲜艳紫被错配成黑/白）。
///   wab < 1.0 → 明度占主导，输出更灰、彩色墨几乎不用。
///
///   1.5 是项目在回归测试集上扫描 wab 得到的最优值（见源项目 issue #28）。
pub const WAB: f64 = 1.5;

#[derive(Debug, Clone, Copy, PartialEq)]
pub struct OkLab {
    pub l: f64, // 明度  [0,1]
    pub a: f64, // 绿-红轴（负=绿，正=红）
    pub b: f64, // 蓝-黄轴（负=蓝，正=黄）
}

/// 线性 RGB → OKLab。流程：RGB → LMS → cbrt → OKLab。
pub fn rgb_to_oklab(r: f64, g: f64, b: f64) -> OkLab {
    let l = M_RGB_LMS[0][0] * r + M_RGB_LMS[0][1] * g + M_RGB_LMS[0][2] * b;
    let m = M_RGB_LMS[1][0] * r + M_RGB_LMS[1][1] * g + M_RGB_LMS[1][2] * b;
    let s = M_RGB_LMS[2][0] * r + M_RGB_LMS[2][1] * g + M_RGB_LMS[2][2] * b;

    let l_ = l.cbrt();
    let m_ = m.cbrt();
    let s_ = s.cbrt();

    OkLab {
        l: M2[0][0] * l_ + M2[0][1] * m_ + M2[0][2] * s_,
        a: M2[1][0] * l_ + M2[1][1] * m_ + M2[1][2] * s_,
        b: M2[2][0] * l_ + M2[2][1] * m_ + M2[2][2] * s_,
    }
}

/// OKLab → 线性 RGB（`rgb_to_oklab` 的逆）。输出钳制到 [0,1]。
pub fn oklab_to_rgb(lab: OkLab) -> [f64; 3] {
    let l_ = M2_INV[0][0] * lab.l + M2_INV[0][1] * lab.a + M2_INV[0][2] * lab.b;
    let m_ = M2_INV[1][0] * lab.l + M2_INV[1][1] * lab.a + M2_INV[1][2] * lab.b;
    let s_ = M2_INV[2][0] * lab.l + M2_INV[2][1] * lab.a + M2_INV[2][2] * lab.b;

    let l = l_ * l_ * l_;
    let m = m_ * m_ * m_;
    let s = s_ * s_ * s_;

    let r = M_LMS_RGB[0][0] * l + M_LMS_RGB[0][1] * m + M_LMS_RGB[0][2] * s;
    let g = M_LMS_RGB[1][0] * l + M_LMS_RGB[1][1] * m + M_LMS_RGB[1][2] * s;
    let b = M_LMS_RGB[2][0] * l + M_LMS_RGB[2][1] * m + M_LMS_RGB[2][2] * s;

    [r.clamp(0.0, 1.0), g.clamp(0.0, 1.0), b.clamp(0.0, 1.0)]
}

/// 预先把整个调色板转成 OKLab 坐标，供匹配时重复使用。
pub struct PaletteLab {
    pub colors: Vec<OkLab>,
}

impl PaletteLab {
    /// 从线性 RGB 调色板构建 OKLab 调色板。
    pub fn from_linear_rgb(palette: &[[f64; 3]]) -> Self {
        let colors = palette
            .iter()
            .map(|c| rgb_to_oklab(c[0], c[1], c[2]))
            .collect();
        Self { colors }
    }
}

/// 返回与 `pixel` 最近的调色板颜色索引，用加权笛卡尔 OKLab 距离。
///
/// 【参数 —— 影响巨大】`wab` 见上面 `WAB` 的说明。距离公式：
///   dist² = (1·ΔL)² + (wab·Δa)² + (wab·Δb)²
pub fn match_pixel_oklab(pixel: OkLab, palette: &PaletteLab, wab: f64) -> usize {
    let mut best_idx = 0;
    let mut best_dist = f64::INFINITY;
    let wab_sq = wab * wab;
    for (i, pal) in palette.colors.iter().enumerate() {
        let dl = pixel.l - pal.l;
        let da = pixel.a - pal.a;
        let db = pixel.b - pal.b;
        let dist = dl * dl + wab_sq * (da * da + db * db);
        if dist < best_dist {
            best_dist = dist;
            best_idx = i;
        }
    }
    best_idx
}

// ════════════════════════════════════════════════════════════════════════════
// 第 3 层：误差扩散核（权重表）
// ════════════════════════════════════════════════════════════════════════════

/// 核中一个条目：相对当前像素 (dx, dy) 的偏移 + 预除好的权重。
pub struct KernelOffset {
    pub dx: i32,     // 相对当前像素的横向偏移（正=右）
    pub dy: i32,     // 相对当前像素的纵向偏移（正=下）
    pub weight: f64, // 误差分配比例
}

pub struct Kernel {
    pub offsets: &'static [KernelOffset],
}

/// ── Burkes 核（本项目默认算法）────────────────────────────────────────
///
/// 【参数 —— 决定性】这是整个抖动算法的核心参数表。标准 Burkes 矩阵（* 为当前像素）：
///
///           *    8/32  4/32
///    2/32  4/32  8/32  4/32  2/32
///
/// 7 个权重从三个维度影响效果：
///
///   1) 总和 = 32/32 = 1.0：误差 100% 传递出去。
///      总和 = 1 → 色调精确还原，但可能出纹理伪影；
///      总和 < 1 → 部分误差丢弃（如 Atkinson 只传 6/8，丢 1/4），
///                 噪声更少但暗部大色块会偏亮 / 失真。
///
///   2) 扩散范围：只覆盖当前行右侧 2 格 + 下一行 5 格（共 2 行）。
///      范围越大（Stucki/JJN 跨 3 行）过渡越平滑但更慢、更易出「虫纹」；
///      范围越小（Sierra Lite 仅 3 点）越锐利但易出结构化条纹。
///
///   3) 不对称性：最大权重 8/32 给了「正右」和「正下」，
///      误差沿扫描方向偏移 → 产生方向性拖尾，
///      这正是下面主循环用 `serpentine`（蛇形扫描）来抵消的原因。
///
///   之所以电子墨水屏默认用 Burkes：只需缓存 2 行误差（省内存）、权重和 = 1
///   （色调准）、范围适中（伪影少），是速度/质量/伪影的最佳折中。
pub static BURKES: Kernel = Kernel {
    offsets: &[
        KernelOffset { dx: 1, dy: 0, weight: 8.0 / 32.0 },  // 正右
        KernelOffset { dx: 2, dy: 0, weight: 4.0 / 32.0 },  // 右 2 格
        KernelOffset { dx: -2, dy: 1, weight: 2.0 / 32.0 }, // 下一行 左 2 格
        KernelOffset { dx: -1, dy: 1, weight: 4.0 / 32.0 }, // 下一行 左
        KernelOffset { dx: 0, dy: 1, weight: 8.0 / 32.0 },  // 下一行 正下
        KernelOffset { dx: 1, dy: 1, weight: 4.0 / 32.0 },  // 下一行 右
        KernelOffset { dx: 2, dy: 1, weight: 2.0 / 32.0 },  // 下一行 右 2 格
    ],
};

/// ── Floyd–Steinberg 核（最经典）──────────────────────────────────────
///       *   7/16
/// 3/16  5/16  1/16
/// 总和 = 1，范围 1 行。速度快但棋盘状伪影比 Burkes 明显。
pub static FLOYD_STEINBERG: Kernel = Kernel {
    offsets: &[
        KernelOffset { dx: 1, dy: 0, weight: 7.0 / 16.0 },
        KernelOffset { dx: -1, dy: 1, weight: 3.0 / 16.0 },
        KernelOffset { dx: 0, dy: 1, weight: 5.0 / 16.0 },
        KernelOffset { dx: 1, dy: 1, weight: 1.0 / 16.0 },
    ],
};

/// ── Atkinson 核 ─────────────────────────────────────────────────────
///       *   1/8  1/8
/// 1/8  1/8  1/8
///       1/8
/// 总和 = 6/8 = 0.75（丢弃 1/4 误差）→ 对比度保留好、暗部噪声少，
/// 但大面积暗部会偏亮。偏艺术风格。
pub static ATKINSON: Kernel = Kernel {
    offsets: &[
        KernelOffset { dx: 1, dy: 0, weight: 1.0 / 8.0 },
        KernelOffset { dx: 2, dy: 0, weight: 1.0 / 8.0 },
        KernelOffset { dx: -1, dy: 1, weight: 1.0 / 8.0 },
        KernelOffset { dx: 0, dy: 1, weight: 1.0 / 8.0 },
        KernelOffset { dx: 1, dy: 1, weight: 1.0 / 8.0 },
        KernelOffset { dx: 0, dy: 2, weight: 1.0 / 8.0 },
    ],
};

/// ── Stucki 核（扩散范围最大之一）────────────────────────────────────
/// 跨 3 行、12 个点，总和 = 1。过渡最平滑，但最慢、最容易出虫纹。
pub static STUCKI: Kernel = Kernel {
    offsets: &[
        KernelOffset { dx: 1, dy: 0, weight: 8.0 / 42.0 },
        KernelOffset { dx: 2, dy: 0, weight: 4.0 / 42.0 },
        KernelOffset { dx: -2, dy: 1, weight: 2.0 / 42.0 },
        KernelOffset { dx: -1, dy: 1, weight: 4.0 / 42.0 },
        KernelOffset { dx: 0, dy: 1, weight: 8.0 / 42.0 },
        KernelOffset { dx: 1, dy: 1, weight: 4.0 / 42.0 },
        KernelOffset { dx: 2, dy: 1, weight: 2.0 / 42.0 },
        KernelOffset { dx: -2, dy: 2, weight: 1.0 / 42.0 },
        KernelOffset { dx: -1, dy: 2, weight: 2.0 / 42.0 },
        KernelOffset { dx: 0, dy: 2, weight: 4.0 / 42.0 },
        KernelOffset { dx: 1, dy: 2, weight: 2.0 / 42.0 },
        KernelOffset { dx: 2, dy: 2, weight: 1.0 / 42.0 },
    ],
};

/// ── Sierra Lite 核（范围最小）───────────────────────────────────────
/// 仅 3 个点，总和 = 1。最快，但易出现结构化条纹。
pub static SIERRA_LITE: Kernel = Kernel {
    offsets: &[
        KernelOffset { dx: 1, dy: 0, weight: 2.0 / 4.0 },
        KernelOffset { dx: -1, dy: 1, weight: 1.0 / 4.0 },
        KernelOffset { dx: 0, dy: 1, weight: 1.0 / 4.0 },
    ],
};

// ════════════════════════════════════════════════════════════════════════════
// 第 4 层：误差扩散主循环
// ════════════════════════════════════════════════════════════════════════════

/// 误差扩散抖动。输入 RGB 平铺字节（长度 = width*height*3），
/// 输出调色板索引（长度 = width*height）。
///
/// 【参数】`serpentine`：
///   true  → 奇数行从右往左扫（蛇形扫描），抵消误差方向性拖尾，输出更均匀。
///   false → 每行都从左往右扫（光栅扫描）。
///   Burkes 默认 true。对渐变图影响明显。
pub fn error_diffusion(
    pixels: &[u8],
    width: usize,
    height: usize,
    palette: &Palette,
    kernel: &Kernel,
    serpentine: bool,
) -> Vec<u8> {
    // 1) 调色板 → 线性 RGB → OKLab（匹配用）
    let palette_linear: Vec<[f64; 3]> = palette
        .colors
        .iter()
        .map(|&[r, g, b]| {
            [
                srgb_channel_to_linear(r),
                srgb_channel_to_linear(g),
                srgb_channel_to_linear(b),
            ]
        })
        .collect();
    let palette_lab = PaletteLab::from_linear_rgb(&palette_linear);

    // 2) 调色板 sRGB 浮点值（误差计算用，注意误差在 sRGB 空间算，不在线性空间）
    let palette_srgb_f: Vec<[f64; 3]> = palette
        .colors
        .iter()
        .map(|&[r, g, b]| [r as f64, g as f64, b as f64])
        .collect();

    // 3) 工作缓冲区：sRGB 浮点 [0,255]，累积扩散进来的误差
    let mut buf: Vec<f64> = pixels.iter().map(|&v| v as f64).collect();

    // 4) LUT：u8 sRGB → 线性 f64，避免内层循环每个像素三次 powf
    let lut: Vec<f64> = (0u8..=255).map(srgb_channel_to_linear).collect();

    let mut out = vec![0u8; width * height];

    for y in 0..height {
        // 蛇形：奇数行从右往左
        let reverse = serpentine && y % 2 == 1;

        for xi in 0..width {
            let x = if reverse { width - 1 - xi } else { xi };
            let idx = (y * width + x) * 3;

            let rs = buf[idx].clamp(0.0, 255.0);
            let gs = buf[idx + 1].clamp(0.0, 255.0);
            let bs = buf[idx + 2].clamp(0.0, 255.0);

            // sRGB → 线性 → OKLab → 加权最近色
            let lab = rgb_to_oklab(
                lut[rs.round() as usize],
                lut[gs.round() as usize],
                lut[bs.round() as usize],
            );
            let best = match_pixel_oklab(lab, &palette_lab, WAB);
            out[y * width + x] = best as u8;

            // 量化误差（sRGB 空间）
            let err_r = rs - palette_srgb_f[best][0];
            let err_g = gs - palette_srgb_f[best][1];
            let err_b = bs - palette_srgb_f[best][2];

            // 按核扩散给邻居
            for off in kernel.offsets {
                // 蛇形扫描时把 dx 反向，保证误差仍相对「未扫描」方向扩散
                let effective_dx = if reverse { -off.dx } else { off.dx };

                let nx = x as i64 + effective_dx as i64;
                let ny = y as i64 + off.dy as i64;

                if nx >= 0 && nx < width as i64 && ny >= 0 && ny < height as i64 {
                    let ni = (ny as usize * width + nx as usize) * 3;
                    buf[ni] += err_r * off.weight;
                    buf[ni + 1] += err_g * off.weight;
                    buf[ni + 2] += err_b * off.weight;
                }
            }
        }
    }

    out
}

/// Burkes 便捷封装。
pub fn burkes(pixels: &[u8], w: usize, h: usize, palette: &Palette, serpentine: bool) -> Vec<u8> {
    error_diffusion(pixels, w, h, palette, &BURKES, serpentine)
}

// ════════════════════════════════════════════════════════════════════════════
// 第 5 层：目标调色板
// ════════════════════════════════════════════════════════════════════════════

/// 调色板：每个墨色的 sRGB [R,G,B] + 强调色索引。
///
/// 【参数】`accent_idx`：标记哪个颜色是「强调色」（如 BWR 里的红）。
/// 注意：它不参与上面的最近色匹配，是给下游（色调映射/色域压缩等）用的元数据。
pub struct Palette {
    pub colors: &'static [[u8; 3]], // 每个墨色的 sRGB [R,G,B]
    pub accent_idx: usize,           // 强调色在 colors 里的索引
}

/// 电子墨水屏配色方案。整数值是固件线协议契约，勿改。
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub enum ColorScheme {
    Mono = 0,
    Bwr = 1,
    Bwy = 2,
    Bwry = 3,
    Bwgbry = 4,
    Grayscale4 = 5,
    Grayscale16 = 6,
    SevenColor = 7,
    BwgbrySplit = 8,
    Grayscale8 = 9, // 库本地值，非固件线值（Inkplate 10 等 8 级灰面板用）
}

// ── 各方案的墨色 sRGB 值 ──────────────────────────────────────────────
static PALETTE_MONO: Palette = Palette {
    colors: &[[0, 0, 0], [255, 255, 255]],
    accent_idx: 0,
};
static PALETTE_BWR: Palette = Palette {
    colors: &[[0, 0, 0], [255, 255, 255], [255, 0, 0]],
    accent_idx: 2,
};
static PALETTE_BWY: Palette = Palette {
    colors: &[[0, 0, 0], [255, 255, 255], [255, 255, 0]],
    accent_idx: 2,
};
static PALETTE_BWRY: Palette = Palette {
    colors: &[[0, 0, 0], [255, 255, 255], [255, 255, 0], [255, 0, 0]],
    accent_idx: 3,
};
static PALETTE_BWGBRY: Palette = Palette {
    colors: &[
        [0, 0, 0], [255, 255, 255], [255, 255, 0],
        [255, 0, 0], [0, 0, 255], [0, 255, 0],
    ],
    accent_idx: 3,
};
/// 7 色：BWGBRY 六色 + 橙色（索引 6）。
static PALETTE_SEVEN_COLOR: Palette = Palette {
    colors: &[
        [0, 0, 0], [255, 255, 255], [255, 255, 0],
        [255, 0, 0], [0, 0, 255], [0, 255, 0],
        [255, 128, 0],
    ],
    accent_idx: 3,
};
static PALETTE_GRAYSCALE4: Palette = Palette {
    colors: &[[0, 0, 0], [85, 85, 85], [170, 170, 170], [255, 255, 255]],
    accent_idx: 0,
};
static PALETTE_GRAYSCALE16: Palette = Palette {
    colors: &[
        [0, 0, 0], [17, 17, 17], [34, 34, 34], [51, 51, 51],
        [68, 68, 68], [85, 85, 85], [102, 102, 102], [119, 119, 119],
        [136, 136, 136], [153, 153, 153], [170, 170, 170], [187, 187, 187],
        [204, 204, 204], [221, 221, 221], [238, 238, 238], [255, 255, 255],
    ],
    accent_idx: 0,
};
static PALETTE_GRAYSCALE8: Palette = Palette {
    colors: &[
        [0, 0, 0], [36, 36, 36], [73, 73, 73], [109, 109, 109],
        [146, 146, 146], [182, 182, 182], [219, 219, 219], [255, 255, 255],
    ],
    accent_idx: 0,
};

impl ColorScheme {
    pub fn palette(self) -> &'static Palette {
        match self {
            ColorScheme::Mono => &PALETTE_MONO,
            ColorScheme::Bwr => &PALETTE_BWR,
            ColorScheme::Bwy => &PALETTE_BWY,
            ColorScheme::Bwry => &PALETTE_BWRY,
            ColorScheme::Bwgbry => &PALETTE_BWGBRY,
            ColorScheme::Grayscale4 => &PALETTE_GRAYSCALE4,
            ColorScheme::Grayscale16 => &PALETTE_GRAYSCALE16,
            ColorScheme::SevenColor => &PALETTE_SEVEN_COLOR,
            // 与 Bwgbry 相同墨色，仅固件端平面打包方式不同
            ColorScheme::BwgbrySplit => &PALETTE_BWGBRY,
            ColorScheme::Grayscale8 => &PALETTE_GRAYSCALE8,
        }
    }
}

// ════════════════════════════════════════════════════════════════════════════
// 参数速查表（移植 / 调参时优先看这里）
// ════════════════════════════════════════════════════════════════════════════
//
// ┌─────────────┬───────────┬──────────────────────────────────────────────┐
// │ 参数         │ 默认值     │ 影响                                         │
// ├─────────────┼───────────┼──────────────────────────────────────────────┤
// │ 核权重表      │ Burkes    │ 决定性：总和=色调准度；范围=平滑度/虫纹；      │
// │ (Kernel)    │ (7 权重)   │ 不对称=方向性拖尾(需 serpentine 抵消)         │
// ├─────────────┼───────────┼──────────────────────────────────────────────┤
// │ WAB         │ 1.5       │ 巨大：>1 让饱和色用彩色墨，<1 输出偏灰           │
// ├─────────────┼───────────┼──────────────────────────────────────────────┤
// │ serpentine  │ true      │ 明显：抵消方向性伪影，渐变更均匀                  │
// ├─────────────┼───────────┼──────────────────────────────────────────────┤
// │ 伽马转换      │ 分段 2.4  │ 必要：跳过会导致明度/颜色距离算错               │
// ├─────────────┼───────────┼──────────────────────────────────────────────┤
// │ accent_idx  │ 按方案     │ 不参与匹配，仅供下游元数据使用                  │
// └─────────────┴───────────┴──────────────────────────────────────────────┘
