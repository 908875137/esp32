# ThinkINK EPD — AI/SD 双模式数字艺术相框（ESP32-S3）

为 **7.5 英寸黑白红三色电子墨水屏**（UC8179 驱动）设计的**低功耗**数字艺术相框，
基于 ESP32-S3 N16R8 + ESP-IDF v6.0.2。

电池供电，睡眠为主，每天定时醒来刷新一次，或按按钮唤醒打开 Web 管理页。

支持两种内容来源，共享同一套「解码 → 裁剪 → 缩放 → 文字叠加 → 抖动 → 显示」流程：

- **AI 模式**：DeepSeek 生成每日一句 → 混元生成艺术图片 → 叠加文字 → 显示
- **SD 模式**：从 SD 卡本地图片库随机/顺序选图 + 本地文本库选一句，完全离线

> 详细硬件接线见 [HARDWARE.md](HARDWARE.md)。
> 开发/调试踩过的坑见 [TROUBLESHOOTING.md](TROUBLESHOOTING.md)。

---

## 项目结构

```
esp32/
├── main/
│   ├── main.c                    # 入口 — 低功耗唤醒分发 + 每日任务 + Web 后台
│   ├── CMakeLists.txt            # 源码 / 头文件路径 / 组件依赖
│   ├── idf_component.yml         # 组件依赖 (cjson, freetype, mdns)
│   │
│   ├── display/                  # 显示子系统
│   │   ├── epd.c/h               # UC8179 驱动 (初始化对齐 GxEPD2 / 窗口写入 / 刷新 / 睡眠)
│   │   ├── io.c/h                # SPI (FSPI/SPI2_HOST) + GPIO，整块发送
│   │   └── dither.c/h            # OKLab + Burkes 三色抖动 (RGB565 → 黑白红)
│   │
│   ├── network/
│   │   ├── wifi_manager.c/h      # STA 连接 (多 authmode 轮换) + SoftAP 配网
│   │   └── ai_client.c/h         # DeepSeek 每日一句 + 混元图片生成
│   │
│   ├── image/                    # 图片处理 (URL + SD 共用)
│   │   ├── image_decoder.c/h     # 下载/文件 → lodepng → 裁剪 → 缩放 → RGBA/RGB565
│   │   ├── image_processing.c/h  # 预处理链 (曝光/对比度/饱和度)
│   │   ├── color_space.c/h       # OKLab 色彩空间 + LUT
│   │   └── lodepng.c/h           # 单文件 PNG 解码器
│   │
│   ├── text/text_renderer.c/h    # FreeType 中文渲染 (底部叠加 + 竖图侧边竖排)
│   │
│   ├── storage/
│   │   ├── sd_card.c/h           # SD 卡持久挂载
│   │   ├── image_library.c/h     # 图片库 (8位ID + 分目录 + hash 去重 + images.csv)
│   │   ├── quote_library.c/h     # 文本库 (quotes.csv 流式解析 + 选择 + 去重)
│   │   ├── daily.c/h             # 每日记录 (daily.csv)
│   │   ├── config_store.c/h      # NVS 持久化 + config.json 读取/写回
│   │   └── log_to_sd.c/h         # 日志镜像到 SD 卡 log.txt
│   │
│   ├── ota/ota_manager.c/h       # 固件 OTA 升级 (esp_ota_ops 双分区 + 回滚)
│   ├── content/daily_content.c/h # 内容编排 (选图选句 + AI pipeline + 每日任务)
│   ├── power/sleep_manager.c/h   # light sleep + RTC 定时 + GPIO48 唤醒
│   ├── input/button.c/h          # 按键轮询 (醒着时短按切图)
│   ├── web/
│   │   ├── admin_server.c/h      # mDNS + 静态文件 + 核心 helper + 路由注册
│   │   ├── api_core.c            # status / current / stats / daily / display / mode
│   │   ├── api_content.c         # 图片 + 语录 + CSV 导出
│   │   ├── api_files.c           # 文件管理 + www 上传 + 日志 + OTA
│   │   └── web_server.c/h        # SoftAP 配网页
│   └── config/
│       ├── ai_config.h           # AI 默认值 (可被 config.json 覆盖)
│       └── ai_config.c           # AI 运行时配置变量
│
├── www/                          # Web 前端 (部署到 SD 卡 /sdcard/www/)
│   ├── index.html
│   ├── js/app.js
│   └── css/style.css
├── partitions.csv                # 双 OTA 分区表
├── sdkconfig.defaults
└── CMakeLists.txt                # 项目名 + 版本号 project(epd-uc8179 VERSION 1.0.0)
```

