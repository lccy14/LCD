#include "usb_uac.h"

#include <string.h>
#include <stdint.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"

#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "usb_device_uac.h"
#include "uac_descriptors.h"     /* TUD_AUDIO_DESCRIPTOR / UAC2_ENTITY_* 宏 */
#include "board.h"
#include "audio_player.h"
#include "mic_rec.h"
#include "voice_tts.h"
#include "usb_msc.h"
#include "usb_cdc.h"
#include "usb_ncm.h"

static const char *TAG = "USB_UAC";

/* ---------------- 接口号（AS_PART 模式下需自己定义） ---------------- */
enum {
    ITF_NUM_AUDIO_CONTROL = 0,
    ITF_NUM_AUDIO_STREAMING_SPK,
    ITF_NUM_AUDIO_STREAMING_MIC,
    ITF_NUM_TOTAL,
};

/* ---------------- UAC 描述符 ---------------- */
#define UAC_EP_OUT   0x01        /* Host → 设备（喇叭） */
#define UAC_EP_FB    0x81        /* 反馈端点 */
#define UAC_EP_IN    0x82        /* 设备 → Host（麦克风） */
#define UAC_CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_AUDIO_DEVICE_DESC_LEN)

static const tusb_desc_device_t s_uac_dev_desc = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = TUSB_CLASS_MISC,
    .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol    = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = 0x303A,
    .idProduct          = 0x8001,
    .bcdDevice          = 0x0100,
    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,
    .bNumConfigurations = 0x01,
};

static const uint8_t s_uac_cfg_desc[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, UAC_CONFIG_TOTAL_LEN, 0x00, 100),
    TUD_AUDIO_DESCRIPTOR(ITF_NUM_AUDIO_CONTROL, 4, UAC_EP_OUT, UAC_EP_IN, UAC_EP_FB),
};

/* 字符串描述符数量固定 7，全部 static 保证生命周期 */
static const char *s_uac_strings[] = {
    (char[]){0x09, 0x04},   /* 0: LANGID */
    "ESP32-S3",             /* 1: Manufacturer */
    "LCD USB Audio",        /* 2: Product */
    NULL,                   /* 3: Serial（运行时填 MAC） */
    "USB Audio",            /* 4: Audio control interface */
    "Speaker",              /* 5: Speaker interface */
    "Microphone",           /* 6: Microphone interface */
};

/* ---------------- 状态 ---------------- */
static bool              s_active = false;
static tinyusb_config_t  s_tusb_cfg = {0};   /* 异步任务只存指针，必须 static */
static i2s_chan_handle_t s_tx = NULL;        /* I2S0 喇叭 */
static i2s_chan_handle_t s_rx = NULL;        /* I2S1 麦克风 */
static bool              s_tx_en = false;
static bool              s_rx_en = false;
static uint32_t          s_sample_rate = 16000;

/* 数字音量（MAX98357A 无硬件音量寄存器，只能软件缩放 PCM）。
 * 组件把电脑音量映射成 0~100 传给 set_volume_cb，这里预计算 Q15 增益避免逐样本算平方。 */
static volatile uint32_t s_volume = 100;        /* 0~100，默认满音量 */
static volatile bool     s_muted = false;
static volatile int32_t  s_gain_q15 = 32768;    /* scale=(v/100)^2，满音量=1.0 */

/* ---------------- I2S0 TX：喇叭（16bit stereo，无 MCLK） ---------------- */
static esp_err_t uac_i2s_tx_init(void)
{
    if (s_tx) return ESP_OK;
    i2s_chan_config_t ch = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    ch.dma_desc_num = 4;
    ch.dma_frame_num = 240;
    esp_err_t err = i2s_new_channel(&ch, &s_tx, NULL);
    if (err != ESP_OK) return err;

    i2s_std_config_t std = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(s_sample_rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = AUDIO_I2S_BCLK_GPIO,
            .ws   = AUDIO_I2S_LRCK_GPIO,
            .dout = AUDIO_I2S_DOUT_GPIO,
            .din  = I2S_GPIO_UNUSED,
        },
    };
    err = i2s_channel_init_std_mode(s_tx, &std);
    if (err != ESP_OK) { i2s_del_channel(s_tx); s_tx = NULL; return err; }

    /* 打开功放（SD_MODE 拉高） */
    if (AUDIO_I2S_SD_MODE_GPIO >= 0) {
        gpio_set_level(AUDIO_I2S_SD_MODE_GPIO, 1);
    }
    err = i2s_channel_enable(s_tx);
    if (err == ESP_OK) s_tx_en = true;
    return err;
}

