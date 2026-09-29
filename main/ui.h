#pragma once

#include <stdbool.h>

/* 选择显示模式：1 = LVGL 模式，0 = 原始帧缓冲绘制演示。
 * 可用编译宏 -DUSE_LVGL=0 覆盖，切回原始帧缓冲测试。 */
#ifndef USE_LVGL
#define USE_LVGL 1
#endif

/* UI 程序统一入口：根据 USE_LVGL 选择 LVGL 界面或原始帧缓冲测试 */
void ui_app_start(void);

/* 隐藏/恢复顶部系统状态栏 (layer_top 上的统一实例)。
 * 供贪吃蛇等需要全屏的游戏界面临时隐藏。 */
void ui_set_status_bar_visible(bool vis);
