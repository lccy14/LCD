#include "weather.h"
#include "wifi_scan.h"      /* g_wifi_state 等 */
#include "esp_http_client.h"
#include "esp_err.h"        /* esp_err_to_name */
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h" /* xTaskCreateWithCaps 的 MALLOC_CAP_SPIRAM */
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "WEATHER";

/* 天气调试日志开关：默认 0(关闭)。改 1 可打印 任务启动/唤醒 定位日志。
 * 注意: weather updated / geocode / forecast 失败 属功能日志，始终打印。 */
#define WEATHER_DEBUG 0

#define WX_DEFAULT_CITY "Shenzhen"   /* 城市名为空时的显示名回退 */
/* 手动指定城市：留空("")则自动定位（公网 IP，可能跨城市不准）；
 * 填城市名（如 "Shenzhen"）则固定使用该城市，走地理编码，结果最稳。 */
#define WX_MANUAL_CITY "Shenzhen"
/* 天气数据源：sojson（又拍云 CDN，国内直连，免 Key，响应 ~100ms）。
 * 原 Open-Meteo 域名解析到欧洲 OVH 节点，跨境链路频繁丢 SYN 导致 15s 超时。
 * sojson 只支持 HTTP + 城市代码（不支持 HTTPS / 城市名），天气数据不敏感，
 * 明文 HTTP 可接受；城市代码用下方映射表，查不到回退默认城市。 */
#define WX_FC_URL  "http://t.weather.sojson.com/api/weather/city/%s"
#define WX_DEFAULT_CODE "101280601"   /* 深圳 */

/* 城市名（英文/中文）→ sojson 城市代码。设备无城市切换 UI，覆盖默认+常见即可。 */
typedef struct { const char *name; const char *code; } wx_city_code_t;
static const wx_city_code_t s_city_codes[] = {
    {"Shenzhen", "101280601"}, {"深圳", "101280601"},
    {"Shanghai", "101020100"}, {"上海", "101020100"},
    {"Beijing",  "101010100"}, {"北京", "101010100"},
    {"Guangzhou","101280101"}, {"广州", "101280101"},
    {"Hangzhou", "101210101"}, {"杭州", "101210101"},
    {"Chengdu",  "101270101"}, {"成都", "101270101"},
    {"Wuhan",    "101200101"}, {"武汉", "101200101"},
    {"Nanjing",  "101190101"}, {"南京", "101190101"},
    {"Chongqing","101040100"}, {"重庆", "101040100"},
    {"Xian",     "101110101"}, {"西安", "101110101"},
    {"Tianjin",  "101030100"}, {"天津", "101030100"},
    {"Suzhou",   "101190401"}, {"苏州", "101190401"},
    {"Changsha", "101250101"}, {"长沙", "101250101"},
    {"Xiamen",   "101230201"}, {"厦门", "101230201"},
    {"Hong Kong","101320101"}, {"香港", "101320101"},
};

weather_data_t g_weather = {0};

static SemaphoreHandle_t s_req_sem = NULL;
static char s_req_city[40];
static bool s_req_has_city = false;

/* ---------------- 极简 JSON 取值 ---------------- */

/* 找到 "key": 之后值的起始位置（要求 key 被双引号包裹，避免误命中值里的内容）。
 * 返回值可能是字符串（以 " 开头）或数值；由调用方按类型处理。
 * 用 from 指定搜索起点，便于 js_get_double 跳过同名字符串值后继续找下一个。 */
static const char *js_find_val_from(const char *from, const char *key)
{
    size_t kl = strlen(key);
    const char *p = from;
    while ((p = strstr(p, key)) != NULL) {
        if (p > from && p[-1] == '"' && p[kl] == '"') {
            const char *c = p + kl + 1;
            while (*c && *c != ':' && *c != '}' && *c != ',') c++;
            if (*c == ':') {
                const char *v = c + 1;
                while (*v == ' ' || *v == '\t' || *v == '\n' || *v == '\r') v++;
                return v;
            }
        }
        p += kl;
    }
    return NULL;
}

