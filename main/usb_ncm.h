#pragma once

#include <stdbool.h>
#include "esp_err.h"

/* ============================================================
 * USB 网络共享（NCM）—— L2 透明桥
 * ------------------------------------------------------------
 * 电脑插 USB 后，把 ESP32 的 WiFi STA 当作自己的网卡：
 *   电脑 ←USB NCM→ ESP32 ←WiFi STA→ 路由器
 * 复用工程现有的 WiFi 连接（wifi_scan.c 的 g_wifi_state），
 * 在 L2 层转发以太网帧（esp_wifi_internal_tx/rx），不走 lwIP IP 转发。
 * 前提：启动前 WiFi 必须已连上路由器（WIFI_STA_CONNECTED）。
 * 与 U盘 / 声卡 / CDC 共用 USB 口，四选一互斥。
 * ============================================================ */

esp_err_t usb_ncm_start(void);
esp_err_t usb_ncm_stop(void);
bool      usb_ncm_is_active(void);
