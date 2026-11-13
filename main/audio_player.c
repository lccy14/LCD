#include "audio_player.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "board.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "minimp3.h"          /* 公共领域单头文件 MP3 解码器 */

static const char *TAG = "AUDIO";

/* 全局状态：后台任务写、UI 只读，靠 version 轮询同步（与 pc_mon.c 同构） */
volatile audio_state_t g_audio;

/* ---------------- 命令通道 ---------------- */
typedef enum {
    CMD_PLAY, CMD_PAUSE, CMD_RESUME, CMD_STOP, CMD_SEEK, CMD_VOLUME
} cmd_id_t;

typedef struct {
    cmd_id_t id;
    int      value;
    char     path[384];        /* 与 ui.c 的 g_music_files[] 路径缓冲等长 */
} audio_cmd_t;

static QueueHandle_t  s_q = NULL;
static TaskHandle_t   s_task = NULL;

/* ---------------- I2S ---------------- */
static i2s_chan_handle_t s_tx = NULL;
static bool               s_tx_enabled = false;

/* MAX98357A 的 SD_MODE：低=关断(静音省电)，高=工作。开机保持关断，避免上电噗声 */
static void amp_enable(bool on)
{
    gpio_num_t pin = AUDIO_I2S_SD_MODE_GPIO;
    if (pin < 0) return;                 /* 宏设为 GPIO_NUM_NC：硬件已上拉，跳过 */
    gpio_set_level(pin, on ? 1 : 0);
}

static esp_err_t i2s_set_rate(uint32_t rate)
{
    if (!s_tx) {
        i2s_chan_config_t ch = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
        /* 默认 6×240 帧 ≈ 5.7KB 内部 DMA 内存，本项目内部 DRAM 紧张，减到 4×240 */
        ch.dma_desc_num  = 4;
        ch.dma_frame_num = 240;
        esp_err_t err = i2s_new_channel(&ch, &s_tx, NULL);
        if (err != ESP_OK) return err;

        i2s_std_config_t std = {
            .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(rate),
            /* 统一输出 16bit / 双声道。MAX98357A 无 MCLK，内部从 BCLK 恢复时钟 */
            .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                             I2S_SLOT_MODE_STEREO),
            .gpio_cfg = {
                .mclk = I2S_GPIO_UNUSED,
                .bclk = AUDIO_I2S_BCLK_GPIO,
                .ws   = AUDIO_I2S_LRCK_GPIO,
                .dout = AUDIO_I2S_DOUT_GPIO,
                .din  = I2S_GPIO_UNUSED,
                .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
            },
        };
        err = i2s_channel_init_std_mode(s_tx, &std);
        if (err != ESP_OK) return err;
    } else {
        if (s_tx_enabled) {
            i2s_channel_disable(s_tx);
            s_tx_enabled = false;
        }
        i2s_std_clk_config_t clk = I2S_STD_CLK_DEFAULT_CONFIG(rate);
        esp_err_t err = i2s_channel_reconfig_std_clock(s_tx, &clk);
        if (err != ESP_OK) return err;
    }
    esp_err_t err = i2s_channel_enable(s_tx);
    if (err == ESP_OK) s_tx_enabled = true;
    return err;
}

/* ---------------- 曲目信息（WAV / MP3 共用） ---------------- */
typedef enum { FMT_WAV, FMT_MP3 } fmt_t;

typedef struct {
    uint32_t rate;
    uint16_t channels;
    uint16_t bits;
    uint32_t data_off;          /* WAV: data 区偏移；MP3: 跳过 ID3v2 后的偏移 */
    uint32_t data_size;         /* 音频数据字节数 */
    uint32_t frame_bytes;       /* WAV: 一帧（全声道）字节数 */
    uint32_t bytes_per_sec;     /* WAV: 字节率 */
    uint32_t samples_per_frame; /* MP3: 每帧每声道采样数（1152 / 576） */
    uint32_t dur_ms;
} track_info_t;

