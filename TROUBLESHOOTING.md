# 踩坑记录 / 问题与解决办法

本文件记录项目开发、调试过程中遇到的问题、根因和解决办法，方便后续排查。
**遇到新问题，追到原因后记得补一条。**

按类别分组，每条格式：**症状** → 根因 → 解决。

---

## 一、编译 / 构建错误

### `mbedtls/sha256.h: No such file or directory`
- **根因**：ESP-IDF v6 移除了 `mbedtls/sha256.h` 头文件。
- **解决**：改用 MD 抽象接口 —— `mbedtls/md.h` + `mbedtls_md_info_from_type(MBEDTLS_MD_SHA256)`。

### `HTTPD_409_CONFLICT undeclared`
- **根因**：ESP-IDF 的 httpd 没有 409 状态码常量。
- **解决**：改用 `HTTPD_500`（或直接写数字）。

### `-Werror=comment` 报错
- **根因**：注释里出现了 `/*` 序列（例如写路由注释 `/api/display/*` 这种）。
- **解决**：改写注释，避免在注释文本里出现 `/*`。

### `format-truncation` 警告转错误
- **根因**：`snprintf` 目标缓冲区太小，编译器判定可能截断（int 范围 / 字段最大长度）。
- **解决**：放大对应缓冲区（如 `today[40]`、`source[320]`、`auth_hdr[320]`、`sub[320]`）。

### `run_sd_pipeline implicit declaration`
- **根因**：重构时删掉了 `run_sd_pipeline`，但还有调用点没改。
- **解决**：统一收敛到 `show_next_content()`。

### `button.h: No such file`
- **根因**：低功耗重构时把 `input/button.c` 从构建里移除了。
- **解决**：加回 `input/button.c` 到 `main/CMakeLists.txt`。

---

## 二、SD 卡 / 文件系统

### log.txt 一直是空的（或只有一条）
- **根因**：FATFS 的 `fopen("a")` 追加模式**不会真正落盘**（不 flush）。
- **解决**：改用 POSIX `open(O_APPEND) + write + fsync`，每写一行 `fsync` 一次（见 `log_to_sd.c`）。

### mark_used 报 `rename failed`
- **根因**：FATFS 的 `rename()` **不能覆盖已存在的目标文件**。
- **解决**：先 `unlink()` 旧文件，再 rename（见 `image_library.c` / `quote_library.c`）。

### 加 fsync 后 btn_task 栈溢出
- **根因**：`vprintf` 日志钩子里的 `fsync` 跑在**调用者**栈上，btn_task 原来 2KB 不够。
- **解决**：把 btn_task 栈加大到 8KB（`button.c` 里 `xTaskCreate` 的 8192）。

---

## 三、显示 / SPI

### SPI 报 `txdata transfer > hardware max supported len`
- **根因**：ESP32-S3 SPI DMA 单次传输有上限，一次发 48000 字节超了。
- **解决**：分块发送，每块 8192 字节（`SPI_CHUNK_MAX`），整块期间 CS 保持拉低（`SPI_TRANS_CS_KEEP_ACTIVE`，见 `io.c`）。

### 想加快刷屏速度
- **结论**：写数据（96KB @ 10MHz ≈ 77ms）可忽略，大头是三色屏刷新（DRF，约 20~30s），这是**物理限制**。
- SPI 从 4MHz 提到 10MHz 对写数据有帮助，对刷新时间没影响。
- **不要动自定义 LUT**：三色屏不支持快速刷新（GxEPD2 对 GDEY075Z08 标 `hasFastPartialUpdate=false`），手写波形表风险高（花屏/残影/损坏）。

### 竖图文字方向反了
- **根因**：图片整体旋转了 180°，但文字没跟着转。
- **解决**：先叠加文字，再对「图片 + 文字」整体做 180° 旋转（`display_rgba()` 里统一处理）。

---

## 四、WiFi

### 首次连不上 WPA2 路由
- **根因**：默认 authmode 是 `WPA2_WPA3_PSK`，其阈值过滤掉了纯 WPA2 AP（recv=3 < thresh=7）。
- **解决**：authmode 轮换顺序改成 **WPA2 优先**：`WPA2_PSK → WPA2_WPA3_PSK → WPA3_PSK → WPA_WPA2_PSK`（见 `wifi_manager.c`）。

