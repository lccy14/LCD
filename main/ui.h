#pragma once

#include <stdbool.h>
#include "lvgl.h"

/* 选择显示模式：1 = LVGL 模式，0 = 原始帧缓冲绘制演示。
 * 可用编译宏 -DUSE_LVGL=0 覆盖，切回原始帧缓冲测试。 */
#ifndef USE_LVGL
#define USE_LVGL 1
#endif

/* UI 程序统一入口：根据 USE_LVGL 选择 LVGL 界面或原始帧缓冲测试 */
void ui_app_start(void);

/* App 编号，用于懒加载各屏幕；同时也是打开/返回动画定位图标的索引 */
typedef enum {
    APP_NONE = 0,     /* 无来源界面：切屏不带展开动画 */
    APP_WIFI = 1,
    APP_SETTINGS,
    APP_CLOCK,
    APP_MUSIC,
    APP_GAME,
    APP_WEATHER,
    APP_NOVEL,
    APP_PCMON,
    APP_ALARM,
    APP_BLE,
    APP_FILES
} app_id_t;

/* 打开某个 App 界面：带屏幕切换动画（当前用的是 MOVE_LEFT，两屏一起左移） */
void ui_open_screen_anim(lv_obj_t *scr, app_id_t id);

/* 回到主界面：当前界面收缩回它的图标（当前 App 已在内部记录，无需外部传参） */
void ui_go_home(void);

/* 隐藏/恢复顶部系统状态栏 (layer_top 上的统一实例)。
 * 供贪吃蛇等需要全屏的游戏界面临时隐藏。 */
void ui_set_status_bar_visible(bool vis);