static FILE        *s_file = NULL;
static fmt_t        s_fmt = FMT_WAV;
static track_info_t s_trk;
static uint32_t     s_played = 0;          /* WAV：已从 data 区读出的字节 */
static uint64_t     s_decoded = 0;         /* MP3：已解码的「每声道」采样数 */
static uint8_t      s_volume = 80;

/* 全部缓冲放 PSRAM：内部 DRAM 要留给 LVGL / WiFi / SDMMC DMA。
 * i2s_channel_write 用 memcpy 把用户缓冲拷进内部 DMA 描述符，故源缓冲可在 PSRAM。 */
#define SRC_BYTES   1024
/* MP3 一帧最多 MINIMP3_MAX_SAMPLES_PER_FRAME(2304) 个 int16；
 * WAV 最坏情况 8bit mono 1024B → 1024 帧 × 2ch = 2048 个 int16，也放得下。 */
#define OUT_SAMPLES MINIMP3_MAX_SAMPLES_PER_FRAME
#define MP3_IN_CAP  4096
#define MP3_IN_SAFE (MP3_IN_CAP - 8)      /* 尾部留 8 字节给「末尾伪帧头」 */

static uint8_t  *s_src = NULL;            /* WAV 原始字节 */
static int16_t  *s_out = NULL;            /* 统一输出：16bit 交错双声道 */
static uint8_t  *s_mp3_in = NULL;         /* MP3 流式输入窗口 */
static int       s_mp3_len = 0;
static bool      s_mp3_eof = false;
static int16_t  *s_mp3_pcm = NULL;        /* minimp3 解码输出 */
static mp3dec_t *s_mp3dec = NULL;         /* ≈6.7KB，含 MDCT/QMF 状态，必须常驻 */

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | ((uint16_t)p[1] << 8)); }
static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint32_t be32(const uint8_t *p)   /* ID3/Xing 用大端 */
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void track_reset(void)
{
    if (s_file) { fclose(s_file); s_file = NULL; }
    memset(&s_trk, 0, sizeof(s_trk));
    s_played = 0;
    s_decoded = 0;
    s_mp3_len = 0;
    s_mp3_eof = false;
}

/* ---------------- WAV ---------------- */
static esp_err_t wav_open(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return ESP_ERR_NOT_FOUND;

    uint8_t h[12];
    if (fread(h, 1, 12, f) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) {
        fclose(f);
        return ESP_ERR_INVALID_RESPONSE;
    }

    uint16_t format = 0;
    uint8_t  ck[8];
    while (fread(ck, 1, 8, f) == 8) {
        uint32_t sz = rd32(ck + 4);
        long pos = ftell(f);
        if (!memcmp(ck, "fmt ", 4)) {
            uint8_t fmt[16];
            size_t  n = sz < sizeof(fmt) ? sz : sizeof(fmt);
            if (fread(fmt, 1, n, f) == n && n >= 16) {
                format          = rd16(fmt);
                s_trk.channels  = rd16(fmt + 2);
                s_trk.rate      = rd32(fmt + 4);
                s_trk.bits      = rd16(fmt + 14);
            }
        } else if (!memcmp(ck, "data", 4)) {
            s_trk.data_off  = (uint32_t)pos;
            s_trk.data_size = sz;
            break;
        }
        fseek(f, pos + sz + (sz & 1), SEEK_SET);   /* RIFF chunk 按偶数字节对齐 */
    }

    if (format != 1 || !s_trk.data_size || !s_trk.rate ||
        (s_trk.channels != 1 && s_trk.channels != 2) ||
        (s_trk.bits != 8 && s_trk.bits != 16 && s_trk.bits != 24 && s_trk.bits != 32)) {
        fclose(f);
        return ESP_ERR_NOT_SUPPORTED;
    }

    s_trk.frame_bytes   = (uint32_t)s_trk.channels * (s_trk.bits / 8);
    s_trk.bytes_per_sec = s_trk.rate * s_trk.frame_bytes;
    s_trk.dur_ms        = (uint32_t)(((uint64_t)s_trk.data_size * 1000) / s_trk.bytes_per_sec);
    s_fmt = FMT_WAV;
    s_file = f;
    fseek(f, s_trk.data_off, SEEK_SET);
    return ESP_OK;
}

