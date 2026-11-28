# ESP32-S3 LCD 多功能固件

基于 **ESP32-S3 + ST7789（320×240，8 位并行 I80 接口） + FT6336 电容触摸 + microSD 卡** 的多功能设备固件，
使用 **LVGL v9** 图形库，在 320×240 的小屏上集成了文件浏览、图片查看、音乐播放器、天气、时钟闹钟、
WiFi/BLE 扫描、USB 网络共享、自动熄屏、系统诊断、电脑监控、贪吃蛇、小说阅读、设置等模块。

---

## 1. 硬件要求

| 部件 | 说明 |
| --- | --- |
| 主控 | ESP32-S3R8（内置 8MB Octal PSRAM，板载 8MB Flash）；第三版 PCB 为 `BOARD_REV=3` |
| 屏幕 | ST7789，320×240，8 位并行（Intel I80）接口 |
| 触摸 | FT6336（I2C，地址 `0x38`） |
| 存储 | microSD 卡座，走 **SDMMC** 主机（非 SPI） |
| 其它 | 3.3V LDO 供电（建议 ≥500mA）、背光 PWM（LEDC 1kHz） |

**完整引脚接线见 [`wiring_diagram.txt`](./wiring_diagram.txt)。** 关键点：

- LCD 走 8 位并行总线（`GPIO6/7/8/17/18/19/20/3` 为 D0~D7，`GPIO35/36/37/21/38` 为 DC/WR/CS/RST/BLK），**不是 SPI**。
- LCD 的 `RD` 引脚必须在硬件上拉到 3.3V。
- 触摸 I2C 已启用内部上拉，无需外部电阻。
- SD 卡需预先格式化为 **FAT32**；第二版先试 4 线、失败回退 1 线；第三版固定 1 线（S3R8 的 IO35/36/37 被内部 PSRAM 占用，只接 CLK=39/CMD=38/D0=40）。

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
   - 音乐：**`.wav`（未压缩 PCM，8/16/24/32 位、单/双声道）** 或 **`.mp3`（MPEG Layer III）**，放在根目录即可（见第 5 节）。FLAC/OGG/AAC 暂不支持，不会出现在播放列表。
3. 支持**中文文件名**（FATFS 已设为 UTF-8 + CP936）。

---

## 5. 功能与使用

开机进入**主界面**（图标网格），点击图标进入对应应用。

### 文件管理器
- 浏览 SD 卡 `/sdcard` 目录。
- 图片文件（JPG/PNG）点击可直接**查看**，自动缩放并居中到 320×240。
- 音频文件点击进入**音乐播放器**。
- 大图（解码结果超过 320×240 所需内存）会提示转成合适尺寸。

### USB 模拟U盘
- 设置页「USB模拟U盘」开关：开启后把 SD 卡通过 USB 暴露给电脑，电脑直接读写 `/sdcard` 的文件。
- 硬件：ESP32-S3 USB OTG 用 **GPIO19(D-)/GPIO20(D+)**（第二/三版 PCB 已引出 USB 口；第三版这两脚空闲）；这两个脚与内置 USB Serial/JTAG 冲突，SDK 已关闭 USJ，调试仍走 UART0(GPIO43/44)。
- 独占模型：U盘模式下 SD 卡交给电脑，设备端 FATFS 卸载，音乐/录音/TTS 自动暂停；关闭开关后自动重新挂载 FATFS、恢复功能。切换过程约几百 ms（SDMMC 重新初始化）。
- 实现：`usb_msc.c` 用 `esp_tinyusb` 的 MSC（`tinyusb_msc_new_storage_sdmmc` + `MOUNT_USB`），依赖本地覆盖的 `components/esp_tinyusb`。切换在主循环做（不卡 LVGL 回调）。
- ⚠️ U盘模式**必须传纯 MSC 描述符**（`s_msc_dev_desc`/`s_msc_cfg_desc`）：因为编译期开了 CDC/NCM/AUDIO，默认描述符变成复合设备，U盘模式下 NCM 未初始化会导致枚举时 `netd_open` / `dfifo_alloc` ASSERT FAILED。四种 USB 模式（U盘/声卡/CDC/网络）各自传纯描述符，不要删。

