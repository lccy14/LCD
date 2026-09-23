#ifndef __LVGL_PORT_H
#define __LVGL_PORT_H

#include "lvgl.h"

/**
 * LVGL 显示后端适配：
 *  - LVGL 的 draw buffer 直接复用 lcd.c 里的 g_framebuffer（零拷贝）
 *  - flush_cb 调用 LCD_Flush_All() 把整块缓冲 DMA 刷到屏幕
 *  - 用 esp_timer 周期性给 LVGL 提供 tick
 */

void lvgl_port_init(void);   // 初始化显示 + tick，必须在 lcd_init() 之后调用
void lvgl_port_tick(void);   // 由 esp_timer 回调调用，给 LVGL 喂 1ms tick

#endif
