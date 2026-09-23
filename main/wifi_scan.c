#include "wifi_scan.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "time_sync.h"   /* 连上网后自动校时 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "string.h"

static const char *TAG = "WIFISCAN";

ap_info_t g_ap_list[WIFI_SCAN_MAX_AP];
int       g_ap_count = 0;
bool      g_ap_updated = false;

static SemaphoreHandle_t s_scan_sem = NULL;

/* ---------------- STA 连接相关 ---------------- */
#define NVS_NS_WIFI   "wificfg"
#define NVS_KEY_SSID  "ssid"
#define NVS_KEY_PASS  "pass"
#define WIFI_MAX_RETRY 5

volatile wifi_sta_state_t g_wifi_state = WIFI_STA_IDLE;
char g_wifi_ssid[33] = {0};
char g_wifi_ip[16]   = {0};

/* 已保存凭据的副本（开机自动重连用），避免反复读 NVS */
static char  s_saved_ssid[33] = {0};
static char  s_saved_pass[65] = {0};
static bool  s_saved_valid = false;
static uint8_t s_retry = 0;
/* 标记「这次断开是我们自己主动调用的 esp_wifi_disconnect() 造成的」。
 * 用户点断开、或切换网络前都会先断开，这会触发 WIFI_EVENT_STA_DISCONNECTED，
 * 若不区分就会被判成连接失败（状态栏变红）。 */
static bool  s_manual_disconnect = false;

/* 从 NVS 读取已保存的凭据 */
static void saved_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS_WIFI, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    size_t len = sizeof(s_saved_ssid);
    if (nvs_get_str(h, NVS_KEY_SSID, s_saved_ssid, &len) == ESP_OK && s_saved_ssid[0] != '\0') {
        size_t plen = sizeof(s_saved_pass);
        if (nvs_get_str(h, NVS_KEY_PASS, s_saved_pass, &plen) != ESP_OK) {
            s_saved_pass[0] = '\0';
        }
        s_saved_valid = true;
        ESP_LOGI(TAG, "saved AP found: %s", s_saved_ssid);
    }
    nvs_close(h);
}

/* 把凭据写入 NVS */
static void saved_store(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS_WIFI, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open for write failed");
        return;
    }
    nvs_set_str(h, NVS_KEY_SSID, ssid);
    nvs_set_str(h, NVS_KEY_PASS, pass ? pass : "");
    nvs_commit(h);
    nvs_close(h);
}

/* 清除 NVS 中的凭据 */
static void saved_clear(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS_WIFI, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_erase_key(h, NVS_KEY_SSID);
    nvs_erase_key(h, NVS_KEY_PASS);
    nvs_commit(h);
    nvs_close(h);
}

/* 执行一次扫描并刷新全局结果 */
static void do_scan(void)
{
    wifi_ap_record_t records[WIFI_SCAN_MAX_AP];
    uint16_t num = WIFI_SCAN_MAX_AP;
    esp_err_t r = esp_wifi_scan_start(NULL, true);   // 阻塞直到扫描完成
    if (r != ESP_OK) {
        ESP_LOGE(TAG, "scan start failed: %d", r);
        return;
    }
    if (esp_wifi_scan_get_ap_num(&num) != ESP_OK) {
        num = 0;
    }
    if (num > WIFI_SCAN_MAX_AP) {
        num = WIFI_SCAN_MAX_AP;
    }
    if (esp_wifi_scan_get_ap_records(&num, records) == ESP_OK) {
        g_ap_count = (int)num;
        for (int i = 0; i < num; i++) {
            memset(g_ap_list[i].ssid, 0, sizeof(g_ap_list[i].ssid));
            if (records[i].ssid[0] != '\0') {
                strncpy(g_ap_list[i].ssid, (char *)records[i].ssid, 32);
            }
            g_ap_list[i].rssi     = records[i].rssi;
            g_ap_list[i].authmode = records[i].authmode;
            memcpy(g_ap_list[i].bssid, records[i].bssid, 6);
            g_ap_list[i].channel  = records[i].primary;
        }
        g_ap_updated = true;
        ESP_LOGI(TAG, "scan done, found %d APs", num);
    }
}

static void wifi_scan_task(void *arg)
{
    (void)arg;
    while (1) {
        if (xSemaphoreTake(s_scan_sem, portMAX_DELAY) == pdTRUE) {
            do_scan();
        }
    }
}

/* ---------------- STA 连接 ---------------- */

