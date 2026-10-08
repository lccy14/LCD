# ESP32-S3 LCD 多功能固件

基于 **ESP32-S3 + ST7789（320×240，8 位并行 I80 接口） + FT6336 电容触摸 + microSD 卡** 的多功能设备固件，
使用 **LVGL v9** 图形库，在 320×240 的小屏上集成了文件浏览、图片查看、音乐播放器、天气、时钟闹钟、
WiFi/BLE 扫描、电脑监控、贪吃蛇、小说阅读、设置等模块。

---

## 1. 硬件要求

| 部件 | 说明 |
| --- | --- |
| 主控 | ESP32-S3（**需带 PSRAM**，板载 8MB Flash） |
| 屏幕 | ST7789，320×240，8 位并行（Intel I80）接口 |
| 触摸 | FT6336（I2C，地址 `0x38`） |
| 存储 | microSD 卡座，走 **SDMMC** 主机（非 SPI） |
| 其它 | 3.3V LDO 供电（建议 ≥500mA）、背光 PWM（LEDC 1kHz） |

**完整引脚接线见 [`wiring_diagram.txt`](./wiring_diagram.txt)。** 关键点：

- LCD 走 8 位并行总线（`GPIO6/7/8/17/18/19/20/3` 为 D0~D7，`GPIO35/36/37/21/38` 为 DC/WR/CS/RST/BLK），**不是 SPI**。
- LCD 的 `RD` 引脚必须在硬件上拉到 3.3V。
- 触摸 I2C 已启用内部上拉，无需外部电阻。
- SD 卡需预先格式化为 **FAT32**；代码先试 4 线模式，失败自动回退 1 线（只接 D0 也能用）。

---

## 2. 软件环境

- **ESP-IDF v6.0.1**（也在 ESP-IDF v5.3+ 上应可编译，未逐一验证）
- **LVGL v9**（通过 `managed_components` 引入）
- 推荐 **VS Code + Espressif ESP-IDF 插件**

> 关键配置（内存/中文/图片解码）已写在 [`sdkconfig.defaults`](./sdkconfig.defaults) 中，
> 内部 DRAM 非常紧张，请勿随意改小其中的 LVGL 内存池、图片缓存、PSRAM 预留等项。

---

## 3. 构建与烧录

### 命令行

```bash
# 1. 激活 ESP-IDF 环境（按你的安装方式，例如）
. $IDF_PATH/export.sh          # Linux/macOS
# idf.py set-target esp32s3    # Windows 用 Espressif 提供的 export.bat

# 2. 配置目标并编译
idf.py set-target esp32s3
idf.py build

# 3. 烧录并打开监视器（/dev/ttyUSBx 换成你的串口号）
idf.py flash monitor
```

### VS Code（ESP-IDF 插件）

1. `Ctrl+Shift+P` → **ESP-IDF: Set Espressif Device Target** → 选 `esp32s3`。
2. **ESP-IDF: Build** 编译。
3. **ESP-IDF: Flash** 烧录；**ESP-IDF: Monitor** 看日志。

分区表见 [`partitions.csv`](./partitions.csv)（`factory` 6MB，给中文字库留空间；`storage` 1.94MB 预留给小说文本）。

---

## 4. SD 卡准备

1. 把 microSD 卡格式化为 **FAT32**。
2. 在根目录直接放文件：
   - 图片：**`.jpg` / `.jpeg` / `.png`**（JPG 用内置 tjpgd 自解码，PNG 走 LVGL）。**不支持 BMP。**
   - 音乐：任意音频文件（通过文件管理器打开后进入音乐播放器，见第 5 节）。
3. 支持**中文文件名**（FATFS 已设为 UTF-8 + CP936）。

---

## 5. 功能与使用

开机进入**主界面**（图标网格），点击图标进入对应应用。

### 文件管理器
- 浏览 SD 卡 `/sdcard` 目录。
- 图片文件（JPG/PNG）点击可直接**查看**，自动缩放并居中到 320×240。
- 音频文件点击进入**音乐播放器**。
- 大图（解码结果超过 320×240 所需内存）会提示转成合适尺寸。

### 图片查看
- 支持 **JPG / PNG**；**不支持 BMP**。
- 自动居中显示，超出屏幕部分按比例缩放。