### USB 声卡（UAC，免驱）
- 设置页「USB声卡(免驱)」开关：开启后电脑把 ESP32 识别为标准 **USB 音频设备**（免装驱动），电脑的喇叭声音从板载功放（MAX98357A）播出，板载麦克风（INMP441）作为电脑的麦克风。
- 规格：**16kHz / 16bit / 单声道**。喇叭端把单声道复制到左右声道；麦克风端读 32bit I2S 取高 16 位转单声道。
- 与 U盘**二选一**（共用 USB 口）：开一个会自动关另一个，开启声卡时音乐/录音/TTS 自动暂停；切换在主循环做。
- 实现：`usb_uac.c` + 本地组件 `components/usb_device_uac`（复用其回调框架与 UAC 描述符宏），底层用本地 `components/esp_tinyusb`（已开 `CFG_TUD_AUDIO`，装在 `components/` 覆盖 managed 版本）。两处采样率必须一致：`tusb_config.h` 的 `CFG_TUD_AUDIO_FUNC_1_MAX_SAMPLE_RATE` 与 `sdkconfig.defaults` 的 `CONFIG_UAC_SAMPLE_RATE`（均 16000）。
- ⚠️ 该组件以 `CONFIG_USB_DEVICE_UAC_AS_PART=y` 模式编译，不自行初始化 TinyUSB/PHY，由 `usb_uac.c` 传自定义 UAC 描述符给 `esp_tinyusb`；I2S0(功放)/I2S1(麦克风)在 UAC 期间由本模块独占。
- 音量：由**电脑**音量面板控制（组件把 -50~0dB 映射成 0~100），`usb_uac.c` 对 PCM 做平方律数字增益；MAX98357A 无硬件音量寄存器。设置页的「喇叭音量」只管本地播放，对声卡无效。

### USB 调试串口（CDC）
- 设置页「USB调试串口」开关：开启后电脑出现一个 **COM 口**（免驱），用于调试与上位机控制。
- **ESP_LOG 日志镜像**：开启后所有 `ESP_LOGx` 日志会同时输出到 USB CDC（原 UART0 照常保留），COM 口就是完整调试日志口——和串口看到的内容一样。实现用 `esp_log_set_vprintf()` 钩子把日志存进静态缓冲，`cdc_task` 定期刷出（不在钩子里直接操作 USB，避免阻塞/重入）。
- 另外：收到的文本**原样回显**；每 5 秒上报一次内部 RAM / PSRAM 空闲量。
- 与 U盘 / 声卡**三选一互斥**（共用 USB 口），切换在主循环做。实现：`usb_cdc.c` 传纯 CDC 描述符给 `esp_tinyusb`，`CONFIG_TINYUSB_CDC_ENABLED=y` + `COUNT=1`。

