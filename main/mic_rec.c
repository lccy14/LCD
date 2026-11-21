#include "mic_rec.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>

#include "board.h"
#include "audio_player.h"     /* 录音开始前停播放，避免回授与 SD/CPU 争抢 */
#include "sd_card.h"          /* sd_card_is_mounted / SD_MOUNT_POINT */
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "driver/i2s_std.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

static const char *TAG = "MIC";

volatile mic_state_t g_mic;

/* ---------------- 可调参数 ---------------- */
/* 16kHz / 16bit / 单声道 = 32KB/s，SD 卡在低速档也写得过来。
 * 想要更高音质可改 32000 或 44100，但注意 SD 带宽与 DMA/缓冲占用。 */
#define MIC_SAMPLE_RATE   16000
#define MIC_READ_FRAMES   1024       /* 每块读的帧数，1024 帧 ≈ 64ms */
#define MIC_MAX_SECONDS   300        /* 单次录音上限 5 分钟，防止写满卡 */
/* I2S MEMS 麦克风(INMP441 等)正常说话时输出偏小，这里做整数倍增益。
 * 削波会自动限幅；录音太轻可改成 4~8，太吵改成 1。 */
#define MIC_GAIN          2

/* 内部 DMA 缓冲必须放内部 RAM，按 32bit × 2 slot × 128 帧 ≈ 4KB 配置，
 * 比默认 6×240 帧省一半以上（本项目内部 DRAM 紧张）。 */
#define MIC_DMA_DESC_NUM  4
#define MIC_DMA_FRAMES    128

typedef enum { MIC_CMD_START, MIC_CMD_STOP } mic_cmd_t;

static QueueHandle_t s_q = NULL;
static TaskHandle_t  s_task = NULL;
static i2s_chan_handle_t s_rx = NULL;
static bool          s_rx_enabled = false;
static int32_t      *s_raw = NULL;   /* 32bit 立体声原始帧，PSRAM */
static int16_t      *s_pcm = NULL;   /* 16bit 单声道 PCM，PSRAM */
/* 命令已投递但后台任务还没把 recording 置起来时，连点两下会投两个 START，
 * 第二段会在第一段结束后意外启动。用这个标志把「待启动」也算进状态。 */
static volatile bool s_start_pending = false;

static void publish(void) { g_mic.version++; }

/* volatile char[] 逐字节写，写完才 publish，UI 拿到新 version 时内容已完整 */
static void set_hint(const char *s)
{
    size_t i = 0;
    if (s) for (; i + 1 < sizeof(g_mic.hint) && s[i]; i++) g_mic.hint[i] = s[i];
    g_mic.hint[i] = '\0';
}

static void post_cmd(mic_cmd_t id)
{
    if (!s_q) return;
    /* 队列满时丢最旧一条再投，保证「停止」不被卡住 */
    if (xQueueSend(s_q, &id, 0) != pdTRUE) {
        mic_cmd_t dummy;
        xQueueReceive(s_q, &dummy, 0);
        xQueueSend(s_q, &id, 0);
    }
}

/* ---------------- I2S RX（独立 I2S_NUM_1） ---------------- */
static esp_err_t mic_i2s_prepare(void)
{
    if (s_rx) {
        if (!s_rx_enabled) {
            esp_err_t e = i2s_channel_enable(s_rx);
            if (e == ESP_OK) s_rx_enabled = true;
            return e;
        }
        return ESP_OK;
    }

    i2s_chan_config_t ch = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    ch.dma_desc_num  = MIC_DMA_DESC_NUM;
    ch.dma_frame_num = MIC_DMA_FRAMES;
    esp_err_t err = i2s_new_channel(&ch, NULL, &s_rx);   /* 只要 RX */
    if (err != ESP_OK) {
        s_rx = NULL;
        ESP_LOGE(TAG, "i2s_new_channel(I2S1 RX) failed: %s", esp_err_to_name(err));
        return err;
    }

    /* 数字 MEMS 麦克风要求每 WS 周期 64 个 BCLK（32bit × 2 slot），
     * 所以即使用单声道也要配成 STEREO 32bit，再按 L/R 取其中一个 slot。
     * 数据 24bit 左对齐在 32bit 帧里，取高 16 位就是 16bit PCM。 */
    i2s_std_config_t std = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(MIC_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = MIC_I2S_BCLK_GPIO,
            .ws   = MIC_I2S_WS_GPIO,
            .dout = I2S_GPIO_UNUSED,
            .din  = MIC_I2S_DIN_GPIO,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    err = i2s_channel_init_std_mode(s_rx, &std);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "init std RX failed: %s", esp_err_to_name(err));
        i2s_del_channel(s_rx);
        s_rx = NULL;
        return err;
    }
    err = i2s_channel_enable(s_rx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "enable RX failed: %s", esp_err_to_name(err));
        return err;
    }
    s_rx_enabled = true;
    ESP_LOGI(TAG, "I2S1 RX ready: %uHz 32bit×2slot BCLK=%d WS=%d DIN=%d",
             (unsigned)MIC_SAMPLE_RATE, (int)MIC_I2S_BCLK_GPIO,
             (int)MIC_I2S_WS_GPIO, (int)MIC_I2S_DIN_GPIO);
    return ESP_OK;
}

