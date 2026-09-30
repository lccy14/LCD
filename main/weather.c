#include "weather.h"
#include "wifi_scan.h"      /* g_wifi_state 等 */
#include "esp_http_client.h"
#include "esp_crt_bundle.h" /* esp_crt_bundle_attach，用于校验服务器证书 */
#include "esp_log.h"
#include "esp_timer.h"
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "WEATHER";

#define WX_DEFAULT_CITY "Shanghai"
/* 手动指定城市：留空("")则自动定位（公网 IP，可能跨城市不准）；
 * 填城市名（如 "Shenzhen"）则固定使用该城市，走地理编码，结果最稳。 */
#define WX_MANUAL_CITY "Shenzhen"
/* 默认城市（上海）经纬度：地理编码失败时的回退值，保证天气仍能显示 */
#define WX_DEF_LAT 31.2304
#define WX_DEF_LON 121.4737
/* Open-Meteo 免费、无需 API Key、支持 HTTPS；geocoding 把城市名转经纬度 */
#define WX_GEO_URL "https://geocoding-api.open-meteo.com/v1/search?name=%s&count=1&language=en&format=json"
#define WX_FC_URL  "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f&current=temperature_2m,relative_humidity_2m,weather_code,wind_speed_10m&timezone=auto"
/* 自动定位：用设备公网 IP 获取城市与经纬度（城市级精度，免 API Key）。
 * ipinfo.io 返回 {"city":"...","loc":"lat,lon",...}；定位失败则回退默认城市。 */
#define WX_LOC_URL "https://ipinfo.io/json"

weather_data_t g_weather = {0};

static SemaphoreHandle_t s_req_sem = NULL;
static char s_req_city[40];
static bool s_req_has_city = false;

/* 自动定位得到的经纬度/城市（定位成功后缓存，避免每次刷新都打 IP 定位服务） */
static double s_loc_lat = 0, s_loc_lon = 0;
static char   s_loc_city[40];
static bool   s_loc_valid = false;

/* ---------------- 极简 JSON 取值（针对 Open-Meteo 固定结构） ---------------- */

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

static bool js_get_double(const char *s, const char *key, double *out)
{
    const char *from = s;
    while (1) {
        const char *v = js_find_val_from(from, key);
        if (!v) {
            return false;
        }
        /* Open-Meteo 在 current_units 里用同名键、但值是字符串单位（如 "°C"），
         * 跳过字符串型值，继续找下一个同名键，直到命中数值那次。 */
        if (*v == '"') {
            from = v + 1;
            continue;
        }
        char *end = NULL;
        double val = strtod(v, &end);
        if (end == v) {                       /* 没解析到数字，继续找下一个 */
            from = v + 1;
            continue;
        }
        *out = val;
        return true;
    }
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

/* WMO weather code -> 英文描述 */
static void wmo_desc(int code, char *out, size_t n)
{
    const char *s;
    switch (code) {
        case 0:  s = "晴"; break;
        case 1:  s = "少云"; break;
        case 2:  s = "多云"; break;
        case 3:  s = "阴"; break;
        case 45: s = "雾"; break;
        case 48: s = "雾凇"; break;
        case 51: s = "毛毛雨"; break;
        case 53: s = "细雨"; break;
        case 55: s = "密集细雨"; break;
        case 56: case 57: s = "冻毛雨"; break;
        case 61: s = "小雨"; break;
        case 63: s = "中雨"; break;
        case 65: s = "大雨"; break;
        case 66: case 67: s = "冻雨"; break;
        case 71: s = "小雪"; break;
        case 73: s = "中雪"; break;
        case 75: s = "大雪"; break;
        case 77: s = "米雪"; break;
        case 80: s = "小阵雨"; break;
        case 81: s = "中阵雨"; break;
        case 82: s = "暴雨"; break;
        case 85: s = "小阵雪"; break;
        case 86: s = "强阵雪"; break;
        case 95: s = "雷阵雨"; break;
        case 96: case 99: s = "雷阵雨伴冰雹"; break;
        default: s = "未知"; break;
    }
    strncpy(out, s, n - 1);
    out[n - 1] = '\0';
}

/* ---------------- HTTPS GET（带证书 bundle 校验） ---------------- */

static esp_err_t http_get(const char *url, char *buf, size_t cap)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach, /* 用 ESP 内置 CA 包校验 Let's Encrypt 等证书 */
        .user_agent = "esp32-weather/1.0",          /* 部分 CDN/服务器会拦截无 UA 的请求 */
        .timeout_ms = 15000,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        return ESP_FAIL;
    }
    /* 强制服务器返回未压缩内容；即便返回 gzip，开启 CONFIG_ESP_HTTP_CLIENT_ENABLE_GZIP
     * 后 esp_http_client 也会自动解压，避免拿到二进制导致 JSON 解析失败。 */
    esp_http_client_set_header(c, "Accept-Encoding", "identity");

    esp_err_t err = ESP_FAIL;
    if (esp_http_client_open(c, 0) == ESP_OK) {          /* 0 = GET，无请求体 */
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
    } else {
        ESP_LOGE(TAG, "HTTP open failed for %s", url);
        buf[0] = '\0';
    }
    esp_http_client_cleanup(c);
    return err;
}

