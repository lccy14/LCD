#include "voice_tts.h"
#include "ai_key_local.h"     /* BAIDU_TTS_API_KEY / BAIDU_TTS_SECRET_KEY（编译期常量） */
#include "audio_player.h"     /* audio_player_play：播放生成的 WAV */
#include "sd_card.h"          /* SD_MOUNT_POINT / sd_card_is_mounted */
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <dirent.h>

static const char *TAG = "TTS";

/* 百度语音合成（免费额度）：短文本 REST API，aue=6 直接返回 WAV。
 * 流程：oauth 换 access_token（缓存30天） → POST text2audio → 流式写 SD → audio_player 播放。
 * 单次建议 ≤60 汉字，长文本按 UTF-8 字符边界分段、串行合成+播放。 */
#define TTS_TOKEN_URL   "https://aip.baidubce.com/oauth/2.0/token"
#define TTS_SPEECH_URL  "https://tsn.baidu.com/text2audio"
#define TTS_TIMEOUT_MS  20000
#define TTS_MAX_CHARS   55        /* 每段最多 55 个 UTF-8 字符（留余量，百度建议≤60汉字） */
#define TTS_MAX_SEGS    12        /* s_text 600B 最坏全是 ASCII=600字符 → 11段(55/段)，留余量 */

volatile voice_tts_state_t g_tts;

static SemaphoreHandle_t s_sem = NULL;
static char  s_text[600];          /* 待合成文本（PSRAM 任务只读，UTF-8 已校验） */
static char *s_body = NULL;        /* form body 缓冲，PSRAM */
#define BODY_CAP (4 * 1024)

/* token 缓存：编译期 Key 换来的 access_token，30 天有效。PSRAM 任务专用，无锁（只此任务写）。 */
static char      s_token[160];
static uint64_t  s_token_expire_ms = 0;   /* esp_timer 毫秒 */
static bool      s_conn_fail = false;     /* 本次请求是否连接层失败（TLS/网络），供任务决定是否重试 */

/* TLS 接收缓冲必须在内部 RAM：esp_tls_conn_read 把解密结果直接写进调用者 buffer，
 * 若 buffer 在 PSRAM（TTS 任务栈就在 PSRAM），硬件 AES 多记录解密会触发
 * PSA_ERROR_INVALID_ARGUMENT (-0x0087)。文件级 static 落在 .bss（内部 DRAM）。 */
static char    s_tokbuf[1024];    /* token 响应缓冲，需在内部 RAM 解密 */

static void tts_task(void *arg);

/* 事件驱动下载：esp_http_client_read() 内部会把解密好的数据通过
 * HTTP_EVENT_ON_DATA 回调吐出来，这里直接写文件，绕开 read() 返回值路径。
 * （实测 get_header 在此处读到错误的 Content-Length，而事件回调里的头是对的。） */
static FILE   *s_dl_file = NULL;    /* 当前下载目标文件 */
static int     s_dl_total = 0;      /* 累计写入字节数 */
static uint32_t s_dl_cl = 0;        /* 事件回调捕获的真实 Content-Length */

static esp_err_t tts_http_event_handler(esp_http_client_event_t *evt)
{
    switch (evt->event_id) {
        case HTTP_EVENT_ON_HEADER:
            if (evt->header_key && evt->header_value &&
                strcasecmp(evt->header_key, "Content-Length") == 0) {
                s_dl_cl = (uint32_t)strtoul(evt->header_value, NULL, 10);
            }
            break;
        case HTTP_EVENT_ON_DATA:
            if (s_dl_file && evt->data && evt->data_len > 0) {
                if (fwrite(evt->data, 1, evt->data_len, s_dl_file) == (size_t)evt->data_len) {
                    s_dl_total += evt->data_len;
                }
            }
            break;
        case HTTP_EVENT_ON_FINISH:
            break;
        case HTTP_EVENT_ERROR:
            ESP_LOGE(TAG, "HTTP_EVENT_ERROR");
            break;
        default:
            break;
    }
    return ESP_OK;
}

static void publish(void) { g_tts.version++; }

static void set_hint(const char *s)
{
    size_t i = 0;
    if (s) for (; i + 1 < sizeof(g_tts.hint) && s[i]; i++) g_tts.hint[i] = s[i];
    g_tts.hint[i] = '\0';
}

/* 生成 MP3 路径：/sdcard/TTS_xxxx.mp3，编号取现有文件的最大值+1。
 * 不用静态 counter：重启后 counter 归零会和 SD 卡上残留文件重名，
 * 且编号必须永远最大，否则 sd_prune_files 按编号清理时会误删刚合成的文件。 */