static void mic_i2s_release(void)
{
    if (s_rx && s_rx_enabled) {
        i2s_channel_disable(s_rx);
        s_rx_enabled = false;
    }
    /* 通道保留不 del：下次录音直接 enable，省去反复申请内部 DMA 内存 */
}

/* ---------------- WAV 头（44 字节标准 RIFF/WAVE PCM） ---------------- */
static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
}

/* data_bytes 先写 0，录完再 fseek 回来回填 */
static bool wav_write_header(FILE *f, uint32_t data_bytes)
{
    uint8_t h[44] = {0};
    memcpy(h + 0,  "RIFF", 4);
    put32 (h + 4,  36 + data_bytes);
    memcpy(h + 8,  "WAVE", 4);
    memcpy(h + 12, "fmt ", 4);
    put32 (h + 16, 16);                          /* fmt chunk 长度 */
    put16 (h + 20, 1);                           /* PCM */
    put16 (h + 22, 1);                           /* 单声道 */
    put32 (h + 24, MIC_SAMPLE_RATE);
    put32 (h + 28, MIC_SAMPLE_RATE * 2);         /* byte rate = rate × 1ch × 16bit */
    put16 (h + 32, 2);                           /* block align */
    put16 (h + 34, 16);                          /* bits per sample */
    memcpy(h + 36, "data", 4);
    put32 (h + 40, data_bytes);
    return fwrite(h, 1, sizeof(h), f) == sizeof(h);
}

/* 找最大编号 +1 作为新录音文件名。
 * 绝不能「找第一个不存在的编号」——旧文件被清理后小编号会空出来被复用，
 * 而 sd_prune_files 按编号保留最大的 N 个，刚录的小编号文件会被当最旧的删掉，
 * 导致录完立刻消失、ASR 打不开文件。编号只增不复用就没这个问题。 */
static bool make_rec_path(char *out, size_t n)
{
    DIR *d = opendir(SD_MOUNT_POINT);
    if (!d) return false;
    int max_id = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        const char *nm = e->d_name;
        if (strncmp(nm, "REC_", 4) != 0) continue;
        size_t len = strlen(nm);
        if (len < 12 || strcmp(nm + len - 4, ".wav") != 0) continue;
        int id = atoi(nm + 4);   /* "REC_0001.wav" → 0001 */
        if (id > max_id) max_id = id;
    }
    closedir(d);
    if (max_id >= 9999) return false;
    snprintf(out, n, "%s/REC_%04d.wav", SD_MOUNT_POINT, max_id + 1);
    return true;
}

/* 32bit×2slot 原始帧 → 16bit 单声道 PCM。
 * 顺带做一个一阶 DC 阻断（I2S MEMS 麦克风有直流偏置），并计算峰值。 */