### USB 网络共享（NCM）
- 设置页「USB网络共享」开关：开启后电脑插 USB 出现一块网卡，**经 ESP32 的 WiFi STA 上网**（相当于 USB 网卡）。
- 前提：先在 **WiFi App 页**打开 WiFi 并连上路由器，否则启动会失败并提示。关闭 WiFi App 页开关时会自动停掉 USB 网络共享；重新打开 WiFi 后需手动再开一次 NCM。
- 实现：`usb_ncm.c` 用 **NAPT 网关**模式（不是 L2 桥）——USB NCM 创建独立 netif（192.168.4.1/24 + DHCP server），电脑拿 192.168.4.x；`esp_netif_napt_enable()` 把 USB 子网流量经 WiFi STA 转发出去。电脑和设备**各用独立 IP**，互不干扰。
- 为什么不用 L2 桥：L2 桥共用 WiFi STA 的 MAC/IP，设备自己收不到 DNS 响应（`getaddrinfo` EAI_AGAIN / 202），或两边 TCP 栈抢同一个 IP 导致都没网（电脑显示 0 bps）。NAPT 是标准"网络共享"方案，设备自己联网不受影响。
- DNS：DHCP server 不再把 DNS 下发成 `192.168.4.1`（设备自身没有 DNS 服务），而是在启动 DHCP 前读取 WiFi STA 的上游 DNS；取不到时回退到 STA 网关（例如 `192.168.1.1`）。电脑 `ipconfig /all` 里 USB 网卡 DNS 应为上游路由/网关。
- TX 缓冲：`usb_netif_transmit()` 交给 `tinyusb_net_send_sync()` 的 buffer 是 lwIP pbuf payload，归 lwIP 所有；不能由 USB 回调 `free()`。当前实现关闭 `free_tx_buffer`，同步发送返回后再由 lwIP 正常释放，避免堆损坏/随机重启。
- 吞吐优化：WiFi STA 已调用 `esp_wifi_set_ps(WIFI_PS_NONE)` 关闭 modem sleep；TCP 默认发送/接收窗口从 5760 提到 16384（见 `sdkconfig` / `sdkconfig.defaults`）。USB Full Speed 原始 12Mbps，实测通常 3~8Mbps，受 WiFi 信号和上游带宽影响。
- ⚠️ 两个版本差异坑：① MAC 字符串索引必须对齐 `STRID_MAC=7`，由 `tinyusb_net_init()` 内部写 ASCII MAC，不要自己构造 UTF-16LE（会被 `strnlen` 截断成空串，枚举卡住）；② 新版 TinyUSB 的 NCM 不调用 `tud_network_init_cb()`，`on_init_callback` 不触发，改用 `ncm_monitor_task` 轮询 `tud_mounted()` 报告枚举状态。
- 与 U盘 / 声卡 / CDC **四选一互斥**（共用 USB 口），切换在主循环做。NTB buffer：OUT=1×2048，IN=3×2048（IN 少于 3 会疯狂丢包）。发送缓冲满的丢包日志降频为每 100 次汇总一次。
- 配置：`CONFIG_LWIP_IP_FORWARD=y` + `CONFIG_LWIP_IPV4_NAPT=y` + `CONFIG_TINYUSB_NET_MODE_NCM=y` + NCM buffer + TCP 窗口 16384（sdkconfig.defaults 持久化）。

### 图片查看
- 支持 **JPG / PNG**；**不支持 BMP**。
- 自动居中显示，超出屏幕部分按比例缩放。

### 音乐播放器
硬件：ESP32-S3 I2S → **MAX98357AETE+T**（Class D 功放，3 线 BCLK/LRCK/DIN，无需 MCLK、无需 I2C 配置）。
引脚在 `main/board.h` 的 `AUDIO_I2S_*` 宏集中定义，按**第二版 PCB** 接线：`BCLK=6 / LRCK=7 / DOUT→DIN=5 / SD_MODE=4`。SD_MODE 由软件控制：播放时拉高（功放工作），暂停/停止时拉低（关断、消除底噪滋滋声）。硬件侧该脚另有 10K 上拉到 VDD，GPIO 输出的低阻抗足以在需要时拉低。
⚠️ `board.h` 顶部有 `BOARD_REV` 开关，三版引脚不兼容，换板必须改对再烧录：
- **`1`** = 第一版（旧引脚，无板载功放/麦克风）
- **`2`** = 第二版（外接 Quad PSRAM，SD 4 线，背光 IO35）
- **`3`**（默认）= 第三版 **ESP32-S3R8**（模组内置 8MB **Octal** PSRAM，SD **1 线**，背光 IO41）

第三版的 IO35/36/37 被内部 PSRAM 占用，绝不能作 SD 的 D2/D1/D3；PSRAM 已切 `SPIRAM_MODE_OCT`（CLK=IO30/CS=IO26）。完整引脚表见 `wiring_diagram.txt`。

