#ifndef __WIFI_SCAN_H
#define __WIFI_SCAN_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define WIFI_SCAN_MAX_AP 24

typedef struct {
    char    ssid[33];   /* 可能为空（隐藏 SSID） */
    int8_t  rssi;
    uint8_t authmode;    /* wifi_auth_mode_t */
    uint8_t bssid[6];
    uint8_t channel;
} ap_info_t;

/* 扫描结果（由扫描任务填充，主循环读取显示） */
extern ap_info_t g_ap_list[WIFI_SCAN_MAX_AP];
extern int       g_ap_count;
extern bool      g_ap_updated;   /* 有新结果时置 true，主循环消费后清零 */

/* ---------------- STA 连接状态 ---------------- */
typedef enum {
    WIFI_STA_IDLE = 0,      /* 未连接 */
    WIFI_STA_CONNECTING,    /* 正在连接 / 等待 IP */
    WIFI_STA_CONNECTED,     /* 已连接且已拿到 IP */
    WIFI_STA_FAILED         /* 连接失败（密码错误 / 超时 / 找不到 AP） */
} wifi_sta_state_t;

/* 这几个变量由 WiFi 事件回调（event loop 任务）写、LVGL 任务读，故用 volatile。
 * 字符串只是整块读取，不做逐字节同步，UI 显示短暂不一致可接受。 */
extern volatile wifi_sta_state_t g_wifi_state;
extern char g_wifi_ssid[33];     /* 已连接 / 正在连接的 SSID */
extern char g_wifi_ip[16];       /* 已连接时的 IP（点分十进制），未连接为空串 */

/* 初始化 WiFi station 并启动后台扫描任务（不自动周期扫描） */
esp_err_t wifi_scan_init(void);

/* 触发一次扫描（按钮点击时调用） */
void wifi_scan_trigger(void);

/* 注意：不要把这些函数命名成 wifi_sta_connect / wifi_sta_disconnect！
 * ESP-IDF 内部的 libnet80211.a 里已经有同名的 wifi_sta_disconnect
 * （ieee80211_ioctl.o），会被链接器报 multiple definition。
 * 这里统一加 app_ 前缀避免冲突。 */

/* 连接指定 AP。pass 为 NULL 或空串表示开放网络。
 * 凭据会写入 NVS，下次上电自动重连。 */
esp_err_t wifi_app_connect(const char *ssid, const char *pass);

/* 断开当前连接，并清除 NVS 中保存的凭据（之后不再自动重连） */
esp_err_t wifi_app_disconnect(void);

/* NVS 中是否已保存过凭据 */
bool wifi_app_has_saved(void);

#endif /* __WIFI_SCAN_H */
