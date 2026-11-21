#include "ai_chat.h"
#include "ai_key_local.h"   /* 本地预置 Key（gitignore，首次启动写入 NVS） */
#include "esp_log.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* 国内 OpenAI 兼容端点，全部国内直连（避免跨境链路超时）。
 * 默认 GLM-4-Flash 永久免费；用户可在 Key 界面切换平台。 */
typedef struct {
    const char *url;
    const char *model;
    const char *name;
} ai_provider_info_t;

static const ai_provider_info_t s_providers[AI_PROVIDER_MAX] = {
    [AI_PROVIDER_GLM] = {
        "https://open.bigmodel.cn/api/paas/v4/chat/completions",
        "glm-4-flash", "智谱GLM"       /* 永久免费，128K 上下文 */
    },
    [AI_PROVIDER_DEEPSEEK] = {
        "https://api.deepseek.com/chat/completions",
        "deepseek-chat", "DeepSeek"    /* 低价，中文好 */
    },
    [AI_PROVIDER_SILICON] = {
        "https://api.siliconflow.cn/v1/chat/completions",
        "Qwen/Qwen2.5-7B-Instruct", "硅基流动"  /* 9B 以下免费 */
    },
};

#define AI_TIMEOUT_MS 12000   /* 单次超时；配合 SYN 重传降低 + 重试2次 */

#define NVS_NS        "aichat"
#define NVS_PROVIDER  "provider"
/* 每个平台独立的 Key 键名，切换平台后各用自己的 Key，不互相覆盖 */
static const char *const s_key_names[AI_PROVIDER_MAX] = {
    [AI_PROVIDER_GLM]      = "key_glm",
    [AI_PROVIDER_DEEPSEEK] = "key_deepseek",
    [AI_PROVIDER_SILICON]  = "key_silicon",
};

static const char *TAG = "AI";

volatile ai_chat_state_t g_ai;

static SemaphoreHandle_t s_sem = NULL;
/* 用户问题在 PSRAM 里排队，避免把 LVGL textarea 的指针留给后台任务 */
static char  s_question[1024];
static char *s_req_buf = NULL;   /* 请求 JSON，PSRAM */
static char *s_res_buf = NULL;   /* 响应 JSON，PSRAM */
#define REQ_CAP  (8 * 1024)
#define RES_CAP  (16 * 1024)

/* 最近一轮上下文（PSRAM 缓冲，跟随请求一起发），提升多轮连贯性 */
static char s_last_q[512];
static char s_last_a[512];    /* 与 parse_content 输出/reply 等大，全链路统一 512 */

/* UTF-8 安全拷贝：截断时回退到完整字符边界，避免把 3 字节汉字/4 字节 emoji
 * 截成非法序列——那会导致下一轮请求带坏 UTF-8 被 API 以 400 拒绝。 */
static void utf8_strncpy(volatile char *dst, const volatile char *src, size_t cap)
{
    if (cap == 0) return;
    size_t n = 0;
    while (src[n] && n < cap - 1) { dst[n] = src[n]; n++; }  /* 逐字节拷贝（volatile 不能 memcpy） */
    dst[n] = '\0';
    /* 回退到完整字符边界：从末尾往前找最后一个起始字节(非 10xxxxxx)，
     * 若从它到末尾的字节数 >= 它要求的长度，则该字符完整、保留；
     * 否则这个字符被截断了，把起始字节也删掉，继续往前找。 */
    while (n > 0) {
        size_t start = n - 1;
        /* 往前跳过连续的续字节，找到当前字符的起始字节 */
        while (start > 0 && ((unsigned char)dst[start] & 0xC0) == 0x80) start--;
        unsigned char lead = (unsigned char)dst[start];
        size_t need = (lead < 0x80) ? 1 : (lead < 0xE0) ? 2 : (lead < 0xF0) ? 3 : 4;
        if (n - start >= need) break;    /* 末尾字符完整，到此为止 */
        n = start;                       /* 末尾字符不完整，删掉整个字符 */
    }
    dst[n] = '\0';
}