/* ---------------- I2S1 RX：麦克风（32bit stereo，MEMS 要求 64 BCLK/WS） ---------------- */
static esp_err_t uac_i2s_rx_init(void)
{
    if (s_rx) return ESP_OK;
    i2s_chan_config_t ch = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    ch.dma_desc_num = 4;
    ch.dma_frame_num = 240;
    esp_err_t err = i2s_new_channel(&ch, NULL, &s_rx);
    if (err != ESP_OK) return err;

    i2s_std_config_t std = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(s_sample_rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = MIC_I2S_BCLK_GPIO,
            .ws   = MIC_I2S_WS_GPIO,
            .dout = I2S_GPIO_UNUSED,
            .din  = MIC_I2S_DIN_GPIO,
        },
    };
    err = i2s_channel_init_std_mode(s_rx, &std);
    if (err != ESP_OK) { i2s_del_channel(s_rx); s_rx = NULL; return err; }
    err = i2s_channel_enable(s_rx);
    if (err == ESP_OK) s_rx_en = true;
    return err;
}

static void uac_i2s_deinit(void)
{
    if (s_tx) {
        if (s_tx_en) i2s_channel_disable(s_tx);
        i2s_del_channel(s_tx);
        s_tx = NULL; s_tx_en = false;
    }
    if (s_rx) {
        if (s_rx_en) i2s_channel_disable(s_rx);
        i2s_del_channel(s_rx);
        s_rx = NULL; s_rx_en = false;
    }
    /* 关功放，消除底噪 */
    if (AUDIO_I2S_SD_MODE_GPIO >= 0) gpio_set_level(AUDIO_I2S_SD_MODE_GPIO, 0);
}

/* ---------------- UAC 回调：喇叭（USB → I2S0） ----------------
 * USB 送来 16bit mono（每帧 2 字节），复制到左右声道变 16bit stereo 写 I2S。
 * 同时应用数字音量（平方律 Q15）和静音——MAX98357A 无硬件音量，只能改 PCM。 */
static esp_err_t uac_output_cb(uint8_t *buf, size_t len, void *ctx)
{
    (void)ctx;
    static int16_t stereo[1024];   /* 静态缓冲，避免回调里 malloc */
    size_t frames = len / 2;       /* 16bit mono 采样数 */
    if (frames == 0 || frames > 1024) return ESP_OK;

    bool muted = s_muted;
    int32_t gain = s_gain_q15;

    for (size_t i = 0; i < frames; i++) {
        int16_t s;
        memcpy(&s, buf + i * 2, 2);
        if (muted) {
            s = 0;
        } else if (gain != 32768) {
            int32_t v = ((int32_t)s * gain) >> 15;
            if (v >  32767) v =  32767;
            if (v < -32768) v = -32768;
            s = (int16_t)v;
        }
        stereo[i * 2] = s;         /* 左 */
        stereo[i * 2 + 1] = s;     /* 右 */
    }
    size_t written = 0;
    esp_err_t err = i2s_channel_write(s_tx, stereo, frames * 4, &written, 1000);
    if (err != ESP_OK || written != frames * 4) {
        ESP_LOGW(TAG, "spk write err=%d %u/%u", err, (unsigned)written, (unsigned)(frames * 4));
    }
    return ESP_OK;   /* 不返回错误，避免 USB 流中断 */
}

/* ---------------- UAC 回调：麦克风（I2S1 → USB） ----------------
 * I2S 读 32bit stereo（每帧 8 字节），取左声道高 16 位，输出 16bit mono。 */
static esp_err_t uac_input_cb(uint8_t *buf, size_t len, size_t *bytes_read, void *ctx)
{
    (void)ctx;
    static uint8_t raw[2048];      /* 静态缓冲 */
    size_t want_frames = len / 2;  /* 要输出的 16bit mono 采样数 */
    size_t raw_bytes = want_frames * 8;
    if (raw_bytes > sizeof(raw)) raw_bytes = sizeof(raw);
    if (raw_bytes == 0) { if (bytes_read) *bytes_read = 0; return ESP_OK; }

    size_t got = 0;
    esp_err_t err = i2s_channel_read(s_rx, raw, raw_bytes, &got, 1000);
    if (err != ESP_OK) {
        if (bytes_read) *bytes_read = 0;
        return ESP_OK;
    }
    /* 每 8 字节 = 左32 + 右32（小端），取左声道高 16 位 */
    size_t in_frames = got / 8;
    size_t out_frames = in_frames < want_frames ? in_frames : want_frames;
    for (size_t i = 0; i < out_frames; i++) {
        int32_t sample;
        memcpy(&sample, raw + i * 8, 4);      /* 左声道 32bit */
        int16_t s16 = (int16_t)(sample >> 16); /* 取高 16 位 */
        memcpy(buf + i * 2, &s16, 2);
    }
    if (bytes_read) *bytes_read = out_frames * 2;
    return ESP_OK;
}

