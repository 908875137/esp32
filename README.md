# ThinkINK EPD — AI/SD 双模式数字艺术相框（ESP32-S3）

为 **7.5 英寸黑白红三色电子墨水屏**（UC8179 驱动）设计的数字艺术相框，
基于 ESP32-S3 + ESP-IDF v6.0.2。

支持两种图片来源，共享同一套解码 + 裁剪 + 缩放 + 抖动 + 显示流程：

- **AI 模式**：DeepSeek 生成每日一句 → 混元生成艺术图片 → 叠加文字 → 显示
- **SD 模式**：从 SD 卡读取本地 PNG 图片，顺序播放，完全不依赖网络

> 详细硬件接线见 [HARDWARE.md](HARDWARE.md)。

---

## 项目结构

```
esp32/
├── main/                        # 应用代码
│   ├── main.c                   # 入口 — 初始化 + 双模式 dispatch + 主循环
│   ├── CMakeLists.txt           # 编译配置 (源码、头文件路径、组件依赖)
│   ├── idf_component.yml        # 组件依赖声明 (cjson, freetype)
│   │
│   ├── display/                 # 显示子系统
│   │   ├── epd.c / epd.h        # UC8179 EPD 驱动 (初始化/写入/刷新/睡眠)
│   │   ├── io.c / io.h          # SPI (FSPI/SPI2_HOST) + GPIO，一次性初始化
│   │   └── dither.c / dither.h  # Stucki 误差扩散三色抖动 (RGB565→黑白红)
│   │
│   ├── network/                 # 网络子系统
│   │   ├── wifi_manager.c/h     # STA 连接 + SoftAP 配网 + NVS 凭证存储
│   │   └── ai_client.c/h        # DeepSeek 每日一句 + 混元图片生成 (两阶段)
│   │
│   ├── image/                   # 图片处理子系统（URL + SD 共用）
│   │   ├── image_decoder.c/h    # URL下载 / SD文件 → lodepng解码 → 裁剪 → 缩放 → RGBA8 / RGB565
│   │   ├── lodepng.c/h          # 单文件 PNG 解码器（无外部依赖，仅解码）
│   │   └── cos_trusted_certs.h  # 腾讯云 COS 中间 CA 证书
│   │
│   ├── text/                    # 文字渲染子系统
│   │   └── text_renderer.c/h    # FreeType 中文渲染，字体从 SD 卡加载
│   │
│   ├── storage/                 # 存储子系统
│   │   ├── sd_card.c/h          # SD 卡持久挂载 + PNG 图片枚举
│   │   └── config_store.c/h     # NVS 模式持久化 + config.json 读取
│   │
│   ├── input/                   # 输入子系统
│   │   └── button.c/h           # 三级按键 (短按/长按/超长按)
│   │
│   ├── web/                     # Web 配网子系统
│   │   ├── web_server.c/h       # HTTP 配网页面服务器
│   │   └── web_page.h           # 内嵌 HTML 配网页
│   │
│   ├── config/                  # 配置文件
│   │   ├── ai_config.h          # AI API Key/URL/模型/Prompt
│   │   └── image_data.h         # 默认开机图片 C 数组占位
│   │
│   ├── events/                  # (预留) 应用事件系统
│   └── cli/                     # (废弃) 原始 HTTP 客户端，不再编译
│
├── partitions.csv               # 分区表 (4MB factory，无字体分区)
├── sdkconfig.defaults           # 项目默认 Kconfig
├── CMakeLists.txt               # 顶层 CMake (目标芯片 ESP32-S3)
│
├── build/                       # 构建输出 (.gitignore)
├── managed_components/          # 第三方库: cjson, freetype (.gitignore)
└── dependencies.lock            # 组件版本锁定 (.gitignore)
```

---

## 核心代码逻辑

### 1. 主流程 (`main.c`)

```
上电
  ↓
① 初始化 NVS
  ↓
② 初始化 SD 卡 (持久挂载 /sdcard，可失败)
  ↓
③ 加载配置 (NVS 模式 + /sdcard/config.json 覆盖)
  ↓
④ 初始化文字渲染 (FreeType 从 SD 卡加载字体)
  ↓
⑤ 初始化 WiFi + 三级按键 (GPIO0)
  ↓
⑥ 按模式分发：
   SD 模式 → run_sd_pipeline()   读本地图片 → 显示
   AI 模式 → 连 WiFi → run_ai_pipeline()
  ↓
⑦ 主循环 (每 500ms)：
   - 处理按键请求 (短按切图 / 长按切模式 / 超长按配网)
   - 处理配网页提交
   - AI 模式：连上 WiFi 后跑一次 pipeline
```

### 2. AI 模式 Pipeline (`run_ai_pipeline()`)

```
Stage 1  DeepSeek 每日一句
         POST /chat/completions (deepseek-v4-pro)
         → 解析 JSON {quote, author, source, type, mood}
  ↓
Stage 2  构造图片 Prompt (ai_build_image_prompt)
         POST 混元 hy-image-v3 → 解析 data[0].url
  ↓
Stage 3  image_download_and_decode_rgba(url)
         下载 PNG → lodepng 解码 → 裁剪 → 缩放 → RGBA8
  ↓
Stage 4  text_render_overlay(rgba, quote, source)
         FreeType 中文文字叠加 (底部区域)
  ↓
Stage 5  display_rgba() → RGB565 → Stucki 抖动 → EPD
```