static const char *js_find_val(const char *s, const char *key)
{
    return js_find_val_from(s, key);
}

static bool js_get_str(const char *s, const char *key, char *out, size_t n)
{
    const char *v = js_find_val(s, key);
    if (!v || *v != '"') {
        return false;
    }
    const char *q = v + 1;
    size_t i = 0;
    while (*q && *q != '"' && i + 1 < n) {
        out[i++] = *q++;
    }
    out[i] = '\0';
    return i > 0;
}

/* sojson 的数值多是字符串（如 "wendu":"18.1"、"shidu":"66%"），
 * 取引号内内容再转数字；匹配前缀数字，自动跳过 ℃/% 等后缀。 */
static bool js_get_str_num(const char *s, const char *key, double *out)
{
    char buf[32];
    if (!js_get_str(s, key, buf, sizeof(buf))) return false;
    char *end = NULL;
    double v = strtod(buf, &end);
    if (end == buf) return false;
    *out = v;
    return true;
}

/* 从字符串里提取第一个数字（如 "高温 21℃"→21、"PM2.5"跳过字母找数）。
 * 找不到返回 false。用于 sojson 的 high/low 原文。 */
static bool extract_num(const char *s, double *out)
{
    if (!s) return false;
    while (*s) {
        if ((*s >= '0' && *s <= '9') || *s == '-' || *s == '.') break;
        s++;
    }
    char *end = NULL;
    double v = strtod(s, &end);
    if (end == s) return false;
    *out = v;
    return true;
}

/* 取数值型 JSON 字段（pm25 等是裸数字，非字符串） */
static bool js_get_num(const char *s, const char *key, double *out)
{
    const char *v = js_find_val(s, key);
    if (!v || *v == '"') return false;
    char *end = NULL;
    double d = strtod(v, &end);
    if (end == v) return false;
    *out = d;
    return true;
}

/* 城市名（英文/中文）→ sojson 代码，查不到回退默认（深圳） */
static const char *city_code(const char *name)
{
    if (name && name[0]) {
        for (size_t i = 0; i < sizeof(s_city_codes)/sizeof(s_city_codes[0]); i++) {
            if (strcasecmp(name, s_city_codes[i].name) == 0) return s_city_codes[i].code;
        }
    }
    return WX_DEFAULT_CODE;
}

/* 中文城市名→英文名（UI 城市标题用 montserrat_20，不含汉字，必须显示英文）。
 * 按 sojson 代码匹配映射表里的英文项，查不到则回退传入的查询名。 */
static const char *city_english(const char *code, const char *fallback)
{
    for (size_t i = 0; i < sizeof(s_city_codes)/sizeof(s_city_codes[0]); i++) {
        if (strcmp(code, s_city_codes[i].code) == 0 && s_city_codes[i].name[0] >= 'A')
            return s_city_codes[i].name;   /* 英文项（大写字母开头） */
    }
    return fallback;
}

/* ---------------- HTTP GET（sojson 是明文 HTTP，无需证书校验） ---------------- */

