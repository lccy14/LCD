#pragma once

#include <stdbool.h>
#include "esp_err.h"

/* ============================================================
 * USB UAC 声卡（喇叭 + 麦克风），与 USB 模拟U盘二选一切换
 * ------------------------------------------------------------
 * 进入 UAC 模式：停音乐/录音/TTS → 卸载 U盘 → 安装 UAC 描述符
 *   + 初始化 I2S0(喇叭 TX) / I2S1(麦克风 RX) → uac_device_init
 * 退出：反向恢复。
 * 采样率 16kHz / 16bit / 单声道（与麦克风硬件、描述符一致）。
 * ============================================================ */

esp_err_t usb_uac_start(void);
esp_err_t usb_uac_stop(void);
bool      usb_uac_is_active(void);