界面：黑胶唱片封面 + 曲名 + 进度条（含当前/总时长）+ 音量条 + 四个按钮（上一首 / 播放·暂停 / 下一首 / 录音）。
- 播放链路：开机 `audio_player_init()` 建 I2S 通道与后台任务（栈/缓冲放 PSRAM）→ WAV 直接取 PCM / MP3 经 **minimp3** 解码 → 统一转成 16bit 双声道 → `i2s_channel_write` 真实输出，**进度由实际送出的字节/采样数推进**，到尾自动下一首。
- **WAV**：未压缩 PCM，8/16/24/32 位、单/双声道；时长由头精确计算，seek 精确。
- **MP3**：MPEG Layer III（`main/minimp3.h` 单文件解码器，CC0 公共领域，零外部依赖、约 30KB Flash / 20KB PSRAM）。支持 ID3v2 标签、CBR 与 VBR（Xing/Info/VBRI 帧数）；不支持 Layer I/II、FLAC/OGG/AAC。
  - 时长：有 Xing/VBRI 帧数时精确，纯 CBR 文件按首帧码率估算（可能偏差几秒）。
  - **seek 是近似的**：MP3 没有帧索引，按字节比例定位后重新找帧同步，VBR 文件偏差更明显。
- **上一首 / 下一首**：在 SD 卡音频列表（WAV + MP3）间切换（到末尾停止）。文件管理器里点音频文件也能直接跳转播放。
- **播放·暂停**：真正暂停/恢复 I2S 输出，并关断/开启功放 SD_MODE（暂停时无底噪滋滋声）。
- **拖动进度条**：seek；**音量条**：PCM 软件缩放 0~100%。

### 录音（麦克风）
硬件：I2S 数字 MEMS 麦克风（INMP441 / ICS-43432 等），接**第二版 PCB 预留引出脚**
`WS=GPIO1 / SCK(BCLK)=GPIO2 / SD(数据)=GPIO42`，宏在 `main/board.h` 的 `MIC_I2S_*`。

- 用独立 **`I2S_NUM_1`** 控制器，与功放的 `I2S_NUM_0` 完全隔离；主控给 SCK/WS，麦克风无需 MCLK 或 I2C 配置。
- 音乐屏第四个按钮：**点一下开始、再点停止**。曲名栏实时显示录音时长与电平，进度条临时当电平表。
- 后台任务把 32bit 立体声原始帧按声道取高 16 位、做 DC 阻断和增益，写成 **16kHz / 16bit / 单声道 WAV**（32KB/s），录完回填 RIFF 头，保存到 `/sdcard/REC_0001.wav`（编号递增、不覆盖旧文件），并自动刷新播放列表，可直接点播。每次录完自动清理：只保留最近 20 个 `REC_xxxx.wav`，更早的删除（`sd_prune_files`），防止 SD 卡被填满。
- 单次上限 5 分钟；录音时自动停播放（防回授啸叫），录音中播放请求会被拒绝。
- 全局状态 `g_mic` + `version` 轮询（与播放/天气同构），网络/文件 IO 全在 PSRAM 栈的后台任务里，不碰 LVGL 线程。
- ⚠️ **第一版 PCB 不能接麦克风**（IO1/IO2 是 SD D2/D3、IO42 是 SD CMD）；`BOARD_REV=1` 时 `MIC_HW_PRESENT=0`，录音按钮自动变灰、功能完全禁用。
- 排错（全噪声/静音）：`L/R` 接 3V3 时把 `MIC_SLOT_RIGHT` 改成 `1`；声音太轻把 `mic_rec.c` 的 `MIC_GAIN` 从 `2` 调到 `4~8`。详见 `wiring_diagram.txt` 麦克风章节。