---

## 低功耗工作方式

```
上电 / 唤醒
  ↓
读唤醒原因 (esp_sleep_get_wakeup_causes)
  ├── GPIO48 按钮唤醒 → 连 WiFi + 开管理后台 15 分钟
  ├── RTC 定时唤醒    → 每日任务 (AI 生成 或 SD 轮换)
  └── 上电 (UNDEFINED) → 每日任务 + 开管理后台 15 分钟
  ↓
任务完成后 → 配置定时唤醒 + GPIO 唤醒 → light sleep
  ↓
(唤醒后 esp_restart() 干净重启，重新走一遍)
```

- **light sleep** 约 130µA + 降压板静态 60µA，待机目标 3 个月
- GPIO48 非 RTC GPIO，故用 light sleep 的 `gpio_wakeup_enable` 而非 deep sleep

---

## 图片库（images 目录）

```
/sdcard/
├── images/
│   ├── 000/00000001.png   # 全局递增 8 位 ID，每 1000 张一个子目录
│   ├── 000/00000002.png
│   └── 001/00001001.png
├── images.csv              # id,hash,used_count,last_used_at
├── quotes.csv              # id,text,author,source,used_count,last_used_at
├── daily.csv               # date,image_id,quote_id  (当天固定内容)
├── config.json
├── log.txt                 # 运行日志 (每次上电追加)
├── fonts/NotoSansCJKsc-Subset.otf
└── www/                    # Web 前端 (index.html / js/app.js / css/style.css)
```

- 图片通过 Web 上传导入，自动分配 ID、算 SHA-256、去重、写 images.csv
- 文本库 quotes.csv 可直接用电脑编辑、或 Web 上传/导入
- daily.csv 保证「同一天重启不换内容」，第二天自动换新

---

## config.json 格式

```json
{
    "mode": "ai",
    "refresh_hour": 8,
    "font": "/sdcard/fonts/NotoSansCJKsc-Subset.otf",
    "wifi_ssid": "你的WiFi",
    "wifi_password": "密码",
    "ai_api_key": "sk-...",
    "ai_chat_url": "https://tokenhub.tencentmaas.com/v1/chat/completions",
    "ai_image_url": "https://tokenhub.tencentmaas.com/v1/wand/hunyuan-image/v3-generation",
    "ai_chat_model": "deepseek-v4-pro-202606",
    "ai_image_model": "hy-image-v3"
}
```

| 字段 | 说明 |
|------|------|
| `mode` | `"ai"` / `"sd"` |
| `refresh_hour` | 每日刷新小时 (0-23) |
| `font` | 中文字体路径 |
| `wifi_ssid` / `wifi_password` | WiFi 凭证（写 NVS 后生效，可拔卡改） |
| `ai_*` | AI API 配置（覆盖 ai_config.h 内置默认值） |

> 不写 `wifi_*` / `ai_*` 字段就用内置默认值。改 key / 换模型拔卡改 config.json 即可，无需重编译。

---

## 按键

| 动作 | 行为 |
|------|------|
| 短按 (醒着时) | 切换下一张图片（AI 模式重新生成 / SD 模式换图+句） |
| 睡眠时按下 | 唤醒设备 → 打开 Web 管理后台 |

> 按键为 GPIO48（接 GND，非自锁，按下接地），内部上拉。

---

## Web 管理后台

设备唤醒后连 WiFi，通过 `http://thinkink.local`（mDNS）或直接 IP 访问：

