#include "ble_scan.h"

#include "esp_log.h"
#include "esp_bt.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_hs_adv.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>

static const char *TAG = "BLE_SCAN";

ble_device_t g_ble_dev_list[BLE_SCAN_MAX_DEVICES];
int          g_ble_dev_count = 0;
bool         g_ble_dev_updated = false;

volatile ble_state_t g_ble_state = BLE_STATE_IDLE;
char g_ble_conn_name[32] = {0};

static SemaphoreHandle_t s_scan_sem = NULL;
static SemaphoreHandle_t s_host_sync_sem = NULL;
static volatile bool s_host_synced = false;
static uint16_t s_conn_handle = 0;
static bool s_connected = false;

/* ---------------- 工具函数 ---------------- */

/* 在设备列表中查找或新增设备（按 MAC 地址匹配） */
static ble_device_t *find_or_alloc(const uint8_t addr[6], int8_t addr_type)
{
    for (int i = 0; i < g_ble_dev_count; i++) {
        if (memcmp(g_ble_dev_list[i].addr, addr, 6) == 0 &&
            g_ble_dev_list[i].addr_type == addr_type) {
            return &g_ble_dev_list[i];
        }
    }
    if (g_ble_dev_count >= BLE_SCAN_MAX_DEVICES) return NULL;
    ble_device_t *d = &g_ble_dev_list[g_ble_dev_count++];
    memset(d, 0, sizeof(*d));
    memcpy(d->addr, addr, 6);
    d->addr_type = addr_type;
    return d;
}

/* 从广播数据中提取设备名 */
static void parse_adv_name(ble_device_t *d, const uint8_t *data, uint8_t len)
{
    struct ble_hs_adv_fields fields;
    int rc = ble_hs_adv_parse_fields(&fields, data, len);
    if (rc != 0) {
        ESP_LOGD(TAG, "parse adv fields failed: %d", rc);
        return;
    }

    /* 优先使用完整名称，否则使用缩短名称 */
    const uint8_t *name = NULL;
    int name_len = 0;

    if (fields.name_len > 0 && fields.name != NULL) {
        name = fields.name;
        name_len = fields.name_len;
        ESP_LOGD(TAG, "found name (len=%d): %.*s", name_len, name_len, name);
    }

    if (name_len > 0) {
        int n = name_len;
        if (n >= (int)sizeof(d->name)) n = sizeof(d->name) - 1;
        memcpy(d->name, name, n);
        d->name[n] = '\0';
    }
}

/* ---------------- GAP 事件回调 ---------------- */

static int gap_event_cb(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    switch (event->type) {
    case BLE_GAP_EVENT_DISC: {
        struct ble_gap_disc_desc *desc = &event->disc;

        /* 仅处理通用/受限发现广播（忽略扫描响应，名字在 adv 里） */
        if (desc->event_type != BLE_HCI_ADV_RPT_EVTYPE_ADV_IND &&
            desc->event_type != BLE_HCI_ADV_RPT_EVTYPE_NONCONN_IND &&
            desc->event_type != BLE_HCI_ADV_RPT_EVTYPE_SCAN_RSP) {
            break;
        }

        ble_device_t *d = find_or_alloc(desc->addr.val, desc->addr.type);
        if (!d) break;

        d->rssi = desc->rssi;
        d->is_connectable = (desc->event_type == BLE_HCI_ADV_RPT_EVTYPE_ADV_IND);

        if (desc->length_data > 0) {
            parse_adv_name(d, desc->data, desc->length_data);
            if (desc->length_data <= sizeof(d->adv_data)) {
                memcpy(d->adv_data, desc->data, desc->length_data);
                d->adv_data_len = desc->length_data;
            }
        }

        /* 扫描响应里的名字优先（更完整） */
        if (desc->event_type == BLE_HCI_ADV_RPT_EVTYPE_SCAN_RSP &&
            desc->length_data > 0) {
            parse_adv_name(d, desc->data, desc->length_data);
        }
        break;
    }

    case BLE_GAP_EVENT_DISC_COMPLETE:
        ESP_LOGI(TAG, "scan complete, found %d devices", g_ble_dev_count);
        g_ble_dev_updated = true;
        if (g_ble_state == BLE_STATE_SCANNING) {
            g_ble_state = BLE_STATE_IDLE;
        }
        break;

    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle;
            s_connected = true;
            g_ble_state = BLE_STATE_CONNECTED;
            ESP_LOGI(TAG, "connected, handle=%u", s_conn_handle);
        } else {
            s_connected = false;
            g_ble_state = BLE_STATE_FAILED;
            ESP_LOGW(TAG, "connection failed, status=%d", event->connect.status);
        }
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        s_connected = false;
        g_ble_state = BLE_STATE_IDLE;
        g_ble_conn_name[0] = '\0';
        ESP_LOGI(TAG, "disconnected, reason=%d", event->disconnect.reason);
        break;

    default:
        break;
    }
    return 0;
}

/* ---------------- NimBLE 主机同步 ---------------- */