/* WAV 源 PCM → 16bit 双声道 + 软件音量，写入 s_out，返回字节数 */
static size_t pcm_convert(size_t n)
{
    const int ch  = s_trk.channels;
    const int bps = s_trk.bits / 8;
    size_t frames = n / s_trk.frame_bytes;

    for (size_t i = 0; i < frames; i++) {
        int32_t l = 0, r = 0;
        for (int c = 0; c < ch; c++) {
            const uint8_t *p = s_src + (i * ch + c) * bps;
            int32_t v;
            switch (s_trk.bits) {
            case 8:  v = ((int32_t)p[0] - 128) << 8; break;   /* 8bit WAV 是无符号 */
            case 16: v = (int16_t)rd16(p); break;
            case 24: v = (int16_t)(p[1] | ((uint16_t)p[2] << 8)); break;  /* 取高 16 位 */
            default: v = (int32_t)rd32(p) >> 16; break;                     /* 32bit 取高 16 位 */
            }
            if (c == 0) l = v; else r = v;
        }
        if (ch == 1) r = l;                 /* 单声道复制到左右，避免只接了一边没声 */
        s_out[2 * i]     = (int16_t)(l * s_volume / 100);
        s_out[2 * i + 1] = (int16_t)(r * s_volume / 100);
    }
    return frames * 4;                     /* 16bit × 2ch */
}

/* ---------------- MP3 ----------------
 * minimp3 是「喂一块数据、解一帧」的流式解码器：
 *   mp3dec_decode_frame(dec, in, in_len, pcm, &info)
 *   返回每声道采样数；info.frame_bytes 是本次消耗的输入字节数。
 * 调用方必须按 frame_bytes 把输入窗口前移（保留未消费的尾部），否则会丢帧或死循环。
 * pcm=NULL 是官方支持的「只解析帧头」模式：不解码，info 里照样填好
 * hz / channels / bitrate_kbps / layer，正好用于打开文件时探测格式和估算时长。
 */
static void mp3_refill(void)
{
    if (s_mp3_eof || !s_file || s_mp3_len >= MP3_IN_SAFE) return;
    size_t room = MP3_IN_SAFE - (size_t)s_mp3_len;
    size_t n = fread(s_mp3_in + s_mp3_len, 1, room, s_file);
    s_mp3_len += (int)n;
    if (n == 0 || feof(s_file)) {
        s_mp3_eof = true;
        /* 文件末尾只剩最后一帧时，minimp3 的 mp3d_match_frame 需要「下一帧头」
         * 才能确认同步，否则最后一帧会被丢弃。追加当前帧头的 4 字节副本作为
         * 伪下一帧头，让最后一帧也能正常解出（这 4 字节之后会被当垃圾消费掉）。 */
        if (s_mp3_len >= 4) {
            memcpy(s_mp3_in + s_mp3_len, s_mp3_in, 4);
            s_mp3_len += 4;
        }
    }
}