- **首页**：设备状态、当前版本、图片/文本数量、使用统计、上一张/下一张/刷新
- **图片**：分页浏览、上传（PNG ≤500KB、≤50万像素、自动去重）、删除
- **语录**：分页浏览、单条添加、CSV 导入/导出、删除
- **设置**：切换 AI/SD 模式、固件升级、上传前端文件
- **日志**：查看 SD 卡 log.txt
- **文件**：SD 卡文件管理（浏览/上传/下载/删除，排除 images/）

---

## 固件 OTA 升级

- 双 OTA 分区（`ota_0` / `ota_1` 交替），`factory` 作为初始/救援镜像，OTA 永不覆盖
- Web 设置页上传 `.bin` → 流式写入备用分区 → 校验 → 切启动分区 → 自动重启
- 新固件启动后自检通过才确认有效，失败则 bootloader 自动回滚
- 版本号在根 `CMakeLists.txt` 的 `project(epd-uc8179 VERSION 1.0.0)`，改这里发新版本

### 分区表（16MB Flash）

```
nvs(24KB) + phy_init(4KB) + factory(4MB) + ota_0(4MB) + ota_1(4MB) + otadata(8KB)
```

> 改分区表后需 USB 完整烧录一次，之后才能走 OTA。

---

## 构建 & 烧录

```bash
cd /path/to/esp32
# 激活 ESP-IDF 环境后
idf.py build
idf.py -p COMx flash monitor
```

- 首次烧录（或分区表改动后）：`idf.py -p COMx erase-flash flash`（会清 NVS，需重新配网）
- 之后可通过 Web OTA 升级，无需 USB

---

## 硬件配置

| 项目 | 配置 |
|------|------|
| 芯片 | ESP32-S3 N16R8 |
| Flash / PSRAM | 16MB (QIO) / 8MB (Octal) |
| EPD | UC8179 7.5" 800×480 三色 |
| EPD SPI | SPI2_HOST (FSPI)，GPIO4 SCK / 5 MOSI / 15 CS / 7 DC / 6 RST / 16 BUSY |
| SD SPI | SPI3_HOST (HSPI)，GPIO19 MISO / 20 CLK / 21 MOSI / 47 CS |
| 按键 | GPIO48（低功耗唤醒 + 醒着短按切图） |

---

## 内存管理策略

| 缓冲区 | 内存池 | 典型大小 |
|--------|--------|----------|
| PNG 下载/SD 读取 buffer | PSRAM | 0.5~2.8 MB |
| RGBA8 输出 (800×480) | PSRAM | 1.5 MB |
| RGB565 输出 | DRAM/PSRAM | 768 KB |
| EPD BW/Red | DRAM | 各 48 KB |

关键原则：大块走 PSRAM、用完即释放、图片库/文本库全程流式（不整体载入 RAM）。

---

## 已知限制 / 注意事项

1. **SD 图片仅支持 PNG**（lodepng 解码，jpg 需引入 esp_jpeg）
2. **AI 模式的「每天固定」依赖时钟**：SD 模式不联网，时间无基准时退化为「每次随机，不写 daily.csv」
3. **字体必须放 SD 卡** `/sdcard/fonts/NotoSansCJKsc-Subset.otf`
4. **brownout 掉电**：电池供电时 WiFi 启动/图片解码的电流尖峰可能触发掉电复位（reset reason 9），需降压板输出端并 470~1000µF 电容，并已降 WiFi 发射功率缓解
5. **固件签名**：OTA 只做 project_name 校验 + 镜像格式校验，未启用安全启动/签名，LAN 内靠 token 做基本访问限制，非强防篡改

---

## 项目自动生成文件

| 文件/目录 | 生成方式 |
|-----------|----------|
| `build/` | `idf.py build` |
| `managed_components/` | 下载的第三方组件 (cjson, freetype, mdns) |
| `dependencies.lock` | 组件版本锁定 |
| `sdkconfig` | 当前生效 Kconfig |
