# ThinkINK EPD — AI 每日艺术作品（ESP32-S3）

为 **7.5 英寸黑白红三色电子墨水屏**（UC8179 驱动）设计的 AI 每日艺术作品生成器，
基于 ESP32-S3 + ESP-IDF v6.0.2。

## 项目结构

```
esp32/
├── main/                        # 应用代码
│   ├── main.c                   # 应用入口 — 初始化 + 主循环
│   ├── CMakeLists.txt           # 编译配置 (源码、头文件路径、组件依赖)
│   ├── idf_component.yml        # 组件依赖声明 (espressif/cjson)
│   │
│   ├── display/                 # 显示子系统
│   │   ├── epd.c / epd.h        # UC8179 EPD 驱动 (初始化/写入/刷新/睡眠)
│   │   ├── io.c / io.h          # SPI (FSPI/SPI2_HOST) + GPIO 底层操作
│   │   └── dither.c / dither.h  # Stucki 误差扩散三色抖动 (RGB565→黑白红)
│   │
│   ├── network/                 # 网络子系统
│   │   ├── wifi_manager.c/h     # STA 连接 + SoftAP 配网 + NVS 凭证存储
│   │   └── ai_client.c/h        # 混元图片 API V3 客户端 (HTTPS POST→解析 data[0].url)
│   │
│   ├── image/                   # 图片下载 & 处理子系统
│   │   ├── image_decoder.c/h    # HTTPS 下载 PNG → lodepng 解码 → 居中裁剪 → 双线性缩放到 800×480 → RGB565
│   │   ├── lodepng.c/h          # 单文件 PNG 解码器 (无外部依赖，仅解码)
│   │   └── cos_trusted_certs.h  # 腾讯云 COS 中间 CA 证书 (GlobalSign Atlas R3 — ESP-IDF 默认 bundle 缺失)
│   │
│   ├── web/                     # Web 配网子系统
│   │   ├── web_server.c/h       # HTTP 配网页面服务器 (GET / → 页面, POST /save → 保存凭证)
│   │   └── web_page.h           # 内嵌 HTML 配网页 (const char*)
│   │
│   ├── input/                   # 输入子系统
│   │   └── button.c/h           # 按键轮询任务 (GPIO0, 长按 ≥3s → 进入配网模式)
│   │
│   ├── config/                  # 配置文件
│   │   ├── ai_config.h          # AI API Key/URL/Prompt (唯一配置点)
│   │   └── image_data.h         # 默认开机图片 C 数组占位
│   │
│   ├── events/                  # (预留) 应用事件系统
│   │   └── events.h             # 事件 ID 枚举 + 载荷结构体
│   │
│   ├── cli/                     # (废弃) 原始 HTTP 客户端代码，不再编译
│   │   └── http.c/h
│   │
│   └── storage/                 # (预留) 本地存储/NVS 封装
│
├── partitions.csv               # 自定义分区表 (4MB factory + NVS)
├── sdkconfig.defaults           # 项目默认 Kconfig (芯片、PSRAM、WiFi、mbedTLS)
├── sdkconfig                    # 当前生效配置 (由 sdkconfig.defaults + idf.py set-target 生成)
├── CMakeLists.txt               # 顶层 CMake (设置目标芯片 ESP32-S3)
│
├── build/                       # 构建输出 (idf.py build 生成, .gitignore)
├── managed_components/          # ESP-IDF 组件管理器下载的第三方库 (自动生成, .gitignore)
│   └── espressif__cjson/        # cJSON — JSON 解析库 (用于解析 AI API 返回)
└── dependencies.lock            # 组件版本锁定文件 (自动生成, .gitignore)
```

### 关键路径说明

| 路径 | 类型 | 说明 |
|------|------|------|
| `managed_components/` | 自动生成 | IDF 组件管理器根据 `idf_component.yml` 下载的依赖，`gitignored` |
| `dependencies.lock` | 自动生成 | 锁定依赖的精确版本（cJSON 1.7.19, IDF 6.0.2），`gitignore` |
| `build/` | 自动生成 | CMake + ninja 构建产物，`gitignore` |
| `sdkconfig` | 半自动 | 由 `sdkconfig.defaults` + `idf.py set-target esp32s3` 生成，`gitignore` |
| `.clangd` | IDE 支持 | 告诉 clangd 移除 `-f*` `-m*` 等交叉编译 flag，方便代码补全 |