static int32_t s_dc_prev = 0, s_dc_out = 0;
static size_t convert_block(size_t frames, uint8_t *peak_out)
{
    int32_t peak = 0;
    const size_t slot = MIC_SLOT_RIGHT ? 1 : 0;
    for (size_t i = 0; i < frames; i++) {
        int32_t v = s_raw[i * 2 + slot] >> 16;   /* 取高 16 位 */
        /* y[n] = x[n] - x[n-1] + 0.995·y[n-1]，滤掉直流 */
        s_dc_out = v - s_dc_prev + s_dc_out * 995 / 1000;
        s_dc_prev = v;
        int32_t a = s_dc_out * MIC_GAIN;
        if (a >  32767) a =  32767;
        if (a < -32768) a = -32768;
        s_pcm[i] = (int16_t)a;
        int32_t m = a < 0 ? -a : a;
        if (m > peak) peak = m;
    }
    *peak_out = (uint8_t)((uint32_t)peak * 100 / 32767);
    s_dc_prev = 0; s_dc_out = 0;                 /* 分块处理，重置避免块间突变 */
    return frames * sizeof(int16_t);
}

/* ---------------- 一次录音会话 ---------------- */
static void record_session(void)
{
    char path[128];
    if (!sd_card_is_mounted()) {
        set_hint("SD 卡未挂载"); publish();
        return;
    }
    if (!make_rec_path(path, sizeof(path))) {
        set_hint("录音文件过多"); publish();
        return;
    }

    FILE *f = fopen(path, "wb");
    if (!f) {
        set_hint("无法创建录音文件"); publish();
        return;
    }
    if (!wav_write_header(f, 0)) {
        ESP_LOGE(TAG, "write WAV header failed");
        fclose(f); remove(path);
        set_hint("写文件失败"); publish();
        return;
    }
    if (mic_i2s_prepare() != ESP_OK) {
        fclose(f); remove(path);
        set_hint("麦克风初始化失败"); publish();
        return;
    }

    g_mic.recording     = true;
    g_mic.done          = false;
    g_mic.elapsed_ms    = 0;
    g_mic.bytes_written = 0;
    g_mic.peak          = 0;
    g_mic.last_path[0]  = '\0';
    set_hint(NULL);
    publish();
    ESP_LOGI(TAG, "record start: %s", path);

    const size_t raw_bytes = MIC_READ_FRAMES * 2 * sizeof(int32_t);
    const uint32_t max_bytes = MIC_SAMPLE_RATE * 2 * MIC_MAX_SECONDS;
    uint32_t data_bytes = 0;
    uint32_t last_publish_ms = 0;
    bool ok = true;

    while (data_bytes < max_bytes) {
        mic_cmd_t cmd;
        if (xQueueReceive(s_q, &cmd, 0) == pdTRUE && cmd == MIC_CMD_STOP) break;

        size_t got = 0;
        esp_err_t e = i2s_channel_read(s_rx, s_raw, raw_bytes, &got, 100);
        if (got == 0) {
            if (e != ESP_OK && e != ESP_ERR_TIMEOUT)
                ESP_LOGW(TAG, "i2s read: %s", esp_err_to_name(e));
            continue;
        }
        size_t frames = got / (2 * sizeof(int32_t));
        if (frames == 0) continue;

        uint8_t pk = 0;
        size_t pcm_bytes = convert_block(frames, &pk);
        if (fwrite(s_pcm, 1, pcm_bytes, f) != pcm_bytes) {
            ESP_LOGE(TAG, "SD write failed / too slow, stop recording");
            set_hint("SD 写入失败"); publish();
            ok = false;
            break;
        }
        data_bytes += (uint32_t)pcm_bytes;
        g_mic.bytes_written = data_bytes;
        /* data_bytes 上限约 9.6MB，×1000 会超 uint32_t，必须先转 64 位 */
        g_mic.elapsed_ms    = (uint32_t)(((uint64_t)data_bytes * 1000) /
                                         (MIC_SAMPLE_RATE * 2));
        g_mic.peak          = pk;

        /* 每 ~200ms 才 publish 一次，UI 不必每 64ms 重绘 */
        if (g_mic.elapsed_ms - last_publish_ms >= 200) {
            last_publish_ms = g_mic.elapsed_ms;
            publish();
        }
    }

    mic_i2s_release();

    /* 回填 WAV 头的 RIFF size 与 data size，录出来的文件播放器才能识别 */
    if (ok) {
        fflush(f);
        if (fseek(f, 0, SEEK_SET) == 0) wav_write_header(f, data_bytes);
    }
    fflush(f);
    fclose(f);

    if (!ok || data_bytes == 0) {
        remove(path);
        set_hint("录音未保存"); publish();
    } else {
        size_t i = 0;
        for (; i + 1 < sizeof(g_mic.last_path) && path[i]; i++)
            g_mic.last_path[i] = path[i];
        g_mic.last_path[i] = '\0';
        set_hint("录音已保存"); publish();
        ESP_LOGI(TAG, "record done: %s (%u bytes, %u ms)",
                 path, (unsigned)data_bytes, (unsigned)g_mic.elapsed_ms);
        /* 只保留最近 20 个录音，旧的自动删除，防止 SD 卡被填满 */
        sd_prune_files("REC_", ".wav", 20);
    }
    g_mic.recording = false;
    g_mic.done      = true;
    publish();
}

