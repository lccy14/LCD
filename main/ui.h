#pragma once

/* 选择显示模式：1 = LVGL 模式，0 = 原始帧缓冲绘制演示。
 * 可用编译宏 -DUSE_LVGL=0 覆盖，切回原始帧缓冲测试。 */
#ifndef USE_LVGL
#define USE_LVGL 1
#endif

/* UI 程序统一入口：根据 USE_LVGL 选择 LVGL 界面或原始帧缓冲测试 */
void ui_app_start(void);