static esp_err_t mp3_open(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return ESP_ERR_NOT_FOUND;

    fseek(f, 0, SEEK_END);
    long fsz = ftell(f);
    if (fsz < 32) { fclose(f); return ESP_ERR_NOT_SUPPORTED; }

    /* 先读开头一块，用于跳过 ID3v2 并探测首个 MP3 帧头。
     * file_off = 音频数据在文件中的绝对偏移；buf_off = 它在探测缓冲内的偏移。
     * 两者在大 ID3v2 时不同，混用会越界，必须分开记。 */
    fseek(f, 0, SEEK_SET);
    size_t   probe_len = fread(s_mp3_in, 1, MP3_IN_SAFE, f);
    uint32_t file_off  = 0;
    size_t   buf_off   = 0;
    if (probe_len >= 10 && !memcmp(s_mp3_in, "ID3", 3)) {
        uint32_t tag = ((uint32_t)(s_mp3_in[6] & 0x7f) << 21) |
                       ((uint32_t)(s_mp3_in[7] & 0x7f) << 14) |
                       ((uint32_t)(s_mp3_in[8] & 0x7f) << 7)  |
                       (uint32_t)(s_mp3_in[9] & 0x7f);
        file_off = 10 + tag + ((s_mp3_in[5] & 0x10) ? 10 : 0);  /* 0x10=有 footer */
        buf_off  = file_off;
    }
    /* 带大封面的 ID3v2 可能超过探测块：从音频起点重新读，缓冲内偏移归 0 */
    if (buf_off + 8 >= probe_len) {
        if (file_off >= (uint32_t)fsz - 8) { fclose(f); return ESP_ERR_NOT_SUPPORTED; }
        fseek(f, file_off, SEEK_SET);
        probe_len = fread(s_mp3_in, 1, MP3_IN_SAFE, f);
        buf_off = 0;
    }
    uint8_t *probe_ptr = s_mp3_in + buf_off;
    int      probe_avail = (int)(probe_len - buf_off);
    if (probe_len < 8 || probe_avail < 8) { fclose(f); return ESP_ERR_NOT_SUPPORTED; }

    /* 探测复用 PSRAM 里的解码器实例：避免在栈上再放一个 6.7KB 的 mp3dec_t，
     * 与 minimp3 内部 16KB 的 mp3dec_scratch_t 叠加会吃掉任务栈。
     * pcm=NULL 只解析帧头、不写解码状态，之后 memset 复位即可正式使用。 */
    memset(s_mp3dec, 0, sizeof(*s_mp3dec));   /* 清掉上一首残留的 header/状态 */
    mp3dec_frame_info_t info;
    int spf = mp3dec_decode_frame(s_mp3dec, probe_ptr, probe_avail, NULL, &info);
    /* MINIMP3_ONLY_MP3 只编了 Layer III，Layer I/II 文件会全程静音，直接拒绝 */
    if (spf <= 0 || info.layer != 3 || info.hz < 8000 || info.hz > 48000 ||
        info.bitrate_kbps <= 0 ||
        info.frame_offset < 0 || info.frame_offset + 4 > probe_avail) {
        fclose(f);
        return ESP_ERR_NOT_SUPPORTED;
    }

    /* 优先用 Xing/Info/VBRI 的精确帧数算时长（VBR），否则按首帧码率估算（CBR） */
    uint32_t frames = 0;
    const uint8_t *fp = probe_ptr + info.frame_offset;
    int avail = probe_avail - info.frame_offset;
    int scan = avail < 256 ? avail : 256;
    for (int i = 0; i + 12 <= scan; i++) {
        if (!memcmp(fp + i, "Xing", 4) || !memcmp(fp + i, "Info", 4)) {
            if (be32(fp + i + 4) & 1) frames = be32(fp + i + 8);
            break;
        }
        if (i + 18 <= scan && !memcmp(fp + i, "VBRI", 4)) {
            frames = be32(fp + i + 14);
            break;
        }
    }

    uint32_t audio_bytes = (uint32_t)fsz - file_off;
    uint8_t  tail[3];
    if (fsz >= 131 && fseek(f, fsz - 128, SEEK_SET) == 0 &&
        fread(tail, 1, 3, f) == 3 && !memcmp(tail, "TAG", 3)) {
        audio_bytes -= 128;                  /* 扣掉 ID3v1 尾部 */
    }

    if (frames > 0) {
        s_trk.dur_ms = (uint32_t)(((uint64_t)frames * (uint32_t)spf * 1000) / info.hz);
    } else {
        /* bytes × 8bit / (kbps×1000 bit/s) × 1000 ms = bytes × 8 / kbps */
        s_trk.dur_ms = (uint32_t)(((uint64_t)audio_bytes * 8) / (uint32_t)info.bitrate_kbps);
    }

    s_trk.rate              = (uint32_t)info.hz;
    s_trk.channels          = (uint16_t)info.channels;
    s_trk.bits              = 16;            /* 解码器输出 16bit PCM */
    s_trk.data_off          = file_off;
    s_trk.data_size         = audio_bytes;
    s_trk.samples_per_frame = (uint32_t)spf;
    s_fmt = FMT_MP3;
    s_file = f;

    /* 从音频起点正式开始解码：探测时 mp3d_find_frame 会写 free_format_bytes
     * 等状态，需整块清零（mp3dec_init 只清 header[0]，不够） */
    memset(s_mp3dec, 0, sizeof(*s_mp3dec));
    s_mp3_len = 0;
    s_mp3_eof = false;
    fseek(f, file_off, SEEK_SET);
    ESP_LOGI(TAG, "MP3 %uHz %uch %dkbps %ums (%s)", (unsigned)s_trk.rate,
             (unsigned)s_trk.channels, info.bitrate_kbps, (unsigned)s_trk.dur_ms,
             frames > 0 ? "VBR/Xing" : "CBR 估算");
    return ESP_OK;
}