### 天气 / 时钟 / 闹钟
- 需要 **WiFi 联网**：时钟通过 NTP 同步时间，天气通过在线接口获取。
- 首次使用请在 **WiFi App 页**打开 WiFi 并连接路由器。
- 数据源 **sojson**（又拍云 CDN，`t.weather.sojson.com`，国内直连、免 Key）：按城市代码一次 HTTP 取实况（温度/湿度/天气中文描述），响应约 100ms。原 Open-Meteo 域名解析到欧洲 OVH 节点，跨境丢 SYN 导致 15s 超时，已弃用。
  城市名→代码映射内置常见城市（`weather.c` 的 `s_city_codes`），查不到回退深圳；当前固定 `WX_MANUAL_CITY="Shenzhen"`，无城市切换 UI。sojson 只支持明文 HTTP（天气数据不敏感），超时 8s，失败按 60 秒退避重试。
  天气页展示 5 行：城市 / 温度+天气 / 今日温区+风向风力 / 湿度+空气质量+PM2.5 / 更新时间（溢出时曾试过 7 行含日出日落与生活提示，内容过多且提示会换行超出屏幕，已删减）。sojson 实况无精确风速（km/h），用风向+风力等级（如「东北风2级」）代替。无刷新按钮：进入天气页自动拉一次，之后每 10 分钟刷新（失败按 60 秒重试）。

### AI 聊天（多平台可切换）
- 主页「AI聊天」图标进入。**Key 在「设置 → AI聊天 Key / 平台」里配置**：点开浮层，粘贴对应平台的 Key，点保存（Key 已通过 `main/ai_key_local.h` 预置，首次启动自动写入 NVS，一般无需再填）。浮层右上角「切换平台」可循环切换服务商，各平台 Key 独立存储在 NVS（`aichat` 命名空间），不写死在固件里。聊天页本身只保留「清空」按钮。
- 聊天界面为微信式对话气泡：我方消息靠右蓝色、AI 回复靠左灰色，回复到达时直接替换「思考中」气泡，不堆积历史。
- **点「切换平台」按钮循环切换**（标题会显示当前平台）：
  | 平台 | 模型 | 费用 | 申请地址 |
  |---|---|---|---|
  | 智谱 GLM（**默认**） | `glm-4-flash` | **永久免费** | open.bigmodel.cn |
  | DeepSeek | `deepseek-chat` | 低价 | platform.deepseek.com |
  | 硅基流动 | `Qwen/Qwen2.5-7B-Instruct` | 9B 以下免费 | siliconflow.cn |
  三家都是 OpenAI 兼容格式、国内直连；切换后输入框自动载入该平台已存的 Key。
- 点输入框弹出拼音/英文键盘，输入后点「发送」或键盘 Enter；后台异步请求，界面显示「思考中...」，回复返回后自动刷新。支持最近一轮上下文。
- **语音输入（第一步）**：聊天页顶部绿色「语音」按钮，点一下开始录音、再点「停止」结束；录音经智谱 `glm-asr-2512` 识别（复用当前平台的 Key，multipart 上传 WAV），识别出的文字**自动填入输入框**，你确认/修改后再点发送——识别错了也能改，不会一错到底。单次语音建议 ≤18 秒（ASR 上传缓冲上限）。
- **语音输出（TTS 播报）**：用**百度语音合成（免费额度，个人认证 5 万次）**把 AI 回复合成 MP3 并播放。先在百度智能云创建语音合成应用，把 API Key / Secret Key 填到 `main/ai_key_local.h` 的 `BAIDU_TTS_API_KEY` / `BAIDU_TTS_SECRET_KEY`（换 Key 需重编译）。流程：oauth 换 access_token（缓存 30 天）→ POST `tsn.baidu.com/text2audio`（`aue=3` 返回 MP3）→ 流式写 SD（`/sdcard/TTS_xxxx.mp3`）→ `audio_player` 播放。用 MP3 而非 WAV：同长度语音数据量约 1/10，避免 ESP32 TLS 接收大响应时偶发 `read error -0x0087` 截断（曾导致只播第一个字）；下载后按 `Content-Length` 校验完整性，不全则删文件重试。单次限 60 汉字，长回复按字符边界自动分段、串行合成播放。两种触发：① 设置页开「AI回复自动播报」后每条回复自动读；② 关闭时**点 AI 气泡**手动读该条。合成在 PSRAM 后台任务。同理只保留最近 20 个 `TTS_xxxx.mp3`，更早的自动删除，防止 SD 卡填满。
- 拼音输入用 **`main/lv_pinyin_dict_cn.c`** 生成的简体字典（420 个拼音 / 2 万字，替代 LVGL 自带仅 1168 字的繁体字典），常用简体字都可打出；候选栏 `<` `>` 翻页。字典由 `python tools/gen_pinyin_dict.py` 从 pypinyin 数据生成（需 `pip install pypinyin`），自动生成文件勿手改。
- HTTPS + CA bundle 校验，非流式；网络/JSON 缓冲与任务栈全在 PSRAM。单次连接超时 12 秒、失败自动重试 1 次（对抗偶发丢 SYN）。曾因内部 RAM 耗尽在 RSA 验证时 `abort()`，已将音乐路径表/书单挪到 PSRAM、保留 draw buf 320×40×2 不降画质；请求日志打印 `free_internal` 便于诊断。
- ⚠️ Key 以明文存 NVS，仅适合个人设备。免费平台也有频率/额度限制，政策以各家官网为准。