static void mic_task(void *arg)
{
    (void)arg;
    for (;;) {
        mic_cmd_t cmd;
        if (xQueueReceive(s_q, &cmd, portMAX_DELAY) != pdTRUE) continue;
        if (cmd == MIC_CMD_START) {
            s_start_pending = false;   /* 会话已被接手，允许下次再启动 */
            if (!g_mic.recording) record_session();
        }
        /* STOP 由 record_session() 内部消费；这里的 STOP 直接忽略 */
    }
}

/* ---------------- 对外 API ---------------- */
void mic_rec_init(void)
{
#if MIC_HW_PRESENT
    const uint32_t caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    s_raw = (int32_t *)heap_caps_malloc(MIC_READ_FRAMES * 2 * sizeof(int32_t), caps);
    s_pcm = (int16_t *)heap_caps_malloc(MIC_READ_FRAMES * sizeof(int16_t), caps);
    if (!s_raw || !s_pcm) {
        ESP_LOGE(TAG, "PSRAM buffers alloc failed, recorder disabled");
        set_hint("录音内存不足"); publish();
        return;
    }
    s_q = xQueueCreate(4, sizeof(mic_cmd_t));
    if (!s_q) { ESP_LOGE(TAG, "cmd queue create failed"); return; }

    /* 栈放 PSRAM：不占紧张的内部 DRAM；没有 minimp3 那种大栈变量，12KB 足够 */
    BaseType_t ok = xTaskCreateWithCaps(mic_task, "mic_rec", 12288, NULL, 4, NULL,
                                        MALLOC_CAP_SPIRAM);
    if (ok != pdPASS) {
        ok = xTaskCreate(mic_task, "mic_rec", 12288, NULL, 4, &s_task);
    }
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "recorder task create failed");
        return;
    }
    ESP_LOGI(TAG, "init ok (%uHz 16bit mono -> %s), WS=%d BCLK=%d DIN=%d",
             (unsigned)MIC_SAMPLE_RATE, SD_MOUNT_POINT,
             (int)MIC_I2S_WS_GPIO, (int)MIC_I2S_BCLK_GPIO, (int)MIC_I2S_DIN_GPIO);
#else
    /* 第一版 PCB：IO1/IO2 是 SD D2/D3、IO42 是 SD CMD，不能接麦克风 */
    set_hint("本板未引出麦克风"); publish();
    ESP_LOGW(TAG, "BOARD_REV 1 has no mic pins wired, recorder disabled");
#endif
}

bool mic_rec_available(void)
{
#if MIC_HW_PRESENT
    return s_q != NULL;
#else
    return false;
#endif
}

void mic_rec_start(void)
{
    if (!mic_rec_available() || g_mic.recording || s_start_pending) return;
    s_start_pending = true;
    audio_player_stop();          /* 关功放、停 TX，避免回授啸叫与 SD/CPU 争抢 */
    post_cmd(MIC_CMD_START);
}

void mic_rec_stop(void)
{
    if (!mic_rec_available()) return;
    post_cmd(MIC_CMD_STOP);
}

void mic_rec_toggle(void)
{
    if (!mic_rec_available()) return;
    if (g_mic.recording) mic_rec_stop();
    else                 mic_rec_start();
}

bool mic_rec_is_recording(void) { return g_mic.recording || s_start_pending; }

void mic_rec_ack_done(void)
{
    if (g_mic.done) { g_mic.done = false; publish(); }
}