### 3. SD 模式 Pipeline (`run_sd_pipeline()`)

```
sd_card_count_images()        统计 /sdcard/images/*.png 数量
  ↓
sd_card_get_image_path(idx)   获取第 N 张图片路径
  ↓
image_file_decode_rgba(path)  fread PNG → lodepng 解码 → 裁剪 → 缩放 → RGBA8
  ↓
display_rgba()                RGB565 → Stucki 抖动 → EPD
```

### 4. 图片来源抽象

AI 和 SD 只在「字节来源」不同，后续流程完全复用 `png_bytes_to_rgba()`：

```
AI:  HTTPS 下载 → PSRAM buffer ─┐
                                ├→ png_bytes_to_rgba() → RGBA8 → RGB565 → dither → EPD
SD:  fread 文件 → PSRAM buffer ─┘
```

---

## 按键逻辑（三级按键）

| 动作 | 时长 | 行为 | 模式 |
|------|------|------|------|
| 短按 | < 1.5s | 切下一张图片 | 仅 SD 模式 |
| 长按 | 1.5s – 5s | 切换 AI / SD 模式（写 NVS） | 两者 |
| 超长按 | ≥ 5s | 进入 WiFi 配网模式 | 两者 |

> **重要设计**：按键回调只置 `volatile` 标志位，重活（解码/抖动/配网）全部在主循环执行。
> 因为按键任务只有 2KB 栈，直接在回调里跑解码会栈溢出导致重启。
>
> **Boot 键说明**：GPIO0 只在**上电/复位瞬间**被采样决定是否进下载模式；运行起来后就是普通输入引脚，按它不会重启。「按住 Boot + 按 Reset」才进固件下载模式。

---

## SD 卡目录结构

```
/sdcard/
├── images/                      # PNG 图片 (顺序播放)
│   ├── 001.png
│   ├── 002.png
│   └── ...
├── fonts/
│   └── NotoSansCJKsc-Subset.otf # 中文字体
├── quotes/
│   └── quotes.json              # (预留)
└── config.json                  # 配置文件
```

### config.json 格式

```json
{
    "mode": "sd",
    "image_dir": "/sdcard/images",
    "font": "/sdcard/fonts/NotoSansCJKsc-Subset.otf",
    "image_mode": "sequential",
    "quote_enabled": true,
    "refresh_hour": 8
}
```

| 字段 | 说明 | 当前状态 |
|------|------|:---:|
| `mode` | `"ai"` / `"sd"` 启动模式 | ✅ 生效（NVS 优先级更高） |
| `image_dir` | SD 图片目录 | ⚠️ 预留（枚举仍用 /sdcard/images） |
| `font` | 字体文件路径 | ✅ 生效 |
| `image_mode` | `"sequential"` / `"random"` | ❌ 预留 |
| `quote_enabled` | 是否显示每日一句 | ❌ 预留 |
| `refresh_hour` | 每日刷新时间 | ❌ 预留 |

> **模式优先级**：NVS 里保存的模式 > config.json 的 `mode` 字段。长按切换过模式后，NVS 会覆盖 config.json。

---

## 内存管理策略

| 缓冲区 | 分配器 | 内存池 | 典型大小 | 生命周期 |
|--------|--------|--------|----------|----------|
| PNG 下载/SD读取 buffer | `heap_caps_malloc(SPIRAM)` | PSRAM | 0.5~2.8 MB | 读入→解码 |
| lodepng RGBA 原始 | `malloc()` | DRAM/PSRAM | 源图尺寸×4 | 解码→缩放 |
| RGBA8 输出 (800×480) | `malloc()` | PSRAM | 1.5 MB | 缩放→文字→RGB565 |
| RGB565 输出 | `malloc()` | DRAM | 768 KB | 转换→抖动→显示 |
| EPD BW/Red | `calloc()` | DRAM | 各 48 KB | 抖动→写入 |
| Dither 误差行 | `calloc()` | DRAM | ~29 KB | 抖动期间 |

关键原则：
- PNG 字节 buffer 固定走 PSRAM，避免 DRAM 碎片化
- `malloc()` 因 `SPIRAM_USE_MALLOC=y` 自动把 >4KB 大块路由到 PSRAM
- 大块 buffer 用完即释放，不长期共存

---

## 抖动算法 (`dither.c`)

- **Stucki 误差扩散** — 每像素量化误差按 42 分母扩散到 12 个相邻像素
- **三色量化**：输入 RGB 与 {黑, 白, 红} 计算加权感知距离，选最近者
- **红色判定**：绝对 R 阈值 + R 主导 G/B + HSV 饱和度 & 色相检查 + 红色惩罚
- **对比度增强**：量化前亮度拉伸 (`IMAGE_CONTRAST=1.08`)
- 内存仅 ~29KB，不依赖 PSRAM

---