/* 返回 1=s_out 中有数据待写，0=本轮无产出需继续，-1=播放结束 */
static int mp3_pump(size_t *out_bytes)
{
    mp3_refill();
    if (s_mp3_len <= 0) return -1;

    mp3dec_frame_info_t info;
    int samples = mp3dec_decode_frame(s_mp3dec, s_mp3_in, s_mp3_len, s_mp3_pcm, &info);
    int consumed = info.frame_bytes;
    if (consumed < 0) consumed = 0;
    if (consumed > s_mp3_len) consumed = s_mp3_len;

    bool got = samples > 0;
    if (got) {
        int ch = (info.channels == 1 || info.channels == 2) ? info.channels : 2;
        /* minimp3 输出 int16 交错 PCM；统一扩成双声道并施加软件音量 */
        for (int i = 0; i < samples; i++) {
            int32_t l = s_mp3_pcm[i * ch];
            int32_t r = (ch == 2) ? s_mp3_pcm[i * ch + 1] : l;
            s_out[2 * i]     = (int16_t)(l * s_volume / 100);
            s_out[2 * i + 1] = (int16_t)(r * s_volume / 100);
        }
        *out_bytes = (size_t)samples * 4;   /* 16bit × 2ch */
        s_decoded += (uint64_t)samples;
        /* 进度按真实解码出的采样数算，VBR 也准（与 I2S 实际输出同步） */
        g_audio.pos_ms = (uint32_t)((s_decoded * 1000) / s_trk.rate);
    }

    /* 按解码器实际消耗量前移输入窗口，保留未消费的尾部 */
    if (consumed > 0) {
        s_mp3_len -= consumed;
        if (s_mp3_len > 0) memmove(s_mp3_in, s_mp3_in + consumed, (size_t)s_mp3_len);
    }
    if (!got && consumed == 0) return -1;                     /* 无法推进，结束 */
    if (!got && s_mp3_eof && s_mp3_len < 16) return -1;       /* 尾部只剩垃圾 */
    return got ? 1 : 0;                                       /* 跳过标签/垃圾，继续 */
}