## 核心代码逻辑

### 1. 主流程 (`main.c`)

```
┌─────────────────────────────────────────────────────────────┐
│                      设备上电                                │
│                         │                                   │
│              ▼                                               │
│     ① 初始化 NVS + WiFi 管理器 + 按键 GPIO0 + EPD            │
│                         │                                   │
│              ▼                                               │
│     ② 检查 NVS 中有无 WiFi 凭证                              │
│        ├── 有 → ③ 尝试 STA 连接                             │
│        └── 无 → 启动 SoftAP 配网模式 (ThinkINK-XXXX)         │
│                         │                                   │
│              ▼                                               │
│     ③ 如果 STA 连接成功且还未执行过 Pipeline                  │
│        → http_test_baidu()  验证外网连通性                    │
│        → test_ai_image()   完整 AI 图片 Pipeline             │
│                         │                                   │
│              ▼                                               │
│     ④ 主循环 (每 500ms):                                    │
│        - 检查配网页是否提交 → 保存凭证 → 重连 WiFi            │
│        - 检查按键长按 → 进入配网模式                          │
└─────────────────────────────────────────────────────────────┘
```

### 2. AI 图片 Pipeline (`test_ai_image()`)

```
Step 1  ─ ai_generate_image(prompt, result)
│         POST 到混元 hy-image-v3 API
│         携带 size=854x512 + 中文 Prompt
│         解析 JSON → 提取 data[0].url (COS 下载 URL)
│
Step 2~5 ─ image_download_and_decode(url, &rgb565)
│         ① HEAD 请求探测 Content-Length → 预分配 PSRAM buffer
│         ② GET 下载 PNG 字节流 → 指数扩容 PSRAM accumulator
│         ③ lodepng_decode32 → RGBA (走 DRAM，自动路由 PSRAM)
│         ④ 释放 PNG 下载缓冲
│         ⑤ 居中裁剪到 5:3 (800:480) 比例
│         ⑥ 双线性插值缩放到 800×480 → RGB565
│         ⑦ 释放 RGBA 缓冲
│
Step 6  ─ dither_rgb565_to_epd(rgb565, bw, red)
│         Stucki 误差扩散 → 三色量化 (黑/白/红)
│         输出 BW + RED 位图 (各 48000 字节)
│
Step 7  ─ EPD 写入 & 刷新
          epdWriteRED → epdWriteKW → epdUpdate → epdDown
```

### 3. 内存管理策略

| 缓冲区 | 分配器 | 内存池 | 典型大小 | 生命周期 |
|--------|--------|--------|----------|----------|
| PNG 下载 accum | `heap_caps_malloc(SPIRAM)` | PSRAM | 1.3~2.8 MB | 下载→解码 |
| lodepng RGBA | `malloc()` → 自动 PSRAM | DRAM/PSRAM | 1.7~4.0 MB | 解码→缩放 |
| RGB565 输出 | `malloc()` → 自动 DRAM | DRAM | 768 KB | 缩放→抖动→显示 |
| EPD BW/Red | `calloc()` | DRAM | 各 48 KB | 抖动→写入 |
| Dither 误差行 | `calloc()` | DRAM | ~29 KB | 抖动期间 |

关键：PNG accum 固定走 PSRAM，lodepng 用 `malloc()` 自动路由到 DRAM（大块），两者错开避免争用。

### 4. 抖动算法 (`dither.c`)

- **Stucki 误差扩散** — 每像素量化后误差按 42 分分母扩散到 12 个相邻像素
- **三色量化**：输入 RGB 与 {黑, 白, 红} 计算加权感知距离，选最近者
- **红色判定**：需同时满足绝对 R 阈值 + R 主导 G/B + HSV 饱和度 & 色相检查
- **红色惩罚**：附加距离惩罚避免红色过度扩散
- **对比度增强**：量化前对像素做亮度拉伸 (`IMAGE_CONTRAST=1.08`)
- 内存仅 ~29KB (3 行 × 3 通道 × 804 浮点数)，不依赖 PSRAM

### 5. COS 证书修复