### WiFi 扫描 / BLE 扫描
- 扫描附近的 WiFi 网络 / BLE 设备并显示列表。
- **WiFi 开关在 WiFi App 页表头**：开启时启动 WiFi STA 并扫描；关闭时 `esp_wifi_stop()`，清空状态，并自动停止 USB 网络共享；再次开启会用已保存的账号密码自动重连。
- **BLE 开关在蓝牙 App 页表头**：BLE 默认关闭，不进入蓝牙 App 时不初始化 NimBLE，不占射频和蓝牙内存；进入蓝牙 App 或打开该开关时才初始化并扫描 10 秒；关闭时停止扫描（不反复 deinit 控制器）。

### 系统诊断
- 入口：设置页「系统诊断」。
- 页面每行一个独立标签，按状态着色：蓝色为分组标题，绿色为正常，橙色为警告，红色为异常，灰色为未启用/未连接。
- 显示内容：
  - 运行时长 / 帧率
  - 复位原因 / 任务数
  - 内部 RAM：空闲、历史最低、最大空闲块
  - PSRAM：空闲、最大空闲块
  - WiFi 状态；已连接时附带 IP
  - USB 模式：NCM / CDC / U盘 / 声卡 / 未启用
  - SD 卡：线数、实际时钟、理论带宽
- 内部 RAM 历史最低值使用 `heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)`，不会把 PSRAM 混进去。

### 电脑监控
- 在电脑端运行配套程序（见 `tools/`），经 WiFi 把电脑状态上报到设备显示。

### 贪吃蛇
- 触摸控制的小游戏。

### 小说阅读
- 在 SD 卡根目录新建 **`novels`** 文件夹，把 `.txt` / `.md` 电子书放进去（文件名即书名，自动去扩展名）。
- 进入「小说」会自动扫描该文件夹，书单 = 内置示例书 + 你的书；点书名进入阅读器。
- 左右滑动翻页（左滑下一页 / 右滑上一页），底部显示「当前页 / 总页」。
- 大书首次打开时整体读入 PSRAM 并常驻，再次点开即读；若 SD 卡未挂载会自动尝试挂载。
- 注：PDF 不支持，请先把文字版 PDF 导出为 `.txt`（或 `.md`）再放入 `novels` 文件夹。

### 设置
- **亮度**：10%~100% 滑块，LEDC PWM 调 LCD 背光。
- **自动熄屏**：开关，默认开；30 秒无触摸把背光 PWM 调到 0%，任意触摸唤醒；闹钟触发时也会自动点亮。关闭该开关后不会再自动熄屏。熄屏不修改用户设置的亮度值。
- **喇叭音量**：0%~100% 滑块，`audio_player` 软件数字增益（MAX98357A 无硬件音量控制，靠 I2S 数据缩放）。默认 80%，掉电不保存。
- **AI回复自动播报**：开关，默认关；开启后每条 AI 回复自动 TTS 播报，关闭时可点 AI 气泡手动播。
- **系统诊断**：打开系统诊断页，查看运行时长、帧率、复位原因、任务数、内存/PSRAM 空闲与最大空闲块、WiFi 状态/IP、USB 模式、SD 卡线数/时钟/理论带宽。
- 其他配置项（WiFi、BLE 等）见对应 App 页表头开关与源码模块。