static esp_err_t http_get(const char *url, char *buf, size_t cap)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .user_agent = "esp32-weather/1.0",
        .timeout_ms = 8000,    /* 国内直连 ~100ms，8s 足够；失败快速返回不干等 */
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        return ESP_FAIL;
    }
    /* 强制服务器返回未压缩内容；即便返回 gzip，开启 CONFIG_ESP_HTTP_CLIENT_ENABLE_GZIP
     * 后 esp_http_client 也会自动解压，避免拿到二进制导致 JSON 解析失败。 */
    esp_http_client_set_header(c, "Accept-Encoding", "identity");

    esp_err_t err = ESP_FAIL;
    esp_err_t open_err = esp_http_client_open(c, 0);     /* 0 = GET，无请求体 */
    if (open_err == ESP_OK) {
        int content_length = esp_http_client_fetch_headers(c);
        int status = esp_http_client_get_status_code(c);
        if (status == 200) {
            int total = 0, r;
            while (total < (int)cap - 1 &&
                   (r = esp_http_client_read(c, buf + total, cap - 1 - total)) > 0) {
                total += r;
            }
            buf[total] = '\0';
            err = ESP_OK;
        } else {
            ESP_LOGE(TAG, "HTTP status %d for %s", status, url);
            buf[0] = '\0';
            err = ESP_FAIL;
        }
        (void)content_length;
        esp_http_client_close(c);       /* 显式关闭，避免半开连接残留占 socket */
    } else {
        /* 打印错误码：ESP_ERR_HTTP_CONNECT / ESP_ERR_TIMEOUT 等能直接区分
         * 「主机不可达」与「服务器响应慢」，比原来一句 open failed 有用得多。
         * 同时附上当时的内存余量：若INT/PSRAM 都还充裕，就能直接排除「内存不足
         * 导致连不上」这一类怀疑（内存不足通常报 ESP_ERR_NO_MEM 而非超时）。 */
        ESP_LOGE(TAG, "HTTP open failed (%s) for %s", esp_err_to_name(open_err), url);
        ESP_LOGE(TAG, "  此刻内存: INT free=%u big=%u  PSRAM free=%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        buf[0] = '\0';
    }
    esp_http_client_cleanup(c);
    return err;
}

static void weather_task(void *arg)
{
    (void)arg;
    /* 用 static 缓冲，避免大响应体占用任务栈 */
    static char fc[4096];   /* sojson 返回含 15 天预报，约 3KB */

    for (;;) {
        xSemaphoreTake(s_req_sem, portMAX_DELAY);
#if WEATHER_DEBUG
        ESP_LOGI(TAG, "weather task woke, wifi_state=%d", g_wifi_state);
#endif

        if (g_wifi_state != WIFI_STA_CONNECTED) {
            ESP_LOGW(TAG, "WiFi not connected, skip weather fetch");
            continue;
        }

        g_weather.fetching = true;

        /* sojson 按城市代码查，一次 HTTP 搞定，无 geocoding/定位的境外请求。
         * 城市名来自 UI 传入或 WX_MANUAL_CITY，代码查不到回退默认（深圳）。 */
        const char *city = s_req_has_city ? s_req_city
                                          : (WX_MANUAL_CITY[0] ? WX_MANUAL_CITY : NULL);
        char cname[40];
        strncpy(cname, city && city[0] ? city : WX_DEFAULT_CITY, sizeof(cname) - 1);
        cname[sizeof(cname) - 1] = '\0';
        const char *code = city_code(city);

        char fc_url[256];
        snprintf(fc_url, sizeof(fc_url), WX_FC_URL, code);

        double temp = 0, hum = 0;
        char desc[28] = "未知";
        esp_err_t fc_err = http_get(fc_url, fc, sizeof(fc));
        /* sojson: data.wendu="18.1" / data.shidu="66%" / data.forecast[0].type="阴" */
        if (fc_err == ESP_OK &&
            js_get_str_num(fc, "wendu", &temp) &&
            js_get_str_num(fc, "shidu", &hum) &&
            js_find_val(fc, "type")) {

            js_get_str(fc, "type", desc, sizeof(desc));       /* 首个 type = 今天天气 */
            /* UI 城市标题用 montserrat_20（不含汉字），显示英文名而非 sojson 的中文名，
             * 否则三个汉字会渲染成方框（看着像占位符）。 */
            strncpy(g_weather.city, city_english(code, cname), sizeof(g_weather.city) - 1);
            g_weather.city[sizeof(g_weather.city) - 1] = '\0';

            /* 扩展字段：从 sojson 的 data 根级 + forecast[0]（今天）提取。
             * high/low 原文形如 "高温 21℃"，直接存原文，UI 显示时截取数字更灵活。 */
            char raw[40];
            double num;
            g_weather.high[0] = g_weather.low[0] = '\0';
            if (js_get_str(fc, "high", raw, sizeof(raw))) {
                if (extract_num(raw, &num)) snprintf(g_weather.high, sizeof(g_weather.high), "%.0f", num);
            }
            if (js_get_str(fc, "low", raw, sizeof(raw))) {
                if (extract_num(raw, &num)) snprintf(g_weather.low, sizeof(g_weather.low), "%.0f", num);
            }
            js_get_str(fc, "fx", g_weather.wind_dir, sizeof(g_weather.wind_dir));
            js_get_str(fc, "fl", g_weather.wind_level, sizeof(g_weather.wind_level));
            js_get_str(fc, "quality", g_weather.air_quality, sizeof(g_weather.air_quality));
            js_get_str(fc, "sunrise", g_weather.sunrise, sizeof(g_weather.sunrise));
            js_get_str(fc, "sunset", g_weather.sunset, sizeof(g_weather.sunset));
            /* notice 较长且含中文，只取今天第一个 */
            js_get_str(fc, "notice", g_weather.notice, sizeof(g_weather.notice));
            if (js_get_num(fc, "pm25", &num)) g_weather.pm25 = (int)num;
            else g_weather.pm25 = -1;

            /* desc 已是中文（阴/晴/小雨），用 cn_16 显示；code 兼容字段置 0（UI 不用） */
            g_weather.temp     = (float)temp;
            g_weather.humidity = (int)hum;
            g_weather.code     = 0;
            g_weather.wind     = 0;    /* sojson 实况无精确风速，风力等级在 wind_level */
            strncpy(g_weather.desc, desc, sizeof(g_weather.desc) - 1);
            g_weather.desc[sizeof(g_weather.desc) - 1] = '\0';
            g_weather.updated_ms = esp_timer_get_time() / 1000;
            g_weather.valid   = true;
            g_weather.error   = false;
            g_weather.version++;
            ESP_LOGI(TAG, "weather updated: %s %.1fC %s RH%d%% %s%s 空气%s PM2.5=%d",
                     g_weather.city, temp, g_weather.desc, g_weather.humidity,
                     g_weather.wind_dir, g_weather.wind_level,
                     g_weather.air_quality, g_weather.pm25);
        } else {
            if (fc_err == ESP_OK) {
                ESP_LOGW(TAG, "forecast parse failed, body: %.*s",
                         (int)strnlen(fc, 160), fc);
            }
            g_weather.error = true;
            ESP_LOGW(TAG, "forecast fetch/parse failed");
        }

        g_weather.fetching = false;
    }
}