/* ---------------- 统一的「产出一块 PCM」 ---------------- */
static int produce_pcm(size_t *bytes)
{
    if (s_fmt == FMT_MP3) return mp3_pump(bytes);

    uint32_t want = (SRC_BYTES / s_trk.frame_bytes) * s_trk.frame_bytes;
    if (want == 0) want = s_trk.frame_bytes;
    size_t n = fread(s_src, 1, want, s_file);
    if (n == 0) return -1;
    *bytes = pcm_convert(n);
    s_played += (uint32_t)n;
    g_audio.pos_ms = (uint32_t)(((uint64_t)s_played * 1000) / s_trk.bytes_per_sec);
    return *bytes ? 1 : 0;
}

static void publish(void) { g_audio.version++; }

static void set_hint(const char *s)
{
    /* volatile char[] 逐字节写，写完后才 publish，UI 拿到新 version 时内容已就绪 */
    size_t i = 0;
    if (s) for (; i + 1 < sizeof(g_audio.hint) && s[i]; i++) g_audio.hint[i] = s[i];
    g_audio.hint[i] = '\0';
}

/* ---------------- 命令处理 ---------------- */
static void handle_cmd(const audio_cmd_t *c)
{
    switch (c->id) {
    case CMD_PLAY: {
        track_reset();
        size_t len = strlen(c->path);
        bool is_mp3 = len >= 4 && strcasecmp(c->path + len - 4, ".mp3") == 0;
        esp_err_t err = is_mp3 ? mp3_open(c->path) : wav_open(c->path);
        if (err != ESP_OK) {
            track_reset();
            amp_enable(false);
            g_audio.loaded  = false;
            g_audio.playing = false;
            g_audio.eof     = false;
            g_audio.pos_ms  = 0;
            g_audio.dur_ms  = 0;
            set_hint(err == ESP_ERR_NOT_SUPPORTED ? "格式不支持或文件损坏" : "文件打开失败");
            publish();
            break;
        }
        err = i2s_set_rate(s_trk.rate);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "i2s_set_rate(%u) failed: %s", (unsigned)s_trk.rate, esp_err_to_name(err));
            track_reset();
            g_audio.loaded = false; g_audio.playing = false;
            set_hint("I2S 初始化失败");
            publish();
            break;
        }
        g_audio.sample_rate = s_trk.rate;
        g_audio.channels    = (uint8_t)s_trk.channels;
        g_audio.bits        = (uint8_t)s_trk.bits;
        g_audio.dur_ms      = s_trk.dur_ms;
        g_audio.pos_ms      = 0;
        g_audio.loaded     = true;
        g_audio.playing    = true;
        g_audio.eof        = false;
        set_hint(NULL);
        amp_enable(true);
        ESP_LOGI(TAG, "play %s: %s %uHz %uch %ubit %ums", c->path,
                 is_mp3 ? "MP3" : "WAV", (unsigned)s_trk.rate,
                 (unsigned)s_trk.channels, (unsigned)s_trk.bits, (unsigned)s_trk.dur_ms);
        publish();
        break;
    }
    case CMD_PAUSE:
        if (g_audio.loaded) {
            g_audio.playing = false;
            amp_enable(false);               /* 关断功放，暂停时无静态底噪 */
            publish();
        }
        break;
    case CMD_RESUME:
        if (g_audio.loaded && s_file) {
            g_audio.playing = true;
            amp_enable(true);
            publish();
        }
        break;
    case CMD_STOP:
        track_reset();
        amp_enable(false);
        if (s_tx_enabled) { i2s_channel_disable(s_tx); s_tx_enabled = false; }
        g_audio.loaded = false; g_audio.playing = false; g_audio.eof = false;
        g_audio.pos_ms = 0; g_audio.dur_ms = 0;
        set_hint(NULL);
        publish();
        break;
    case CMD_SEEK: {
        if (!g_audio.loaded || !s_file || !s_trk.data_size) break;
        int pct = c->value;
        if (pct < 0) pct = 0;
        if (pct > 1000) pct = 1000;
        uint32_t off = (uint32_t)(((uint64_t)s_trk.data_size * (uint32_t)pct) / 1000);

        if (s_fmt == FMT_WAV) {
            off -= off % s_trk.frame_bytes;      /* 对齐到帧边界，WAV 是精确定位 */
            s_played = off;
            g_audio.pos_ms = (uint32_t)(((uint64_t)off * 1000) / s_trk.bytes_per_sec);
        } else {
            /* MP3 无帧索引：按字节比例近似定位后重新找帧同步（VBR 会有偏差） */
            g_audio.pos_ms = (uint32_t)(((uint64_t)s_trk.dur_ms * (uint32_t)pct) / 1000);
            s_decoded = (uint64_t)g_audio.pos_ms * s_trk.rate / 1000;
            s_mp3_len = 0;
            s_mp3_eof = false;
            memset(s_mp3dec, 0, sizeof(*s_mp3dec));  /* 丢弃 bit reservoir，重新找帧同步 */
        }
        fseek(s_file, s_trk.data_off + off, SEEK_SET);
        g_audio.eof = false;
        publish();
        break;
    }
    case CMD_VOLUME: {
        int v = c->value;
        if (v < 0) v = 0;
        if (v > 100) v = 100;
        s_volume = (uint8_t)v;
        g_audio.volume = s_volume;
        publish();
        break;
    }
    }
}