/* ---------------- NVS: 平台与 Key ---------------- */
ai_provider_t ai_chat_provider_get(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return AI_PROVIDER_GLM;
    uint8_t v = AI_PROVIDER_GLM;
    esp_err_t e = nvs_get_u8(h, NVS_PROVIDER, &v);
    nvs_close(h);
    if (e != ESP_OK || v >= AI_PROVIDER_MAX) return AI_PROVIDER_GLM;
    return (ai_provider_t)v;
}

bool ai_chat_provider_set(ai_provider_t p)
{
    if (p >= AI_PROVIDER_MAX) return false;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t e = nvs_set_u8(h, NVS_PROVIDER, (uint8_t)p);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e == ESP_OK;
}

const char *ai_chat_provider_name(ai_provider_t p)
{
    if (p >= AI_PROVIDER_MAX) return "?";
    return s_providers[p].name;
}

bool ai_chat_key_get(char *out, size_t cap)
{
    if (!out || cap == 0) return false;
    out[0] = '\0';
    ai_provider_t p = ai_chat_provider_get();
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t len = cap;
    esp_err_t e = nvs_get_str(h, s_key_names[p], out, &len);
    nvs_close(h);
    if (e != ESP_OK || out[0] == '\0') { out[0] = '\0'; return false; }
    return true;
}

bool ai_chat_key_set(const char *key)
{
    if (!key) return false;
    ai_provider_t p = ai_chat_provider_get();
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t e = nvs_set_str(h, s_key_names[p], key);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e == ESP_OK;
}

bool ai_chat_key_has(void)
{
    char k[8];
    return ai_chat_key_get(k, sizeof(k));
}

/* ---------------- 配置缓存（PSRAM 任务只读这里，不碰 NVS） ----------------
 * 关键：ai_task 的栈在 PSRAM。NVS 读会触发 SPI flash 操作，期间 cache 被禁用，
 * 而 PSRAM 依赖 cache 访问——在 PSRAM 栈任务里调 NVS 会直接触发 IDF 断言
 * "esp_task_stack_is_sane_cache_disabled" 崩溃。
 * 所以后台任务只访问下面这份内存缓存；由 UI/LVGL 任务（栈在内部 RAM）在
 * 修改配置后调用 ai_chat_cfg_sync() 刷新它。 */
static ai_provider_t s_cache_provider = 0;    /* 0 = 尚未同步 */
static char          s_cache_key[96] = "";
static bool          s_cache_ready  = false;

void ai_chat_cfg_sync(void)
{
    s_cache_provider = ai_chat_provider_get();
    ai_chat_key_get(s_cache_key, sizeof(s_cache_key));
    s_cache_ready = true;
}

bool ai_chat_key_cached(char *out, size_t cap)
{
    if (!out || cap == 0) return false;
    out[0] = '\0';
    if (!s_cache_ready || !s_cache_key[0]) return false;
    size_t n = 0;
    while (n + 1 < cap && s_cache_key[n]) { out[n] = s_cache_key[n]; n++; }
    out[n] = '\0';
    return true;
}

/* ---------------- JSON 辅助 ---------------- */
/* 把字符串按 JSON 规则转义后追加到 buf（处理引号、反斜杠、换行、制表符、
 * 以及 <0x20 的控制字符），返回是否成功。 */
static bool json_append_escaped(char *buf, size_t cap, size_t *len, const char *s)
{
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        char esc[8];
        int n = 0;
        switch (c) {
            case '"':  n = snprintf(esc, sizeof(esc), "\\\""); break;
            case '\\': n = snprintf(esc, sizeof(esc), "\\\\"); break;
            case '\n': n = snprintf(esc, sizeof(esc), "\\n");  break;
            case '\r': n = snprintf(esc, sizeof(esc), "\\r");  break;
            case '\t': n = snprintf(esc, sizeof(esc), "\\t");  break;
            default:
                if (c < 0x20) n = snprintf(esc, sizeof(esc), "\\u%04x", c);
                else { esc[0] = (char)c; esc[1] = '\0'; n = 1; }
                break;
        }
        if (*len + n + 1 >= cap) return false;
        memcpy(buf + *len, esc, n);
        *len += n;
    }
    return true;
}