static void on_sync(void)
{
    s_host_synced = true;
    if (s_host_sync_sem) {
        xSemaphoreGive(s_host_sync_sem);
    }
    ESP_LOGI(TAG, "NimBLE host synced");
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "NimBLE host reset, reason=%d", reason);
}

/* ---------------- 扫描任务 ---------------- */

static void start_scan(void)
{
    if (!s_host_synced) {
        ESP_LOGW(TAG, "start_scan: host not synced yet");
        return;
    }

    /* 先停止已有扫描 */
    ble_gap_disc_cancel();

    g_ble_dev_count = 0;
    g_ble_dev_updated = false;
    g_ble_state = BLE_STATE_SCANNING;

    struct ble_gap_disc_params params = {0};
    params.passive = 1;
    params.itvl = BLE_GAP_SCAN_FAST_INTERVAL_MIN;
    params.window = BLE_GAP_SCAN_FAST_WINDOW;
    params.filter_policy = BLE_HCI_SCAN_FILT_NO_WL;
    params.limited = 0;
    params.filter_duplicates = 1;

    /* 扫描 10 秒 */
    int rc = ble_gap_disc(BLE_OWN_ADDR_PUBLIC, 10000, &params, gap_event_cb, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGE(TAG, "start scan failed: %d", rc);
        g_ble_state = BLE_STATE_IDLE;
    } else {
        ESP_LOGI(TAG, "scan started (rc=%d)", rc);
    }
}

static void ble_scan_task(void *arg)
{
    (void)arg;

    /* 等待 NimBLE 主机同步 */
    if (xSemaphoreTake(s_host_sync_sem, pdMS_TO_TICKS(10000)) != pdTRUE) {
        ESP_LOGE(TAG, "host sync timeout");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "BLE scan task ready");

    for (;;) {
        if (xSemaphoreTake(s_scan_sem, portMAX_DELAY) == pdTRUE) {
            start_scan();
        }
    }
}

static void ble_host_task(void *param)
{
    (void)param;
    ESP_LOGI(TAG, "BLE host task started");
    nimble_port_run();
    nimble_port_freertos_deinit();
}

/* ---------------- 公开 API ---------------- */

esp_err_t ble_scan_init(void)
{
    esp_err_t ret;

    /* 释放经典蓝牙内存（只用 BLE） */
    ret = esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
    ESP_LOGI(TAG, "bt mem release: %d", ret);

    /* 初始化整个 NimBLE 栈（控制器 + 主机 + HCI） */
    ret = nimble_port_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed: %s (%d)", esp_err_to_name(ret), ret);
        return ret;
    }
    ESP_LOGI(TAG, "nimble_port_init ok");

    /* 注册回调 */
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;

    /* 同步信号量 */
    s_host_sync_sem = xSemaphoreCreateBinary();
    s_scan_sem = xSemaphoreCreateBinary();

    /* 启动主机任务（处理 NimBLE 事件） */
    nimble_port_freertos_init(ble_host_task);

    /* 启动扫描任务 */
    xTaskCreate(ble_scan_task, "ble_scan", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "BLE scan initialized");
    return ESP_OK;
}

void ble_scan_trigger(void)
{
    if (s_scan_sem) {
        xSemaphoreGive(s_scan_sem);
    }
}

esp_err_t ble_app_connect(const uint8_t addr[6], int8_t addr_type)
{
    if (!s_host_synced) return ESP_ERR_INVALID_STATE;

    /* 停止扫描再连接 */
    ble_gap_disc_cancel();

    g_ble_state = BLE_STATE_CONNECTING;

    /* 在列表中找设备名 */
    for (int i = 0; i < g_ble_dev_count; i++) {
        if (memcmp(g_ble_dev_list[i].addr, addr, 6) == 0 &&
            g_ble_dev_list[i].addr_type == addr_type) {
            strncpy(g_ble_conn_name, g_ble_dev_list[i].name, sizeof(g_ble_conn_name) - 1);
            g_ble_conn_name[sizeof(g_ble_conn_name) - 1] = '\0';
            break;
        }
    }

    struct ble_gap_conn_params params = {0};
    params.scan_itvl = 80;
    params.scan_window = 40;
    params.itvl_min = 24;
    params.itvl_max = 40;
    params.latency = 0;
    params.supervision_timeout = 200;
    params.min_ce_len = 1;
    params.max_ce_len = 1;

    ble_addr_t peer;
    peer.type = addr_type;
    memcpy(peer.val, addr, 6);

    int rc = ble_gap_connect(BLE_OWN_ADDR_PUBLIC, &peer, 5000, &params, gap_event_cb, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "connect failed: %d", rc);
        g_ble_state = BLE_STATE_FAILED;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "connecting to %02X:%02X:%02X:%02X:%02X:%02X",
             addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]);
    return ESP_OK;
}

esp_err_t ble_app_disconnect(void)
{
    if (!s_connected) {
        g_ble_state = BLE_STATE_IDLE;
        g_ble_conn_name[0] = '\0';
        return ESP_OK;
    }

    int rc = ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    if (rc != 0 && rc != BLE_HS_ENOTCONN) {
        ESP_LOGW(TAG, "disconnect failed: %d", rc);
        return ESP_FAIL;
    }
    return ESP_OK;
}