---

## 6. 目录结构

```
.
├── main/                 # 应用代码
│   ├── ui.c              # 主界面与各应用 UI（图片/音乐/天气/游戏/系统诊断/...）
│   ├── audio_player.c    # 播放引擎：WAV/MP3 解码 → I2S0 → MAX98357A
│   ├── mic_rec.c         # 录音引擎：I2S1 麦克风 → 16bit WAV → /sdcard
│   ├── ai_chat.c/.h      # AI 聊天：多平台 HTTPS POST + NVS 分平台 Key + PSRAM 后台任务
│   ├── voice_asr.c/.h    # 语音输入：录音 WAV → 智谱 glm-asr-2512 识别 → 文字
│   ├── voice_tts.c/.h    # 语音输出：AI 回复 → 百度语音合成(免费) → WAV 写 SD → audio_player 播放
│   ├── lv_pinyin_dict_cn.c/.h  # 简体拼音字典（自动生成，2 万字）
│   ├── minimp3.h         # MP3 解码器（单头文件，CC0 公共领域，无外部依赖）
│   ├── minimp3_impl.c    # minimp3 实现编译单元（只在此展开一次）
│   ├── media.c           # 图片解码（JPG 自解码 + PNG）
│   ├── file_mgr.c        # 文件管理器
│   ├── sd_card.c         # SD 卡挂载（1线40MHz 优先，失败回退低速/4线）
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

- **音乐格式**：仅 WAV(PCM) 与 MP3(Layer III)；FLAC/OGG/AAC 需额外解码器。I2S 引脚见 `main/board.h`，接线不对会完全无声（先确认 SD_MODE 是否被拉低关断）。
- **图片仅 JPG/PNG**：BMP 不支持（屏幕小、解码器误读头易出问题，已主动放弃）。
- **屏幕 320×240**：大图会自动缩放；PNG 整帧解码较占内存，已通过 `sdkconfig.defaults` 调大 LVGL 缓存。
- **内部 DRAM 紧张**：LVGL 内存池、图片缓存、PSRAM 预留等已在 `sdkconfig.defaults` 优化。音乐路径表（24KB）与 SD 书单（14KB）在首次进入对应 App 时从 PSRAM 分配（`ui.c` 懒分配），腾出的空间让 LCD draw buf 保持 320×40×2（不降画质）；改动这几处需谨慎，否则 HTTPS 握手时内部 RAM 不足会崩溃。
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

**Q：USB 网络共享能拿到 IP 但上不了网？**
A：在电脑命令行执行 `ipconfig /all`，确认 USB 网卡的 DNS 不是 `192.168.4.1`。当前固件会下发 WiFi STA 的上游 DNS 或上游网关（如 `192.168.1.1`）。如果仍是旧值，禁用/启用 USB 网卡或执行 `ipconfig /release && ipconfig /renew` 重新获取地址。

**Q：USB 网络共享速度偏慢？**
A：ESP32-S3 USB OTG 是 Full Speed（12Mbps 原始速率），经 NCM + NAPT + WiFi 后实测通常约 3~8Mbps。固件已关闭 WiFi modem sleep，并把 TCP 窗口调到 16384；如果仍慢，先确认电脑自身 WiFi 已关闭、流量确实走 USB 网卡，再检查 WiFi RSSI 和上游外网带宽。

**Q：系统诊断页里“内部 RAM 历史最低”比“空闲”大很多？**
A：历史最低必须和空闲是在同一类内存上统计。当前代码用 `MALLOC_CAP_INTERNAL` 只统计内部 RAM；不要改回 `esp_get_minimum_free_heap_size()`，那个接口统计 `MALLOC_CAP_DEFAULT`，会把 PSRAM 混进去。

---

## 9. 许可证

ESP-IDF 与 LVGL 各自遵循其开源许可证；本项目代码许可证见仓库说明。