/* 把 URL 里的空格做最简单编码（%20），避免城市名带空格时请求失败 */
static void url_encode_spaces(char *url, size_t cap)
{
    for (size_t i = 0; url[i]; ) {
        if (url[i] == ' ') {
            size_t rest = strlen(url + i);
            if (rest + 2 < cap - i) {
                memmove(url + i + 3, url + i + 1, rest); /* 右移 2 字节腾出 %20 */
                url[i] = '%'; url[i + 1] = '2'; url[i + 2] = '0';
                i += 3;
            } else {
                break;
            }
        } else {
            i++;
        }
    }
}

/* 自动定位：用设备公网 IP 获取城市与经纬度（城市级精度，免 API Key）。
 * ipinfo.io 返回 {"city":"...","loc":"lat,lon",...}；失败返回 false。 */
static bool auto_locate(double *lat, double *lon, char *city, size_t n)
{
    char buf[256];
    if (http_get(WX_LOC_URL, buf, sizeof(buf)) != ESP_OK) {
        return false;
    }
    char loc[40];
    if (!js_get_str(buf, "loc", loc, sizeof(loc))) {
        return false;
    }
    if (sscanf(loc, "%lf,%lf", lat, lon) != 2) {
        return false;
    }
    if (!js_get_str(buf, "city", city, n)) {
        strncpy(city, "Unknown", n - 1);
        city[n - 1] = '\0';
    }
    ESP_LOGI(TAG, "auto locate: %s (%.4f, %.4f)", city, *lat, *lon);
    return true;
}