static void audio_task(void *arg)
{
    (void)arg;
    for (;;) {
        audio_cmd_t cmd;
        /* 播放中不能阻塞等命令，否则 I2S 断流会破音；空闲时才阻塞休眠省 CPU */
        BaseType_t got = xQueueReceive(s_q, &cmd, (s_file && g_audio.playing) ? 0 : portMAX_DELAY);
        if (got == pdTRUE) handle_cmd(&cmd);

        if (!s_file || !g_audio.playing) continue;

        size_t bytes = 0;
        int r = produce_pcm(&bytes);
        if (r < 0) {
            /* 播完：置 eof 让 UI 决定下一首，同时关断功放 */
            amp_enable(false);
            g_audio.playing = false;
            g_audio.eof     = true;
            g_audio.pos_ms  = s_trk.dur_ms;
            publish();
            continue;
        }
        if (r == 0) continue;                /* 本轮只跳过了标签/垃圾，继续取数据 */

        size_t written = 0;
        /* 阻塞写出：I2S 时钟决定真实播放速度，因此进度天然与真实时间同步。
         * 超时必须是有限毫秒值 —— 驱动内部走 pdMS_TO_TICKS(timeout_ms)，
         * 传 portMAX_DELAY 会整数溢出成 0，反而立刻返回 TIMEOUT。
         * 一块最多 ~5.8ms 音频，1000ms 余量充足。 */
        esp_err_t err = i2s_channel_write(s_tx, s_out, bytes, &written, 1000);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "i2s write failed: %s", esp_err_to_name(err));
            amp_enable(false);
            g_audio.playing = false;
            set_hint("I2S 输出失败");
            publish();
            continue;
        }
        if (g_audio.pos_ms > s_trk.dur_ms) g_audio.pos_ms = s_trk.dur_ms;
        publish();                         /* 每块推进一次，UI 按 version 刷新进度条 */
    }
}

/* ---------------- 对外 API ---------------- */
static void post_cmd(cmd_id_t id, const char *path, int value)
{
    if (!s_q) return;
    audio_cmd_t c = { .id = id, .value = value };
    if (path) {
        snprintf(c.path, sizeof(c.path), "%s", path);
    } else {
        c.path[0] = '\0';
    }
    /* 队列满时丢最旧的一条再投，保证「停止/切歌」这类关键命令不被卡住 */
    if (xQueueSend(s_q, &c, 0) != pdTRUE) {
        audio_cmd_t dummy;
        xQueueReceive(s_q, &dummy, 0);
        xQueueSend(s_q, &c, 0);
    }
}