/* 从 JSON 里找 "content": "..." 并做反转义，写入 out。
 * DeepSeek 响应结构里 content 是唯一含长文本的字符串字段，
 * 用简单扫描即可，不需要完整 JSON 解析器。 */
static bool parse_content(const char *json, char *out, size_t cap)
{
    const char *p = strstr(json, "\"content\"");
    if (!p) return false;
    p = strchr(p, ':');
    if (!p) return false;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p != '"') return false;
    p++;

    size_t n = 0;
    while (*p && n + 1 < cap) {
        /* 结束引号：直接跳出，绝不 n++ —— 之前 n++ 会在字符串末尾留下一个
         * 未写入的栈垃圾字节(如 0xa5)，下一轮当上下文发出去被 API 以 400 拒绝。 */
        if (*p == '"') break;
        if (*p == '\\') {
            p++;
            switch (*p) {
                case 'n': out[n++] = '\n'; break;
                case 'r': out[n++] = '\r'; break;
                case 't': out[n++] = '\t'; break;
                case '"': out[n++] = '"';  break;
                case '\\': out[n++] = '\\'; break;
                case '/': out[n++] = '/';  break;
                case 'u': {
                    /* \uXXXX：DeepSeek 中文通常直接 UTF-8 输出，这里只兜底
                     * BMP 内字符；代理对(emoji)不做处理，跳过。 */
                    if (p[1] && p[2] && p[3] && p[4]) {
                        unsigned cp = 0;
                        sscanf(p + 1, "%4x", &cp);
                        if (cp >= 0x20 && cp < 0x10000 && cp < 0xD800) {
                            size_t need = (cp < 0x80) ? 1 : (cp < 0x800) ? 2 : 3;
                            if (n + need < cap) {   /* 留 1 字节给结尾 '\0' */
                                if (cp < 0x80) out[n++] = (char)cp;
                                else if (cp < 0x800) {
                                    out[n++] = (char)(0xC0 | (cp >> 6));
                                    out[n++] = (char)(0x80 | (cp & 0x3F));
                                } else {
                                    out[n++] = (char)(0xE0 | (cp >> 12));
                                    out[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                                    out[n++] = (char)(0x80 | (cp & 0x3F));
                                }
                            }
                        }
                        p += 4;
                    }
                    break;
                }
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

/* ---------------- 后台任务 ---------------- */
static esp_err_t ai_http_post(ai_provider_t p, const char *body, char *out, size_t out_cap)
{
    esp_http_client_config_t cfg = {
        .url = s_providers[p].url,
        .method = HTTP_METHOD_POST,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = AI_TIMEOUT_MS,
        .user_agent = "esp32-ai-chat/1.0",
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return ESP_FAIL;

    esp_http_client_set_header(c, "Content-Type", "application/json");
    esp_http_client_set_header(c, "Accept", "application/json");
    esp_http_client_set_header(c, "Accept-Encoding", "identity");

    char auth[160];
    if (!s_cache_key[0]) {   /* 用缓存，禁止在 PSRAM 栈任务里读 NVS */
        esp_http_client_cleanup(c);
        return ESP_ERR_INVALID_STATE;
    }
    snprintf(auth, sizeof(auth), "Bearer %s", s_cache_key);
    esp_http_client_set_header(c, "Authorization", auth);

    /* 用 open/fetch/read 模式（与 weather.c 一致）。之前用 perform() 时，
     * perform 内部会把响应体全部读进自己的 buffer，导致后面的 read 拿到空数据。 */
    esp_err_t err = ESP_FAIL;
    int len = (int)strlen(body);
    esp_err_t e = esp_http_client_open(c, len);   /* len>0 → POST，带请求体长度 */
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "open: %s", esp_err_to_name(e));
        err = e;   /* 返回真实错误码，供上层判断是否重试 */
        goto done;
    }
    int w = esp_http_client_write(c, body, len);  /* 发送请求体 */
    if (w < 0) {
        ESP_LOGE(TAG, "write post failed");
        goto done;
    }
    esp_http_client_fetch_headers(c);             /* 读响应头 */
    int status = esp_http_client_get_status_code(c);
    int total = 0, r;
    if (status == 200) {
        while (total < (int)out_cap - 1 &&
               (r = esp_http_client_read(c, out + total, out_cap - 1 - total)) > 0) {
            total += r;
        }
        out[total] = '\0';
        ESP_LOGI(TAG, "http 200, read %d bytes", total);
        err = ESP_OK;
    } else {
        ESP_LOGE(TAG, "HTTP status %d", status);
        r = esp_http_client_read(c, out, out_cap - 1);
        if (r > 0) out[r] = '\0'; else out[0] = '\0';
        ESP_LOGE(TAG, "error body: %.300s", out);   /* 400 的具体原因（如 content 为空、角色顺序） */
    }
    esp_http_client_close(c);
done:
    esp_http_client_cleanup(c);
    return err;
}

static void set_reply(const char *s)
{
    /* 用 UTF-8 安全拷贝：reply 只有 512B，长回复截断时若停在半个汉字中间，
     * 会产生非法 UTF-8（下一轮当上下文发出去会被 API 以 400 拒绝）。 */
    utf8_strncpy(g_ai.reply, s, sizeof(g_ai.reply));
}

static void ai_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (xSemaphoreTake(s_sem, portMAX_DELAY) != pdTRUE) continue;

        g_ai.busy = true;
        g_ai.error = false;
        set_reply("思考中...");
        g_ai.version++;

        char question[512];
        strncpy(question, s_question, sizeof(question) - 1);
        question[sizeof(question) - 1] = '\0';

        /* 只读缓存，绝不调 NVS（PSRAM 栈禁止 flash 操作）。
         * 缓存由 ai_chat_init() 在内部 RAM 栈上填好；UI 改配置后也会同步。 */
        ai_provider_t prov = s_cache_ready ? s_cache_provider : AI_PROVIDER_GLM;
        if (!s_cache_key[0]) {
            char msg[96];
            snprintf(msg, sizeof(msg), "请先点「API Key」填入 %s 的 Key",
                     s_providers[prov].name);
            set_reply(msg);
            g_ai.error = true;
            g_ai.busy = false;
            g_ai.version++;
            continue;
        }

        /* 组装请求 JSON：system + 最近一轮上下文 + 当前问题 */
        size_t n = 0;
        n += snprintf(s_req_buf + n, REQ_CAP - n,
                      "{\"model\":\"%s\",\"stream\":false,\"messages\":[",
                      s_providers[prov].model);
        n += snprintf(s_req_buf + n, REQ_CAP - n,
                      "{\"role\":\"system\",\"content\":\"你是智能助手，用简洁中文回答。\"}");
        if (s_last_q[0] && s_last_a[0]) {
            n += snprintf(s_req_buf + n, REQ_CAP - n, ",{\"role\":\"user\",\"content\":\"");
            json_append_escaped(s_req_buf, REQ_CAP, &n, s_last_q);
            n += snprintf(s_req_buf + n, REQ_CAP - n, "\"},{\"role\":\"assistant\",\"content\":\"");
            json_append_escaped(s_req_buf, REQ_CAP, &n, s_last_a);
            n += snprintf(s_req_buf + n, REQ_CAP - n, "\"}");
        }
        n += snprintf(s_req_buf + n, REQ_CAP - n, ",{\"role\":\"user\",\"content\":\"");
        json_append_escaped(s_req_buf, REQ_CAP, &n, question);
        n += snprintf(s_req_buf + n, REQ_CAP - n, "\"}]}");

        ESP_LOGI(TAG, "req(%d) free_internal=%u: %.400s", (int)n,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), s_req_buf);
        /* 连接失败（超时/丢 SYN）重试 1 次：重建 client 会重新 DNS/连接，
         * 有机会换到另一个解析 IP；4xx 在 ai_http_post 里已读错误体，这里只对连接失败重试。 */
        esp_err_t e = ai_http_post(prov, s_req_buf, s_res_buf, RES_CAP);
        if (e != ESP_OK && (e == ESP_ERR_HTTP_CONNECT || e == ESP_ERR_TIMEOUT)) {
            ESP_LOGW(TAG, "retry once after connect error: %s", esp_err_to_name(e));
            e = ai_http_post(prov, s_req_buf, s_res_buf, RES_CAP);
        }
        if (e == ESP_OK) {
            char ans[512];
            if (parse_content(s_res_buf, ans, sizeof(ans))) {
                set_reply(ans);
                utf8_strncpy(s_last_q, question, sizeof(s_last_q));
                utf8_strncpy(s_last_a, ans, sizeof(s_last_a));
            } else {
                ESP_LOGW(TAG, "parse failed, body: %.160s", s_res_buf);
                set_reply("回复解析失败，请重试");
                g_ai.error = true;
            }
        } else {
            set_reply("网络请求失败，请检查网络与 Key");
            g_ai.error = true;
        }
        g_ai.busy = false;
        g_ai.version++;
    }
}