static void make_tts_path(char *out, size_t cap)
{
    int max_id = 0;
    DIR *d = opendir(SD_MOUNT_POINT);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            const char *nm = e->d_name;
            if (strncmp(nm, "TTS_", 4) != 0) continue;
            size_t len = strlen(nm);
            if (len < 12 || strcmp(nm + len - 4, ".mp3") != 0) continue;
            int id = atoi(nm + 4);
            if (id > max_id) max_id = id;
        }
        closedir(d);
    }
    if (max_id >= 9999) max_id = 0;   /* 极端情况回绕 */
    snprintf(out, cap, "%s/TTS_%04d.mp3", SD_MOUNT_POINT, max_id + 1);
}

/* URL percent-编码：把 src 编码后写入 dst，返回长度。百度 tex 放 form body 需编码。
 * 空格编码为 %20（非 +），UTF-8 每字节转 %XX。 */
static size_t url_encode(const char *src, char *dst, size_t cap)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t n = 0;
    for (; *src && n + 3 < cap; src++) {
        unsigned char c = (unsigned char)*src;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            dst[n++] = (char)c;
        } else {
            dst[n++] = '%';
            dst[n++] = hex[c >> 4];
            dst[n++] = hex[c & 0x0F];
        }
    }
    dst[n] = '\0';
    return n;
}

/* 获取 access_token（失败返回 false）。缓存有效期内直接复用。 */
static bool baidu_get_token(void)
{
    if (s_token[0] && esp_timer_get_time() / 1000 < s_token_expire_ms) return true;

    esp_http_client_config_t cfg = {
        .url = TTS_TOKEN_URL "?grant_type=client_credentials&client_id=" BAIDU_TTS_API_KEY
               "&client_secret=" BAIDU_TTS_SECRET_KEY,
        .method = HTTP_METHOD_POST,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = TTS_TIMEOUT_MS,
        .buffer_size = 8192,   /* 默认仅 512B，TLS 记录装不下会触发 -0x0087 解密错误 */
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) { set_hint("HTTP 初始化失败"); return false; }
    bool ok = false;
    if (esp_http_client_open(c, 0) == ESP_OK &&
        esp_http_client_fetch_headers(c) == ESP_OK) {
        int status = esp_http_client_get_status_code(c);
        char *resp = s_tokbuf;   /* 内部 RAM：TLS 解密目标不能在 PSRAM */
        int total = 0, r;
        while (total < (int)sizeof(s_tokbuf) - 1 &&
               (r = esp_http_client_read(c, resp + total, sizeof(s_tokbuf) - 1 - total)) > 0) {
            total += r;
        }
        resp[total] = '\0';
        if (status == 200) {
            const char *p = strstr(resp, "\"access_token\":");
            if (p) {
                p = strchr(p, ':');
                while (p && (*(++p) == ' ' || *p == '"')) {}
                size_t i = 0;
                while (p && *p && *p != '"' && i + 1 < sizeof(s_token)) s_token[i++] = *p++;
                s_token[i] = '\0';
            }
            /* 有效期：取 expires_in（秒），留 1 天余量；读不到默认 29 天 */
            uint64_t ttl = 29ULL * 24 * 3600 * 1000;
            const char *e = strstr(resp, "\"expires_in\":");
            if (e) {
                long sec = strtol(e + strlen("\"expires_in\":"), NULL, 10);
                if (sec > 86400) ttl = (uint64_t)(sec - 86400) * 1000;
            }
            s_token_expire_ms = esp_timer_get_time() / 1000 + ttl;
            ok = s_token[0] != '\0';
            ESP_LOGI(TAG, "baidu token ok (%u chars)", (unsigned)strlen(s_token));
        } else {
            ESP_LOGE(TAG, "token HTTP %d: %.200s", status, resp);
            set_hint("百度鉴权失败，检查Key");
        }
        esp_http_client_close(c);
    } else {
        s_conn_fail = true;
        ESP_LOGE(TAG, "token connect failed (TLS/网络), will retry");
        set_hint("连接失败");
    }
    esp_http_client_cleanup(c);
    return ok;
}

/* 合成一段文本，成功返回 true 且写好 WAV 到 path。
 * text 是 UTF-8，URL 编码后放 form body；aue=6 返回 WAV。 */
