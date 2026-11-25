#pragma once

#include <stdbool.h>
#include "esp_err.h"

/* USB MSC 模拟U盘：把 SD 卡通过 USB 暴露给电脑。
 * 硬件：ESP32-S3 USB OTG 固定 GPIO19(D-)/GPIO20(D+)（第二版 PCB 空闲引出）。
 * 注意：U 盘模式下 SD 卡被电脑独占，设备端 FATFS 卸载，
 *       音乐/录音/TTS 全部暂停；关闭开关后恢复。 */
typedef enum {
    USB_MSC_IDLE = 0,     /* 未启用，SD 卡由设备 FATFS 使用 */
    USB_MSC_STARTING,     /* 正在切换（FATFS 卸载 + SDMMC 重初始化） */
    USB_MSC_ACTIVE,       /* U 盘模式，SD 卡由电脑独占 */
    USB_MSC_STOPPING,     /* 正在恢复 */
} usb_msc_state_t;

/* 查询当前状态 */
usb_msc_state_t usb_msc_get_state(void);
bool usb_msc_is_active(void);

/* 进入 U 盘模式（独占 SD 卡）。必须在正常 FATFS 已挂载时调用。
 * 内部会：停音频/录音/TTS → 卸载 FATFS → 裸初始化 SDMMC → 安装 MSC。 */
esp_err_t usb_msc_start(void);

/* 退出 U 盘模式，恢复设备端 FATFS 挂载。 */
esp_err_t usb_msc_stop(void);

/* 全局初始化（当前为空，预留；TinyUSB driver 在进入 U 盘模式时才安装） */
void usb_msc_init(void);