### 音乐播放器
界面：黑胶唱片封面 + 曲名 + 进度条（含当前/总时长）+ 三个按钮（上一首 / 播放·暂停 / 下一首）。
- **上一首 / 下一首**：在 SD 卡音频列表间切换（到末尾停在最后一首）。
- **播放·暂停**：切换播放状态，文字在「播放 / 暂停」间变化，进度条按固定节奏模拟推进。
- ⚠️ **当前为演示状态**：进度是模拟推进，**尚未接音频解码与 I2S 出声**（代码中已预留接入点，确定引脚后可接 MP3 解码器+I2S 输出）。

### 天气 / 时钟 / 闹钟
- 需要 **WiFi 联网**：时钟通过 NTP 同步时间，天气通过在线接口获取。
- 首次使用请在「设置」或对应界面配置 WiFi。

### WiFi 扫描 / BLE 扫描
- 扫描附近的 WiFi 网络 / BLE 设备并显示列表。

### 电脑监控
- 在电脑端运行配套程序（见 `tools/`），经 WiFi 把电脑状态上报到设备显示。

### 贪吃蛇
- 触摸控制的小游戏。

### 小说阅读
- 阅读存放在 `storage` 分区的文本文件（需预先通过工具写入）。

### 设置
- 配置项（WiFi、显示等），具体见源码对应模块。

---

## 6. 目录结构

```
.
├── main/                 # 应用代码
│   ├── ui.c              # 主界面与各应用 UI（图片/音乐/天气/游戏/...）
│   ├── media.c           # 图片解码（JPG 自解码 + PNG）
│   ├── file_mgr.c        # 文件管理器
│   ├── sd_card.c         # SD 卡挂载（SDMMC，4线→1线回退）
│   ├── weather.c / time_sync.c / alarm.c / ble_scan.c / wifi_scan.c / pc_mon.c ...
│   ├── lv_font_cn_16.c   # 中文字库（16px）
│   └── ...
├── lcd/                  # LCD 底层驱动（lcd.c / lcd_init.c / delay.c）
├── assets/               # 图片资源
├── tools/                # 辅助脚本（图片转换、烧录等）
├── wiring_diagram.txt    # 引脚接线图
├── partitions.csv        # 分区表（8MB Flash）
├── sdkconfig.defaults    # 关键配置（内存/中文/图片解码）
└── CMakeLists.txt
```

---

## 7. 已知限制与注意事项

- **音乐不出声**：当前为演示版，进度为模拟推进，未接音频硬件。
- **图片仅 JPG/PNG**：BMP 不支持（屏幕小、解码器误读头易出问题，已主动放弃）。
- **屏幕 320×240**：大图会自动缩放；PNG 整帧解码较占内存，已通过 `sdkconfig.defaults` 调大 LVGL 缓存。
- **内部 DRAM 紧张**：LVGL 内存池、图片缓存、PSRAM 预留等已在 `sdkconfig.defaults` 优化，改动需谨慎。
- **SD 卡**：必须 FAT32；4 线模式对走线长度/上拉敏感，不稳定时自动降速或回退 1 线。

---

## 8. 常见问题

**Q：编译报内存不足 / DRAM 不够？**
A：确认 `sdkconfig.defaults` 已生效（重新 `idf.py build` 或 `idf.py reconfigure`）。不要改小里面的 LVGL 内存池与 PSRAM 预留。

**Q：图片打开全黑？**
A：多半是 LVGL 图片缓存不够大，导致整帧解码失败。检查 `sdkconfig.defaults` 中 `CONFIG_LV_CACHE_DEF_SIZE`（应 ≥ 单张解码结果，320×240×4 ≈ 307KB）。

**Q：中文文件名显示成 `?` 或乱码？**
A：SD 卡 FATFS 须用 UTF-8 + CP936（已在 `sdkconfig.defaults` 开启）。确认未改这两项。

**Q：SD 卡识别不了？**
A：确认卡为 FAT32；检查接线（尤其 SD CLK/CMD/D0）；4 线不稳可只接 D0 走 1 线模式。

---

## 9. 许可证

ESP-IDF 与 LVGL 各自遵循其开源许可证；本项目代码许可证见仓库说明。
