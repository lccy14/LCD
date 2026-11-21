#include "voice_asr.h"
#include "ai_chat.h"
#include "wifi_scan.h"        /* g_wifi_state：诊断连接失败时的 WiFi 状态 */
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "ASR";

/* 智谱语音识别（OpenAI 兼容，multipart/form-data），国内直连，复用 AI 的 Key */
#define ASR_URL       "https://open.bigmodel.cn/api/paas/v4/audio/transcriptions"
#define ASR_MODEL     "glm-asr-2512"
#define ASR_TIMEOUT_MS 12000   /* 单次超时；配合 SYN 重传降低 + 重试2次，避免卡满半分钟 */
#define ASR_BOUNDARY  "----ESP32ASR7f3a9d2c"

volatile voice_asr_state_t g_asr;

static SemaphoreHandle_t s_sem = NULL;
static char   s_path[160];          /* 待识别的 WAV 路径 */
static char  *s_body = NULL;        /* multipart 请求体，PSRAM */
static char  *s_resp = NULL;        /* 响应体，PSRAM */
#define MAX_WAV_BYTES (600 * 1024)  /* WAV 上限≈18s；body 按实际 WAV 大小按需分配，用完释放，不再常驻 600KB */
#define RESP_CAP (4 * 1024)

static void asr_task(void *arg);

static void publish(void) { g_asr.version++; }

static void set_hint(const char *s)
{
    size_t i = 0;
    if (s) for (; i + 1 < sizeof(g_asr.hint) && s[i]; i++) g_asr.hint[i] = s[i];
    g_asr.hint[i] = '\0';
}

void voice_asr_init(void)
{
    /* s_body 不在 init 分配：600KB 常驻会吃光 2MB PSRAM。改为识别时按实际 WAV 大小分配。 */
    s_resp = heap_caps_malloc(RESP_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_resp) {
        ESP_LOGE(TAG, "PSRAM alloc failed, ASR disabled");
        return;
    }
    s_sem = xSemaphoreCreateBinary();
    if (!s_sem) { ESP_LOGE(TAG, "sem failed"); return; }

    BaseType_t ok = xTaskCreateWithCaps(asr_task, "voice_asr", 24576, NULL, 3, NULL, MALLOC_CAP_SPIRAM);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "PSRAM task failed (%d)", (int)ok);
        return;
    }
    ESP_LOGI(TAG, "init ok (model=%s)", ASR_MODEL);
}

/* 从 JSON 里提取 "text":"..."，做基本反转义（\n \" \\） */
static bool parse_asr_text(const char *json, char *out, size_t cap)
{
    const char *p = strstr(json, "\"text\"");
    if (!p) return false;
    p = strchr(p, ':');
    if (!p) return false;
    while (*++p == ' ' || *p == '\t') {}
    if (*p != '"') return false;
    p++;
    size_t n = 0;
    while (*p && *p != '"' && n + 1 < cap) {
        if (*p == '\\' && p[1]) {
            p++;
            switch (*p) {
                case 'n': out[n++] = '\n'; break;
                case 'r': out[n++] = '\r'; break;
                case 't': out[n++] = '\t'; break;
                case '"': out[n++] = '"';  break;
                case '\\': out[n++] = '\\'; break;
                default: out[n++] = *p; break;
            }
            p++;
        } else {
            out[n++] = *p++;
        }
    }
    out[n] = '\0';
    return n > 0;
}

