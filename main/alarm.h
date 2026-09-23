#pragma once
#include <stdbool.h>
#include <time.h>

/* 闹钟：3 个独立闹钟，设置保存在 NVS，重启后依然有效。
 * 数据由 alarm.c 维护，UI 层只读/调用接口，不直接改内部变量。 */

#define ALARM_MAX 3

typedef struct {
    int  hour;      /* 0..23 */
    int  minute;    /* 0..59 */
    bool enabled;   /* 总开关 */
} alarm_conf_t;

/* 程序启动时调用一次，从 NVS 载入上次设置。
 * 必须在 nvs_flash_init() 之后调用——本项目由 wifi_scan_init() 内部完成，
 * 因此在 lvgl_ui_bootstrap() 里要排在 wifi_scan_init() 之后。 */
void alarm_init(void);

/* 取得第 idx 个闹钟的设置（返回内部静态对象指针，只读） */
const alarm_conf_t *alarm_get(int idx);

/* 修改第 idx 个闹钟设置，内部自动写入 NVS */
void alarm_set(int idx, int hour, int minute, bool enabled);

/* 每秒调用一次，传入当前本地时间。
 * 到达任一已启用闹钟的「时:分」且本次尚未触发过则返回 true，并通过 out_idx
 * 返回触发的闹钟 idx（同一分钟内只返回一次）。
 * 全部未触发时返回 false。 */
bool alarm_poll(const struct tm *now, int *out_idx);

/* 用户点了「停止」：标记本次已处理，同一分钟内不再重复弹窗 */
void alarm_dismiss(void);