static bool baidu_tts_once(const char *text, const char *path)
{
    s_conn_fail = false;
    if (!baidu_get_token()) return false;

    char enc[1200];
    url_encode(text, enc, sizeof(enc));
    int n = snprintf(s_body, BODY_CAP,
                     "tex=%s&lan=zh&ctp=1&cuid=esp32lcd&tok=%s&aue=3&per=0&spd=5&pit=5&vol=5",
                     enc, s_token);
    if (n < 0 || n >= BODY_CAP) { set_hint("文本过长"); return false; }

    esp_http_client_config_t cfg = {
        .url = TTS_SPEECH_URL,
        .method = HTTP_METHOD_POST,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = TTS_TIMEOUT_MS,
        .buffer_size = 16384,  /* ≥ TLS record 明文上限，且 < ALWAYSINTERNAL(32KB) 保证在内部 RAM */
        .event_handler = tts_http_event_handler,   /* 事件驱动下载：回调里直接写文件 */
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) { set_hint("HTTP 初始化失败"); return false; }
    esp_http_client_set_header(c, "Content-Type", "application/x-www-form-urlencoded");
    esp_http_client_set_header(c, "Connection", "close");   /* 禁用连接复用，排除响应串味 */

    bool ok = false;
    s_dl_file = NULL; s_dl_total = 0; s_dl_cl = 0;
    if (esp_http_client_open(c, n) == ESP_OK) {
        esp_http_client_write(c, s_body, n);
        esp_http_client_fetch_headers(c);
        int status = esp_http_client_get_status_code(c);
        /* s_dl_cl 由事件回调的 ON_HEADER 捕获（get_header 在此处读到错值） */
        ESP_LOGI(TAG, "speech status=%d CL=%u", status, (unsigned)s_dl_cl);

        if (status == 200 && s_dl_cl > 44) {
            FILE *f = fopen(path, "wb");
            if (!f) {
                set_hint("写文件失败");
            } else {
                s_dl_file = f;
                /* 事件驱动下载：read() 内部会通过 ON_DATA 回调把解密数据写进 f。
                 * 循环只负责驱动读取；即使末尾 read 报 -0x0087，只要回调写够了就算成功。 */
                char sink[64];
                int guard = 0;
                while (s_dl_total < (int)s_dl_cl && guard++ < 4096) {
                    int r = esp_http_client_read(c, sink, sizeof(sink));
                    if (r <= 0) break;   /* -0x0087 会在这里返回负值，退出即可 */
                }
                fclose(f);
                s_dl_file = NULL;

                /* 用事件回调累计的真实字节数 + 真实 CL 校验完整性 */
                if ((uint32_t)s_dl_total >= s_dl_cl * 9 / 10) {   /* 允许 10% 尾差 */
                    ESP_LOGI(TAG, "audio saved: %s (%d bytes, CL=%u)", path, s_dl_total, (unsigned)s_dl_cl);
                    /* 只保留最近 20 个合成文件，旧的自动删除，防止 SD 卡被填满 */
                    sd_prune_files("TTS_", ".mp3", 20);
                    ok = true;
                } else {
                    ESP_LOGE(TAG, "audio truncated: got %d bytes, CL=%u -> retry", s_dl_total, (unsigned)s_dl_cl);
                    remove(path);
                    s_conn_fail = true;   /* 触发任务层重试 */
                    set_hint("音频下载不全，重试中");
                }
            }
        } else {
            char err[160];
            int er = esp_http_client_read(c, err, sizeof(err) - 1);
            if (er > 0) err[er] = '\0'; else err[0] = '\0';
            ESP_LOGE(TAG, "HTTP %d body: %.150s", status, err);
            if (strstr(err, "502") || strstr(err, "token")) {
                s_token[0] = '\0'; s_token_expire_ms = 0;   /* token 失效，强制下次重取 */
                set_hint("鉴权失效，正在重试");
            } else {
                set_hint("合成失败，检查额度/网络");
            }
        }
        esp_http_client_close(c);
    } else {
        s_conn_fail = true;
        ESP_LOGE(TAG, "speech connect failed (TLS/网络), will retry");
        set_hint("连接失败");
    }
    esp_http_client_cleanup(c);
    s_dl_file = NULL;
    return ok;
}