/* ---------------- UAC 音量/静音回调 ----------------
 * 组件已把电脑的 -50~0 dB 映射为 0~100 传进来，直接用。
 * MAX98357A 无硬件音量，音量在 output_cb 里对 PCM 做平方律缩放生效。 */
static void uac_set_mute_cb(uint32_t mute, void *ctx)
{
    (void)ctx;
    s_muted = (mute != 0);
    ESP_LOGI(TAG, "电脑设置喇叭静音: %s", s_muted ? "开" : "关");
}

static void uac_set_volume_cb(uint32_t vol, void *ctx)
{
    (void)ctx;
    if (vol > 100) vol = 100;
    s_volume = vol;
    /* 平方律 scale=(v/100)^2，Q15。v=100 时 32768（满增益，output_cb 会跳过缩放） */
    s_gain_q15 = (int32_t)((vol * vol * 32768UL) / 10000UL);
    ESP_LOGI(TAG, "电脑设置喇叭音量: %u%% (增益 Q15=%ld)", (unsigned)vol, (long)s_gain_q15);
}

/* ---------------- 启停 ---------------- */
esp_err_t usb_uac_start(void)
{
    if (s_active) return ESP_OK;

    /* 重置音量状态：每次会话从满音量、非静音开始，避免上次设置残留 */
    s_volume = 100;
    s_gain_q15 = 32768;
    s_muted = false;

    /* 1. 停掉占用 I2S / USB 的现有功能 */
    audio_player_stop();
    mic_rec_stop();
    voice_tts_stop();
    usb_msc_stop();              /* 卸载 U盘、释放 USB（若正在 U盘模式） */
    usb_cdc_stop();              /* 卸载 CDC 调试口（若在调试模式） */
    usb_ncm_stop();              /* 卸载 USB 网络共享（若在网络模式） */

    /* 2. 初始化 I2S */
    esp_err_t err = uac_i2s_tx_init();
    if (err != ESP_OK) { ESP_LOGE(TAG, "I2S TX init failed: %s", esp_err_to_name(err)); return err; }
    err = uac_i2s_rx_init();
    if (err != ESP_OK) { ESP_LOGE(TAG, "I2S RX init failed: %s", esp_err_to_name(err)); uac_i2s_deinit(); return err; }

    /* 3. 注册 UAC 数据处理（AS_PART + skip_tinyusb_init，TinyUSB 由下面 install） */
    uac_device_config_t uac_cfg = {
        .output_cb     = uac_output_cb,
        .input_cb      = uac_input_cb,
        .set_mute_cb   = uac_set_mute_cb,
        .set_volume_cb = uac_set_volume_cb,
        .cb_ctx        = NULL,
        .skip_tinyusb_init = true,
        .spk_itf_num   = ITF_NUM_AUDIO_STREAMING_SPK,
        .mic_itf_num   = ITF_NUM_AUDIO_STREAMING_MIC,
    };
    err = uac_device_init(&uac_cfg);
    if (err != ESP_OK) { ESP_LOGE(TAG, "uac_device_init failed: %s", esp_err_to_name(err)); uac_i2s_deinit(); return err; }

    /* 4. 安装 TinyUSB，传 UAC 描述符（static 保证生命周期） */
    s_tusb_cfg = TINYUSB_DEFAULT_CONFIG(NULL, NULL);
    s_tusb_cfg.descriptor.device = &s_uac_dev_desc;
    s_tusb_cfg.descriptor.string = s_uac_strings;
    s_tusb_cfg.descriptor.string_count = sizeof(s_uac_strings) / sizeof(s_uac_strings[0]);
    s_tusb_cfg.descriptor.full_speed_config = s_uac_cfg_desc;
    err = tinyusb_driver_install(&s_tusb_cfg);
    if (err != ESP_OK) { ESP_LOGE(TAG, "tinyusb install failed: %s", esp_err_to_name(err)); uac_i2s_deinit(); return err; }

    s_active = true;
    ESP_LOGI(TAG, "UAC 声卡已启动（喇叭+麦克风，%u Hz 16bit mono）", (unsigned)s_sample_rate);
    return ESP_OK;
}

esp_err_t usb_uac_stop(void)
{
    if (!s_active) return ESP_OK;
    tinyusb_driver_uninstall();
    uac_i2s_deinit();
    s_active = false;
    ESP_LOGI(TAG, "UAC 声卡已关闭");
    return ESP_OK;
}

bool usb_uac_is_active(void) { return s_active; }