void ai_chat_init(void)
{
    /* 首次启动：若 NVS 里没有任何 Key，把固件预置的本地 Key 写进去。
     * 只在「所有平台都没配过 Key」时执行一次，之后用户在 UI 里改的优先，
     * 不会被覆盖——避免用户换 Key 后被预置值冲掉。 */
    bool any_key = false;
    for (ai_provider_t p = 0; p < AI_PROVIDER_MAX; p++) {
        char k[8];
        /* 逐平台探测：临时切换 provider 读取太重，直接读 NVS 快路径 */
        nvs_handle_t h;
        if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
            size_t len = sizeof(k);
            if (nvs_get_str(h, s_key_names[p], k, &len) == ESP_OK && k[0]) any_key = true;
            nvs_close(h);
        }
    }
    if (!any_key) {
        ai_chat_provider_set(AI_LOCAL_PROVIDER);
        ai_chat_key_set(AI_LOCAL_KEY);
        ESP_LOGI(TAG, "provisioned local API key (provider=%s)",
                 ai_chat_provider_name(AI_LOCAL_PROVIDER));
    }
    /* 在内部 RAM 栈（app_main）上把 NVS 配置同步到缓存，之后 PSRAM 任务只读缓存 */
    ai_chat_cfg_sync();

    s_req_buf = heap_caps_malloc(REQ_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_res_buf = heap_caps_malloc(RES_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_req_buf || !s_res_buf) {
        ESP_LOGE(TAG, "PSRAM alloc failed, AI chat disabled");
        return;
    }
    s_sem = xSemaphoreCreateBinary();
    if (!s_sem) { ESP_LOGE(TAG, "sem create failed"); return; }

    BaseType_t ok = xTaskCreateWithCaps(ai_task, "ai_chat", 24576, NULL, 3, NULL,
                                        MALLOC_CAP_SPIRAM);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "PSRAM-stack task create failed (%d), retry internal", (int)ok);
        ok = xTaskCreate(ai_task, "ai_chat", 16384, NULL, 3, NULL);
    }
    if (ok != pdPASS) { ESP_LOGE(TAG, "task create failed"); return; }
    ai_provider_t p = ai_chat_provider_get();
    ESP_LOGI(TAG, "init ok (provider=%s, model=%s)",
             s_providers[p].name, s_providers[p].model);
}

void ai_chat_ask(const char *question)
{
    if (!question || !question[0] || !s_sem) return;
    if (g_ai.busy) return;   /* UI 侧也应拦住，这里再兜底 */
    strncpy(s_question, question, sizeof(s_question) - 1);
    s_question[sizeof(s_question) - 1] = '\0';
    xSemaphoreGive(s_sem);
}
