#ifndef __BLE_SCAN_H
#define __BLE_SCAN_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define BLE_SCAN_MAX_DEVICES 24

typedef struct {
    char     name[32];       /* 设备名（来自 adv data / scan rsp，空串=无名设备） */
    uint8_t  addr[6];        /* MAC 地址（公网 / 随机） */
    int8_t   addr_type;      /* 0=public, 1=random, 2=public_id, 3=random_id */
    int8_t   rssi;           /* 信号强度 (dBm) */
    uint8_t  adv_data[31];   /* 原始广播数据 */
    uint8_t  adv_data_len;
    bool     is_connectable; /* 是否可连接 */
} ble_device_t;

/* 扫描结果（扫描任务写，主循环读） */
extern ble_device_t g_ble_dev_list[BLE_SCAN_MAX_DEVICES];
extern int          g_ble_dev_count;
extern bool         g_ble_dev_updated;   /* 有新结果时置 true，主循环消费后清零 */

/* 连接状态 */
typedef enum {
    BLE_STATE_IDLE = 0,
    BLE_STATE_SCANNING,
    BLE_STATE_CONNECTING,
    BLE_STATE_CONNECTED,
    BLE_STATE_FAILED
} ble_state_t;

extern volatile ble_state_t g_ble_state;
extern char g_ble_conn_name[32];   /* 当前连接/正在连接的设备名 */

/* 初始化 NimBLE 主机并启动后台扫描任务 */
esp_err_t ble_scan_init(void);

/* 触发一次扫描（进入蓝牙界面时调用） */
void ble_scan_trigger(void);

/* 连接指定设备（按 MAC 地址匹配） */
esp_err_t ble_app_connect(const uint8_t addr[6], int8_t addr_type);

/* 断开当前连接 */
esp_err_t ble_app_disconnect(void);

#endif /* __BLE_SCAN_H */