/* 实际发起连接（假定凭据已写入 s_saved_*） */
static esp_err_t sta_connect_saved(void)
{
    wifi_config_t cfg = {0};
    strncpy((char *)cfg.sta.ssid, s_saved_ssid, sizeof(cfg.sta.ssid) - 1);
    strncpy((char *)cfg.sta.password, s_saved_pass, sizeof(cfg.sta.password) - 1);
    /* 阈值设 OPEN 表示接受任意加密方式的 AP（WPA/WPA2/WPA3 都能连），
     * 若按扫描到的 authmode 精确设置，遇到 WPA3 或混合模式反而会连不上。 */
    cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
    cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;

    /* 到这里说明是主动发起的连接，清掉手动断开标记 */
    s_manual_disconnect = false;

    esp_err_t r = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    if (r != ESP_OK) {
        ESP_LOGE(TAG, "set_config failed: %d", r);
        return r;
    }
    r = esp_wifi_connect();
    if (r != ESP_OK) {
        ESP_LOGE(TAG, "connect failed: %d", r);
        return r;
    }

    g_wifi_state = WIFI_STA_CONNECTING;
    strncpy(g_wifi_ssid, s_saved_ssid, sizeof(g_wifi_ssid) - 1);
    g_wifi_ssid[sizeof(g_wifi_ssid) - 1] = '\0';
    g_wifi_ip[0] = '\0';
    s_retry = 0;
    ESP_LOGI(TAG, "connecting to %s", s_saved_ssid);
    return ESP_OK;
}

esp_err_t wifi_app_connect(const char *ssid, const char *pass)
{
    if (!ssid || ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    /* 先断开旧连接，再改配置（连着的时候改 STA 配置可能不生效）。
     * 这次断开是我们主动的，标记一下，别让事件回调把它当连接失败。 */
    s_manual_disconnect = true;
    esp_wifi_disconnect();
    g_wifi_state = WIFI_STA_CONNECTING;
    g_wifi_ip[0] = '\0';

    strncpy(s_saved_ssid, ssid, sizeof(s_saved_ssid) - 1);
    s_saved_ssid[sizeof(s_saved_ssid) - 1] = '\0';
    if (pass) {
        strncpy(s_saved_pass, pass, sizeof(s_saved_pass) - 1);
        s_saved_pass[sizeof(s_saved_pass) - 1] = '\0';
    } else {
        s_saved_pass[0] = '\0';
    }
    s_saved_valid = true;
    saved_store(s_saved_ssid, s_saved_pass);

    return sta_connect_saved();
}

esp_err_t wifi_app_disconnect(void)
{
    saved_clear();
    s_saved_valid = false;
    s_saved_ssid[0] = '\0';
    s_saved_pass[0] = '\0';

    s_manual_disconnect = true;
    esp_err_t r = esp_wifi_disconnect();
    g_wifi_state = WIFI_STA_IDLE;
    g_wifi_ssid[0] = '\0';
    g_wifi_ip[0] = '\0';
    return r;
}

bool wifi_app_has_saved(void)
{
    return s_saved_valid;
}

/* WiFi / IP 事件处理：跑在 event loop 任务里，只更新全局状态，不做重活 */
static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_START:
            ESP_LOGI(TAG, "STA start");
            /* 有保存的凭据就自动重连（开机即用） */
            if (s_saved_valid) {
                sta_connect_saved();
            }
            break;

        case WIFI_EVENT_STA_CONNECTED:
            ESP_LOGI(TAG, "connected to AP, waiting for IP");
            g_wifi_state = WIFI_STA_CONNECTING;   /* 拿到 IP 才算真正可用 */
            s_retry = 0;
            break;

        case WIFI_EVENT_STA_DISCONNECTED: {
            wifi_event_sta_disconnected_t *d = (wifi_event_sta_disconnected_t *)data;
            g_wifi_ip[0] = '\0';
            if (s_manual_disconnect) {
                /* 主动断开 / 切换网络：属于正常状态，不是失败 */
                s_manual_disconnect = false;
                g_wifi_state = WIFI_STA_IDLE;
                ESP_LOGI(TAG, "disconnected by user");
            } else if (s_saved_valid && s_retry < WIFI_MAX_RETRY) {
                s_retry++;
                g_wifi_state = WIFI_STA_CONNECTING;
                ESP_LOGW(TAG, "disconnected (reason=%d), retry %d/%d",
                         d->reason, s_retry, WIFI_MAX_RETRY);
                esp_wifi_connect();
            } else {
                g_wifi_state = WIFI_STA_FAILED;
                ESP_LOGE(TAG, "disconnected (reason=%d), give up", d->reason);
            }
            break;
        }
        default:
            break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        esp_ip4addr_ntoa(&ev->ip_info.ip, g_wifi_ip, sizeof(g_wifi_ip));
        g_wifi_state = WIFI_STA_CONNECTED;
        s_retry = 0;
        time_sync_start();   /* 拿到公网 IP，启动 SNTP 自动校时 */
        ESP_LOGI(TAG, "got IP: %s", g_wifi_ip);
    }
}

void wifi_scan_trigger(void)
{
    if (s_scan_sem) {
        xSemaphoreGive(s_scan_sem);
    }
}

esp_err_t wifi_scan_init(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    /* 事件回调必须在 esp_wifi_start() 之前注册，否则收不到 WIFI_EVENT_STA_START，
     * 开机自动重连就不会触发。 */
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               &wifi_event_handler, NULL));

    /* 先读出保存的凭据，STA 启动后由 STA_START 事件触发自动连接 */
    saved_load();

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    s_scan_sem = xSemaphoreCreateBinary();
    xTaskCreate(wifi_scan_task, "wifiscan", 5120, NULL, 3, NULL);
    ESP_LOGI(TAG, "wifi scan ready (scan on demand)");
    return ESP_OK;
}
