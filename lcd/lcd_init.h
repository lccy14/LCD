#ifndef __LCD_INIT_H
#define __LCD_INIT_H

#include "sys.h"
#include "driver/gpio.h"
#include "esp_lcd_panel_io.h"   /* esp_lcd_panel_io_handle_t / esp_lcd_panel_io_event_data_t */

#define USE_HORIZONTAL 2  // ST7789 1.14寸，当前横屏显示模式 2/3 为横屏

#if USE_HORIZONTAL==0||USE_HORIZONTAL==1
#define LCD_W 240
#define LCD_H 320
#else
#define LCD_W 320
#define LCD_H 240
#endif

//-----------------LCD 并行 8bit 接口引脚 (ESP32-S3 GPIO)----------------
// 按用户指定接线：ST7789 并行 8bit 接口
#define LCD_CS_GPIO     GPIO_NUM_37  // 片选
#define LCD_DC_GPIO     GPIO_NUM_35  // 数据/命令 (RS)
#define LCD_WR_GPIO     GPIO_NUM_36  // 写时钟 (PCLK)
#define LCD_RST_GPIO    GPIO_NUM_21  // 复位
#define LCD_BLK_GPIO    GPIO_NUM_38  // 背光

// 8bit 数据总线（D0-D7）
#define LCD_D0_GPIO     GPIO_NUM_6   // D0
#define LCD_D1_GPIO     GPIO_NUM_7   // D1
#define LCD_D2_GPIO     GPIO_NUM_17  // D2
#define LCD_D3_GPIO     GPIO_NUM_18  // D3
#define LCD_D4_GPIO     GPIO_NUM_8   // D4
#define LCD_D5_GPIO     GPIO_NUM_19  // D5
#define LCD_D6_GPIO     GPIO_NUM_20  // D6
#define LCD_D7_GPIO     GPIO_NUM_3   // D7

// RD 引脚在并口只读屏幕上必须接 3.3V，软件不控制

// 控制宏（保留兼容，方便其他文件直接操作）
#define LCD_CS_Clr()   gpio_set_level(LCD_CS_GPIO, 0)
#define LCD_CS_Set()   gpio_set_level(LCD_CS_GPIO, 1)
#define LCD_DC_Clr()   gpio_set_level(LCD_DC_GPIO, 0)
#define LCD_DC_Set()   gpio_set_level(LCD_DC_GPIO, 1)
#define LCD_WR_Clr()   gpio_set_level(LCD_WR_GPIO, 0)
#define LCD_WR_Set()   gpio_set_level(LCD_WR_GPIO, 1)
#define LCD_RST_Clr()  gpio_set_level(LCD_RST_GPIO, 0)
#define LCD_RST_Set()  gpio_set_level(LCD_RST_GPIO, 1)
#define LCD_BLK_Clr()  gpio_set_level(LCD_BLK_GPIO, 0)
#define LCD_BLK_Set()  gpio_set_level(LCD_BLK_GPIO, 1)

void LCD_GPIO_Init(void);                // 初始化 GPIO + I80 并行总线
void LCD_Writ_Bus(u8 dat);               // 并口写一个字节（调试用）
void LCD_Writ_Buf(const u8 *data, u32 len); // 并口批量写（DMA）
void LCD_WR_DATA8(u8 dat);               // 写一个字节数据
void LCD_WR_DATA(u16 dat);               // 写两个字节数据
void LCD_WR_REG(u8 dat);                 // 写一个命令
void LCD_Address_Set(u16 x1,u16 y1,u16 x2,u16 y2); // 设置显示区域
void LCD_Init(void);                     // LCD 初始化

/* 注册 I80 颜色传输完成回调（DMA 刷屏完成时调用，ISR 上下文）。
 * 上层（LVGL）用它来调 lv_disp_flush_ready，让 LVGL 知道可以渲染下一帧。
 * 必须在 LCD_Init() 之前调用。*/
void lcd_set_color_trans_done_cb(bool (*cb)(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t *, void *), void *ctx);

/* 等待一次 I80 颜色 DMA 完成（true=等到；false=超时无完成=队列已空）/ 清空已完成计数。
 * 视频直写 framebuffer 后必须等，防下一帧解码与 DMA 抢缓冲导致花屏。 */
bool LCD_WaitFlushDone(uint32_t timeout_ms);
void LCD_DrainFlushDone(void);

#endif