## COS 证书修复

腾讯云 COS 使用 GlobalSign Atlas R3 OV TLS CA 2026 Q1 中间证书，不在 ESP-IDF
默认 bundle 中。解决：`image/cos_trusted_certs.h` 内嵌中间 CA + 根 CA PEM，
下载时指定 `cert_pem`。

---

## 硬件配置

| 项目 | 配置 |
|------|------|
| 芯片 | ESP32-S3 N16R8 |
| Flash | 16 MB (QIO, 80 MHz) |
| PSRAM | 8 MB (Octal, 80 MHz) |
| EPD 驱动 | UC8179 |
| 分辨率 | 800 × 480 |
| 颜色 | 黑白红 (BW/RED) |
| EPD SPI | SPI2_HOST (FSPI)，4 MHz |
| SD SPI | SPI3_HOST (HSPI)，10 MHz |
| 按键 | GPIO0 (板载 Boot 按钮) |

详见 [HARDWARE.md](HARDWARE.md) 完整引脚图。

---

## SDK 配置要点 (`sdkconfig.defaults`)

| 配置项 | 值 | 说明 |
|--------|-----|------|
| `CONFIG_SPIRAM_USE_MALLOC` | y | `malloc()` 自动将 >4KB 分配到 PSRAM |
| `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL` | 4096 | ≤4KB 走内部 DRAM |
| `CONFIG_SPIRAM_MODE_OCT` | y | Octal PSRAM 模式 |
| `CONFIG_ESP_MAIN_TASK_STACK_SIZE` | 24576 | 24KB 主任务栈 (TLS 握手 + 解码需要) |
| `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE` | y | CA 证书 bundle (AI API 用) |
| `CONFIG_PARTITION_TABLE_CUSTOM` | y | 自定义分区表 |
| `CONFIG_ESPTOOLPY_FLASHSIZE` | 16MB | 16 MB Flash |
| `CONFIG_SPIFFS_OBJ_NAME_LEN` | 64 | 长文件名支持（字体曾放 SPIFFS，现可保留） |

---

## API 配置

| 配置项 | 位置 | 值 |
|--------|------|-----|
| API Key | `main/config/ai_config.h` | 腾讯 TokenHub Key |
| DeepSeek | `main/config/ai_config.h` | `deepseek-v4-pro-202606` |
| 混元图片 | `main/config/ai_config.h` | `hy-image-v3` |
| 图片尺寸 | `main/network/ai_client.c` | `854x512` |

---

## 构建 & 烧录

```bash
cd /path/to/esp32

# 激活 ESP-IDF 环境
source ~/Tools/activate_idf_v6.0.sh

# 构建
idf.py build

# 烧录 & 监控
idf.py -p COMx flash monitor
```

> **分区表改动后**（如删除了 fonts 分区），需要重新烧录分区表。若遇到分区表未更新，用 `idf.py -p COMx erase-flash flash`（会清 NVS，需重新配网）。

---

## 开发计划

- [x] Phase 1: WiFi 连接 + HTTP 测试 + 离线图片显示
- [x] Phase 2: AI 图片生成 Pipeline (DeepSeek + 混元 + 下载 + 解码 + 抖动 + EPD)
- [x] Phase 3: SD 图片读取 + 双模式 (AI/SD) + NVS 持久化 + 三级按键 + 字体从 SD 加载
- [ ] Phase 4: SD 图片 JPG 支持 (引入 esp_jpeg)
- [ ] Phase 5: SD 图片随机播放
- [ ] Phase 6: AI 图片保存到 SD
- [ ] Phase 7: config.json 的 image_dir/image_mode 真正生效
- [ ] Phase 8: 每日一句 + 本地图片合成 (quotes.json)
- [ ] Phase 9: 定时每日刷新 (RTC/SNTP)

---

## 已知限制 / 注意事项

1. **SD 图片仅支持 PNG**（JPG 需引入 esp_jpeg 组件，见 Phase 4）
2. **`image_dir` 配置未打通**：`config_store` 会读该字段，但 `sd_card` 枚举函数仍硬编码 `/sdcard/images`
3. **字体必须放 SD 卡** `/sdcard/fonts/NotoSansCJKsc-Subset.otf`（或 config.json 的 `font` 指定）
4. **模式切换会写 NVS**，长按切过模式后 NVS 优先于 config.json
5. **GPIO15 是 Strapping 引脚**（EPD CS），上电瞬间电平可能影响启动，需留意偶发启动失败

---

## 项目自动生成文件

以下文件/目录由工具自动生成，已加入 `.gitignore`：

| 文件/目录 | 生成方式 | 说明 |
|-----------|----------|------|
| `build/` | `idf.py build` | CMake 构建产物 |
| `managed_components/` | `idf.py build` | 下载的第三方组件 (cjson, freetype) |
| `dependencies.lock` | `idf.py build` | 精确锁定组件版本 |
| `sdkconfig` | `idf.py set-target` | 当前生效 Kconfig |

`.clangd` 告诉 clangd 在索引阶段移除 `-f*` `-m*` 等交叉编译参数，避免 VSCode 红色波浪线误报，不影响编译。