void weather_init(void)
{
    s_req_sem = xSemaphoreCreateBinary();
    /* 栈放 PSRAM：HTTPS 抓取需 ~32KB 栈，若放内部 RAM 会和 LVGL 的两块 38400B
     * draw_buf 互相挤占，导致任一方分配失败(屏幕卡死 / 任务没建却打了 started)。
     * PSRAM 充裕(>800KB)，把任务栈放那里，屏幕与天气两者兼顾。 */
    BaseType_t ok = xTaskCreateWithCaps(weather_task, "weather", 32768, NULL, 3, NULL,
                                        MALLOC_CAP_SPIRAM);
    if (ok != pdPASS) {
        /* 极端情况 PSRAM 也失败：降档用内部 RAM 小栈兜底 */
        ESP_LOGE(TAG, "PSRAM-stack task create failed (%d), retry internal @24KB", (int)ok);
        ok = xTaskCreate(weather_task, "weather", 24576, NULL, 3, NULL);
    }
#if WEATHER_DEBUG
    ESP_LOGI(TAG, "weather task started (create=%d)", (int)ok);
#endif
}

void weather_request(const char *city)
{
    if (city && city[0]) {
        strncpy(s_req_city, city, sizeof(s_req_city) - 1);
        s_req_city[sizeof(s_req_city) - 1] = '\0';
        s_req_has_city = true;
    } else {
        s_req_has_city = false;
    }
    if (s_req_sem) {
        xSemaphoreGive(s_req_sem);
    }
}