### WiFi 发射功率
- 之前调低过（`esp_wifi_set_max_tx_power`），为缓解 brownout 后已恢复默认 20dBm。

---

## 五、OTA

### OTA 报 `not found otadata`
- **根因**：分区表少了 `otadata` 分区。
- **解决**：在 `partitions.csv` 补上 `otadata`（8KB）。

---

## 六、低功耗 / 唤醒

### brownout 掉电复位（reset reason = 9）
- **根因**：电池供电时 WiFi 启动 / EPD 刷新 / 图片解码的电流尖峰把电压拉低。
- **解决**：
  - 软件缓解：降功率、降低 CPU 频率。
  - 硬件根治：换更大电流降压板 + 降压板**输出端**并 470~1000µF 电容（电解电容，10V 即可，焊接在降压板输出端 3.3V/GND 上）。

### 按钮从睡眠唤醒后，行为不对（被当成「上电」处理）
- **症状**：按按钮唤醒后，日志显示跑了 `Daily task:`（应该只进 admin 后台）；AI 模式「按一下等 3 分钟」。
- **根因**：`esp_restart()` 会丢掉 `esp_sleep` 的唤醒原因。之前用 `RTC_DATA_ATTR` 保存唤醒原因，但 **ESP32-S3 / IDF v6 下 `RTC_DATA_ATTR` 跨 `esp_restart()` 不保留**。
- **解决**：改存 **NVS（flash）** —— 唤醒后 `nvs_set_u8("wake_cause")` 再重启，启动时读 + 清（见 `sleep_manager.c`）。NVS 一定跨软复位/断电都保住。
- **注意**：读唤醒原因要在 `nvs_flash_init()` 之后（`main.c` 里已把 nvs 初始化挪到最前）。

### 按一下没反应、按多次后一刻不停一直刷新 / 切模式连刷好几张
- **症状**：AI 模式按一下没反应，多按几次后连续刷新停不下来；AI→SD 切换会连刷好几张。
- **根因**：按键**不是队列**（`s_btn_short` 是 bool，多按只会是 true），Web 请求**也不是队列**（`s_request` 是单槽 int，后写覆盖）。真正原因是**刷新期间按键被延迟触发**——按键任务在刷新期间仍在轮询，把标志置 true，主循环要等当前刷新跑完才回来处理，于是每按一次就"续"一次刷新，形成连锁；再叠加 Web 请求（切模式）同时挂着，就连续刷多张。
- **解决**：加 `content_is_busy()` 忙标志，刷新期间按键直接丢弃（`on_btn_short_press` 里 `if (!content_is_busy())` 才置标志）。见 `daily_content.c` / `main.c`。

---

## 七、AI 内容

### 按按钮不重新生成（日志显示 `Already generated today — skipping`）
- **根因**：同「唤醒原因丢失」——按钮唤醒被当成定时/上电，走了每日去重逻辑。
- **解决**：见上一条，用 NVS 保存唤醒原因，让按钮唤醒正确走 GPIO 分支。

### AI 生成的文字来回重复
- **根因**：每次独立生成，模型容易撞车。
- **解决**：把历史生成记录（`ai_history.txt`）发给模型，prompt 里要求避开（见 `ai_client.c`）。

### AI 生成的图片丑 / 跟文字强相关
- **根因**：早期提示词「根据文字内容生成图片」，效果差。
- **解决**：提示词改为**随机主题**（可爱小动物 / 治愈风景 / 美食甜点 / 复古旅行等 8 个主题，`esp_random()` 选），不再依赖文字。

---

## 排查工具速查

| 手段 | 用途 |
|------|------|
| `log.txt`（SD 卡） | 每次上电追加的完整运行日志，电池供电也能查 |
| 串口 `wakeup cause = N` / `reset reason = N` | 判断唤醒来源 / 复位原因 |
| reset reason 含义 | 1=上电 2=外部 3=软复位 5/6/7=看门狗 8=深睡唤醒 9=brownout |
| Web 日志页 | 直接看 SD 卡 log.txt |