void audio_player_init(void)
{
    /* g_audio 是 volatile 且由后台任务写，这里任务尚未创建，BSS 已清零，
     * 只显式初始化常量字段即可（不对 volatile 对象做 memset） */
    g_audio.volume = s_volume;
    g_audio.hint[0] = '\0';

    gpio_num_t sd = AUDIO_I2S_SD_MODE_GPIO;
    if (sd >= 0) {
        gpio_config_t io = {
            .pin_bit_mask = 1ULL << (unsigned)sd,
            .mode         = GPIO_MODE_OUTPUT,
            .pull_up_en   = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type    = GPIO_INTR_DISABLE,
        };
        gpio_config(&io);
        gpio_set_level(sd, 0);
    }

    /* 缓冲在开机、内存最空时一次性分配，之后常驻（合计约 20KB PSRAM） */
    const uint32_t caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    s_src     = (uint8_t *)heap_caps_malloc(SRC_BYTES, caps);
    s_out     = (int16_t *)heap_caps_malloc(OUT_SAMPLES * sizeof(int16_t), caps);
    s_mp3_in  = (uint8_t *)heap_caps_malloc(MP3_IN_CAP, caps);
    s_mp3_pcm = (int16_t *)heap_caps_malloc(MINIMP3_MAX_SAMPLES_PER_FRAME * sizeof(int16_t), caps);
    s_mp3dec  = (mp3dec_t *)heap_caps_malloc(sizeof(mp3dec_t), caps);
    if (!s_src || !s_out || !s_mp3_in || !s_mp3_pcm || !s_mp3dec) {
        ESP_LOGE(TAG, "audio buffers alloc failed (PSRAM), player disabled");
        return;
    }
    mp3dec_init(s_mp3dec);

    s_q = xQueueCreate(4, sizeof(audio_cmd_t));
    if (!s_q) { ESP_LOGE(TAG, "cmd queue create failed"); return; }

    /* minimp3 的 mp3dec_scratch_t ≈16KB 是栈变量，任务栈必须 ≥24KB；
     * 用 xTaskCreateWithCaps 放到 PSRAM，不占紧张的内部 DRAM。 */
    BaseType_t ok = xTaskCreateWithCaps(audio_task, "audio", 28672, NULL, 5, NULL,
                                        MALLOC_CAP_SPIRAM);
    if (ok != pdPASS) {
        ok = xTaskCreate(audio_task, "audio", 28672, NULL, 5, &s_task);
    }
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "audio task create failed");
        return;
    }
    ESP_LOGI(TAG, "init ok (WAV+MP3), I2S BCLK=%d LRCK=%d DOUT=%d SD_MODE=%d",
             (int)AUDIO_I2S_BCLK_GPIO, (int)AUDIO_I2S_LRCK_GPIO,
             (int)AUDIO_I2S_DOUT_GPIO, (int)AUDIO_I2S_SD_MODE_GPIO);
}

esp_err_t audio_player_play(const char *path)
{
    if (!s_q || !path || !path[0]) return ESP_ERR_INVALID_STATE;
    post_cmd(CMD_PLAY, path, 0);
    return ESP_OK;
}
void audio_player_pause(void)  { post_cmd(CMD_PAUSE, NULL, 0); }
void audio_player_resume(void) { post_cmd(CMD_RESUME, NULL, 0); }
void audio_player_stop(void)   { post_cmd(CMD_STOP, NULL, 0); }
void audio_player_seek(int pct_0_1000) { post_cmd(CMD_SEEK, NULL, pct_0_1000); }
void audio_player_set_volume(uint8_t pct_0_100) { post_cmd(CMD_VOLUME, NULL, pct_0_100); }

void audio_player_ack_eof(void)
{
    g_audio.eof = false;
    publish();
}

bool audio_player_supported(const char *name)
{
    size_t n = strlen(name);
    if (n < 4) return false;
    const char *e = name + n - 4;
    return strcasecmp(e, ".wav") == 0 || strcasecmp(e, ".mp3") == 0;
}