/* 读取 WAV 拼成 multipart 请求体，返回总长度；失败返回 -1 */
static int build_multipart(const char *path, const char *key)
{
    FILE *f = fopen(path, "rb");
    if (!f) { ESP_LOGE(TAG, "open wav failed: %s", path); return -1; }
    fseek(f, 0, SEEK_END);
    long wav_len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (wav_len <= 0 || wav_len > MAX_WAV_BYTES) {
        ESP_LOGE(TAG, "WAV too large or empty (%ld bytes, limit %d)", wav_len, MAX_WAV_BYTES);
        set_hint(wav_len > MAX_WAV_BYTES ? "录音太长，请重录(≤18秒)" : "录音文件为空");
        fclose(f);
        return -1;
    }

    /* 按实际大小分配 multipart buffer：head(~400B) + wav + tail(~40B) + 余量。
     * 不再常驻 600KB——2MB PSRAM 下那是致命浪费。用完由调用方释放。 */
    size_t need = (size_t)wav_len + 1024;
    if (s_body) heap_caps_free(s_body);   /* 防御：上一轮异常残留 */
    s_body = heap_caps_malloc(need, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_body) {
        ESP_LOGE(TAG, "alloc multipart %u bytes failed (PSRAM free %u)",
                 (unsigned)need, (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        set_hint("内存不足，无法识别");
        fclose(f);
        return -1;
    }
    size_t body_cap = need;

    /* 头 + WAV + 尾，留足余量 */
    const char *head_fmt =
        "--%s\r\n"
        "Content-Disposition: form-data; name=\"model\"\r\n\r\n"
        "%s\r\n"
        "--%s\r\n"
        "Content-Disposition: form-data; name=\"stream\"\r\n\r\n"
        "false\r\n"
        "--%s\r\n"
        "Content-Disposition: form-data; name=\"file\"; filename=\"voice.wav\"\r\n"
        "Content-Type: audio/wav\r\n\r\n";
    const char *tail_fmt = "\r\n--%s--\r\n";

    int n = snprintf(s_body, body_cap, head_fmt, ASR_BOUNDARY, ASR_MODEL,
                     ASR_BOUNDARY, ASR_BOUNDARY);
    if (n < 0 || n + wav_len + 128 >= (long)body_cap) {
        ESP_LOGE(TAG, "multipart buffer too small (%u bytes for wav %ld)", (unsigned)body_cap, wav_len);
        set_hint("录音太长，请重录(≤18秒)");
        fclose(f);
        heap_caps_free(s_body); s_body = NULL;
        return -1;
    }
    size_t got = fread(s_body + n, 1, (size_t)wav_len, f);
    fclose(f);
    if (got != (size_t)wav_len) {
        ESP_LOGE(TAG, "read wav short");
        heap_caps_free(s_body); s_body = NULL;
        return -1;
    }
    n += (int)got;
    n += snprintf(s_body + n, body_cap - n, tail_fmt, ASR_BOUNDARY);
    (void)key;   /* Key 在 HTTP header 里用 */
    ESP_LOGI(TAG, "multipart %d bytes (wav %ld)", n, wav_len);
    return n;
}

static void asr_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (xSemaphoreTake(s_sem, portMAX_DELAY) != pdTRUE) continue;

        g_asr.busy = true;
        g_asr.error = false;
        g_asr.done = false;
        set_hint("识别中...");
        publish();

        char key[96];
        /* 用缓存读 Key：本任务栈在 PSRAM，调 NVS 会触发 flash cache 禁用断言崩溃 */
        if (!ai_chat_key_cached(key, sizeof(key))) {
            set_hint("请先配置智谱 API Key");
            g_asr.error = true; g_asr.busy = false; publish();
            continue;
        }

        int len = build_multipart(s_path, key);
        if (len < 0) {
            if (!g_asr.hint[0]) set_hint("读取录音失败");  /* 保留 build_multipart 的具体提示 */
            g_asr.error = true; g_asr.busy = false; publish();
            continue;
        }

        ESP_LOGI(TAG, "post %d bytes to %s (wifi_state=%d)", len, ASR_URL, (int)g_wifi_state);

        char ct[96];
        snprintf(ct, sizeof(ct), "multipart/form-data; boundary=%s", ASR_BOUNDARY);
        char auth[160];
        snprintf(auth, sizeof(auth), "Bearer %s", key);

        /* 失败重试 2 次：不稳定链路偶发丢 SYN，重建 client 会重新 DNS/连接，
         * 有机会换到另一个 IP。单次超时已降到 12s，最坏 24s 内给出结果。 */
        bool ok = false;
        for (int attempt = 1; attempt <= 2 && !ok; attempt++) {
            esp_http_client_config_t cfg = {
                .url = ASR_URL,
                .method = HTTP_METHOD_POST,
                .crt_bundle_attach = esp_crt_bundle_attach,
                .timeout_ms = ASR_TIMEOUT_MS,
                .user_agent = "esp32-voice-asr/1.0",
            };
            esp_http_client_handle_t c = esp_http_client_init(&cfg);
            if (!c) { set_hint("HTTP 初始化失败"); continue; }
            esp_http_client_set_header(c, "Content-Type", ct);
            esp_http_client_set_header(c, "Authorization", auth);

            if (esp_http_client_open(c, len) == ESP_OK) {
                esp_http_client_write(c, s_body, len);
                esp_http_client_fetch_headers(c);
                int status = esp_http_client_get_status_code(c);
                int total = 0, r;
                if (status == 200) {
                    while (total < RESP_CAP - 1 &&
                           (r = esp_http_client_read(c, s_resp + total, RESP_CAP - 1 - total)) > 0) {
                        total += r;
                    }
                    s_resp[total] = '\0';
                    char text[512];
                    if (parse_asr_text(s_resp, text, sizeof(text))) {
                        size_t i = 0;
                        for (; i + 1 < sizeof(g_asr.text) && text[i]; i++) g_asr.text[i] = text[i];
                        g_asr.text[i] = '\0';
                        set_hint("");
                        g_asr.done = true;
                        ok = true;
                        ESP_LOGI(TAG, "recognized: %.80s", text);
                    } else {
                        ESP_LOGW(TAG, "parse failed: %.160s", s_resp);
                        set_hint("识别结果解析失败");
                    }
                } else {
                    ESP_LOGE(TAG, "HTTP status %d", status);
                    r = esp_http_client_read(c, s_resp, RESP_CAP - 1);
                    if (r > 0) s_resp[r] = '\0'; else s_resp[0] = '\0';
                    ESP_LOGE(TAG, "error body: %.200s", s_resp);
                    set_hint("识别请求失败");
                    /* 4xx（Key/参数错）重试无意义，直接跳出 */
                    if (status >= 400 && status < 500) { esp_http_client_close(c); esp_http_client_cleanup(c); break; }
                }
                esp_http_client_close(c);
            } else {
                ESP_LOGW(TAG, "connect failed (attempt %d/2)", attempt);
                if (attempt == 2) set_hint("连接失败，请检查网络");
            }
            esp_http_client_cleanup(c);
        }
        /* 识别结束（成功/失败）立即释放 multipart buffer，避免常驻吃光 PSRAM */
        if (s_body) { heap_caps_free(s_body); s_body = NULL; }
        if (!ok) g_asr.error = true;
        g_asr.busy = false;
        publish();
    }
}

void voice_asr_start(const char *wav_path)
{
    if (!s_sem || !wav_path || g_asr.busy) return;
    strncpy(s_path, wav_path, sizeof(s_path) - 1);
    s_path[sizeof(s_path) - 1] = '\0';
    xSemaphoreGive(s_sem);
}

bool voice_asr_is_busy(void) { return g_asr.busy; }

void voice_asr_ack_done(void)
{
    if (g_asr.done) { g_asr.done = false; publish(); }
}