static void weather_task(void *arg)
{
    (void)arg;
    /* 用 static 缓冲，避免大响应体占用任务栈（TLS 握手本身栈消耗不小） */
    static char geo[2048];
    static char fc[2048];

    for (;;) {
        xSemaphoreTake(s_req_sem, portMAX_DELAY);

        if (g_wifi_state != WIFI_STA_CONNECTED) {
            ESP_LOGW(TAG, "WiFi not connected, skip weather fetch");
            continue;
        }

        g_weather.fetching = true;

        /* 1) 确定经纬度与城市名
         *    - 指定了城市（UI 传入或下方 WX_MANUAL_CITY 配置）：走地理编码
         *    - 都未指定：自动定位（公网 IP -> 城市/经纬度），成功则缓存；
         *      失败再回退到默认城市“Shanghai”的地理编码 */
        double lat = WX_DEF_LAT, lon = WX_DEF_LON;
        char cname[40];

        const char *city = s_req_has_city ? s_req_city
                                          : (WX_MANUAL_CITY[0] ? WX_MANUAL_CITY : NULL);

        if (city) {
            strncpy(cname, city, sizeof(cname) - 1);
            cname[sizeof(cname) - 1] = '\0';

            char geo_url[256];
            snprintf(geo_url, sizeof(geo_url), WX_GEO_URL, city);
            url_encode_spaces(geo_url, sizeof(geo_url));

            esp_err_t geo_err = http_get(geo_url, geo, sizeof(geo));
            if (geo_err == ESP_OK &&
                js_get_double(geo, "latitude", &lat) &&
                js_get_double(geo, "longitude", &lon) &&
                js_get_str(geo, "name", cname, sizeof(cname))) {
                ESP_LOGI(TAG, "geocode ok: %s (%.4f, %.4f)", cname, lat, lon);
            } else {
                if (geo_err == ESP_OK) {
                    ESP_LOGW(TAG, "geocode parse failed, body: %.*s",
                             (int)strnlen(geo, 160), geo);
                }
                ESP_LOGW(TAG, "geocode failed for '%s', fallback to default coords", city);
            }
        } else {
            if (s_loc_valid) {
                lat = s_loc_lat; lon = s_loc_lon;
                strncpy(cname, s_loc_city, sizeof(cname) - 1);
                cname[sizeof(cname) - 1] = '\0';
                ESP_LOGI(TAG, "use cached location: %s (%.4f, %.4f)", cname, lat, lon);
            } else if (auto_locate(&lat, &lon, cname, sizeof(cname))) {
                s_loc_lat = lat; s_loc_lon = lon;
                strncpy(s_loc_city, cname, sizeof(s_loc_city) - 1);
                s_loc_city[sizeof(s_loc_city) - 1] = '\0';
                s_loc_valid = true;
            } else {
                /* 自动定位失败，回退到默认城市地理编码 */
                strncpy(cname, WX_DEFAULT_CITY, sizeof(cname) - 1);
                cname[sizeof(cname) - 1] = '\0';

                char geo_url[256];
                snprintf(geo_url, sizeof(geo_url), WX_GEO_URL, WX_DEFAULT_CITY);
                url_encode_spaces(geo_url, sizeof(geo_url));

                esp_err_t geo_err = http_get(geo_url, geo, sizeof(geo));
                if (geo_err == ESP_OK &&
                    js_get_double(geo, "latitude", &lat) &&
                    js_get_double(geo, "longitude", &lon) &&
                    js_get_str(geo, "name", cname, sizeof(cname))) {
                    ESP_LOGI(TAG, "geocode ok (fallback): %s (%.4f, %.4f)", cname, lat, lon);
                } else {
                    if (geo_err == ESP_OK) {
                        ESP_LOGW(TAG, "geocode parse failed, body: %.*s",
                                 (int)strnlen(geo, 160), geo);
                    }
                    ESP_LOGW(TAG, "auto locate & geocode failed, use default coords");
                }
            }
        }

        /* 2) 当前天气：经纬度 -> 实况 */
        char fc_url[256];
        snprintf(fc_url, sizeof(fc_url), WX_FC_URL, lat, lon);

        double temp = 0, hum = 0, wind = 0, d_code = 0;
        esp_err_t fc_err = http_get(fc_url, fc, sizeof(fc));
        if (fc_err == ESP_OK &&
            js_get_double(fc, "temperature_2m", &temp) &&
            js_get_double(fc, "relative_humidity_2m", &hum) &&
            js_get_double(fc, "weather_code", &d_code) &&
            js_get_double(fc, "wind_speed_10m", &wind)) {

            g_weather.temp     = (float)temp;
            g_weather.humidity = (int)hum;
            g_weather.code     = (int)d_code;
            g_weather.wind     = (float)wind;
            strncpy(g_weather.city, cname, sizeof(g_weather.city) - 1);
            g_weather.city[sizeof(g_weather.city) - 1] = '\0';
            wmo_desc(g_weather.code, g_weather.desc, sizeof(g_weather.desc));
            g_weather.updated_ms = esp_timer_get_time() / 1000;
            g_weather.valid   = true;
            g_weather.error   = false;
            g_weather.version++;
            ESP_LOGI(TAG, "weather updated: %s %.1fC %s (RH %d%%, wind %.1f km/h)",
                     cname, temp, g_weather.desc, g_weather.humidity, wind);
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
    xTaskCreate(weather_task, "weather", 32768, NULL, 3, NULL);
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