腾讯云 COS 使用 GlobalSign Atlas R3 OV TLS CA 2026 Q1 中间证书，
该证书链不在 ESP-IDF v6.0.2 默认 bundle 中。
解决：`image/cos_trusted_certs.h` 内嵌中间 CA + 根 CA PEM，下载时指定 `cert_pem`。

## 硬件配置

| 项目 | 配置 |
|------|------|
| 芯片 | ESP32-S3 N16R8 |
| Flash | 16 MB (QIO, 80 MHz) |
| PSRAM | 8 MB (Octal, 80 MHz) |
| EPD 驱动 | UC8179 |
| 分辨率 | 800 × 480 |
| 颜色 | 黑白红 (BW/RED) |
| SPI 总线 | SPI2_HOST (FSPI) |
| 引脚 | SCK=4, MOSI=5, RST=6, DC=7, CS=15, BUSY=16 |
| 按键 | GPIO0 (板载 Boot 按钮, 低有效, 内部上拉) |

## SDK 配置要点 (`sdkconfig.defaults`)

| 配置项 | 值 | 说明 |
|--------|-----|------|
| `CONFIG_SPIRAM_USE_MALLOC` | y | 让 `malloc()` 自动将 >4KB 分配到 PSRAM |
| `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL` | 4096 | 小于等于 4KB 走内部 DRAM |
| `CONFIG_SPIRAM_MODE_OCT` | y | Octal PSRAM 模式 |
| `CONFIG_ESP_MAIN_TASK_STACK_SIZE` | 16384 | 16KB 主任务栈 (TLS 握手需要 ≥12KB) |
| `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE` | y | 启用 CA 证书 bundle (AI API 用) |
| `CONFIG_PARTITION_TABLE_CUSTOM` | y | 自定义分区表 4MB factory |
| `CONFIG_ESPTOOLPY_FLASHSIZE` | 16MB | 16 MB Flash |

## API 配置

| 配置项 | 位置 | 值 |
|--------|------|-----|
| API Key | `main/config/ai_config.h` | `sk-e7PepnmYbu6iPfjNnZo2NpRgYVKcgvDVuKlUpy2eo8KxaVpR` |
| API URL | `main/config/ai_config.h` | `https://tokenhub.tencentmaas.com/v1/wand/hunyuan-image/v3-generation` |
| 模型 | `main/config/ai_config.h` | `hy-image-v3` |
| 生成尺寸 | `main/network/ai_client.c` | `854x512` |
| AI Prompt | `main/config/ai_config.h` | 中文每日艺术作品提示词 |

## 构建 & 烧录

```bash
# 进入项目目录
cd /path/to/esp32

# 激活 ESP-IDF 环境
source ~/Tools/activate_idf_v6.0.sh

# (首次) 设置目标芯片
idf.py set-target esp32s3

# 构建
idf.py build

# 烧录 & 监控
idf.py -p COMx flash monitor
```

## 开发计划

- [x] Phase 1: WiFi 连接 + HTTP 测试 + 离线图片显示
- [x] Phase 2: AI 图片生成 Pipeline (API → 下载 → 解码 → 裁剪 → 缩放 → 抖动 → EPD)
- [ ] Phase 3: 按键事件系统 (短按 → 重新生成, 长按 → 配网)
- [ ] Phase 4: 定时每日生成 (基于 RTC 或 SNTP)
- [ ] Phase 5: 图片缓存 + 历史浏览

## 项目自动生成文件

以下文件/目录由工具自动生成，已加入 `.gitignore`，无需手动编辑：

| 文件/目录 | 生成方式 | 说明 |
|-----------|----------|------|
| `build/` | `idf.py build` | CMake 构建产物 |
| `managed_components/` | `idf.py build` (首次) | 从 `idf_component.yml` 下载的第三方组件，当前仅 `espressif/cjson` |
| `dependencies.lock` | `idf.py build` (首次) | 精确锁定组件版本号 |
| `sdkconfig` | `idf.py set-target` + 手动调整 | 当前生效的 Kconfig，基于 `sdkconfig.defaults` 生成 |

`.clangd` 文件告诉 clangd (C/C++ 补全引擎) 在代码索引阶段移除 `-f*` `-m*` 等 ESP-IDF 交叉编译参数，避免 VSCode 红色波浪线误报，不影响编译。