/* 等待当前播放结束（轮询 g_audio.eof / !playing）。返回 false 表示被 stop 打断。 */
static bool wait_play_done(uint32_t timeout_ms)
{
    uint64_t start = esp_timer_get_time() / 1000;
    /* play 是异步命令：先等 playing 拉起、且旧 eof 被 CMD_PLAY 清零，
     * 否则会立刻读到上一曲残留的 eof=true 而提前结束。 */
    while (esp_timer_get_time() / 1000 - start < 2000) {
        if (!g_tts.busy) return false;
        if (g_audio.playing && !g_audio.eof) break;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    /* 再等本段播完 */
    start = esp_timer_get_time() / 1000;
    while (esp_timer_get_time() / 1000 - start < timeout_ms) {
        if (!g_tts.busy) return false;
        if (g_audio.eof) { audio_player_ack_eof(); return true; }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    return true;   /* 超时也往下走，避免卡死 */
}

void voice_tts_init(void)
{
    s_body = heap_caps_malloc(BODY_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_body) { ESP_LOGE(TAG, "PSRAM alloc failed for TTS body"); return; }
    s_sem = xSemaphoreCreateBinary();
    if (!s_sem) { ESP_LOGE(TAG, "sem create failed"); return; }
    BaseType_t ok = xTaskCreateWithCaps(tts_task, "tts", 24576, NULL, 3, NULL, MALLOC_CAP_SPIRAM);
    if (ok != pdPASS) ok = xTaskCreate(tts_task, "tts", 16384, NULL, 3, NULL);
    if (ok == pdPASS) ESP_LOGI(TAG, "tts task started (baidu, free tier)");
}

void voice_tts_speak(const char *text)
{
    if (!s_sem || !text || !text[0]) return;
    if (g_tts.busy) return;
    /* 逐字节拷贝 + UTF-8 合法性校验：残缺字节截断，避免非法序列发给 API。 */
    size_t n = 0;
    while (n + 1 < sizeof(s_text) && text[n]) {
        unsigned char c = (unsigned char)text[n];
        size_t need = (c < 0x80) ? 1 : (c >= 0xC2 && c <= 0xDF) ? 2
                    : (c >= 0xE0 && c <= 0xEF) ? 3
                    : (c >= 0xF0 && c <= 0xF4) ? 4 : 0;
        if (need == 0) break;
        if (n + need >= sizeof(s_text)) break;
        bool valid = true;
        for (size_t k = 1; k < need; k++) {
            unsigned char cc = (unsigned char)text[n + k];
            if (cc == 0 || (cc & 0xC0) != 0x80) { valid = false; break; }
        }
        if (!valid) break;
        for (size_t k = 0; k < need; k++) s_text[n + k] = text[n + k];
        n += need;
    }
    s_text[n] = '\0';
    if (n == 0) return;
    g_tts.busy = true;
    g_tts.error = false;
    set_hint("");
    publish();
    xSemaphoreGive(s_sem);
}

bool voice_tts_is_busy(void) { return g_tts.busy; }

void voice_tts_stop(void)
{
    g_tts.busy = false;         /* 让分段循环的 wait 提前退出 */
    audio_player_stop();
}

static void tts_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (xSemaphoreTake(s_sem, portMAX_DELAY) != pdTRUE) continue;

        if (!sd_card_is_mounted()) {
            set_hint("无 SD 卡");
            g_tts.error = true; g_tts.busy = false; publish();
            continue;
        }

        /* 按 UTF-8 字符边界分段：每段 ≤TTS_MAX_CHARS 个字符 */
        char segs[TTS_MAX_SEGS][TTS_MAX_CHARS * 4 + 1];
        int seg_count = 0;
        const char *p = s_text;
        while (*p && seg_count < TTS_MAX_SEGS) {
            char *dst = segs[seg_count];
            size_t dn = 0;
            int chars = 0;
            while (*p && chars < TTS_MAX_CHARS && dn + 4 < sizeof(segs[0])) {
                unsigned char c = (unsigned char)*p;
                size_t clen = (c < 0x80) ? 1 : (c < 0xE0) ? 2 : (c < 0xF0) ? 3 : 4;
                for (size_t k = 0; k < clen && *p; k++) dst[dn++] = *p++;
                chars++;
            }
            dst[dn] = '\0';
            if (dn == 0) break;
            seg_count++;
        }

        bool all_ok = true;
        for (int i = 0; i < seg_count && g_tts.busy; i++) {
            char path[160];
            make_tts_path(path, sizeof(path));
            bool ok = baidu_tts_once(segs[i], path);
            /* 连接层失败(TLS握手/网络抖动)重试 1 次；业务错误(额度/鉴权)不重试 */
            if (!ok && s_conn_fail) {
                ESP_LOGW(TAG, "retry once after connect failure");
                vTaskDelay(pdMS_TO_TICKS(300));
                ok = baidu_tts_once(segs[i], path);
            }
            if (!ok) { all_ok = false; break; }

            if (i == 0) {
                size_t k = 0;
                for (; k + 1 < sizeof(g_tts.path) && path[k]; k++) g_tts.path[k] = path[k];
                g_tts.path[k] = '\0';
            }
            set_hint("");
            audio_player_play(path);
            publish();
            if (!wait_play_done(60000)) break;   /* 等待本段播完再合成下一段 */
        }

        if (!all_ok) g_tts.error = true;
        g_tts.busy = false;
        publish();
    }
}
