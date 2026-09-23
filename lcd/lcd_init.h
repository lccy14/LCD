#ifndef __LCD_INIT_H
#define __LCD_INIT_H

#include "sys.h"
#include "driver/gpio.h"

#define USE_HORIZONTAL 2  // ST7789 1.14寸 横屏显示（2/3 为横屏）

#if USE_HORIZONTAL==0||USE_HORIZONTAL==1
#define LCD_W 240
#define LCD_H 320
#else
#define LCD_W 320
#define LCD_H 240
#endif

//-----------------LCD 引脚定义 (ESP32 GPIO)----------------
// 用户可根据实际接线修改以下引脚号
#define LCD_SCLK_GPIO   GPIO_NUM_5  // SCLK
#define LCD_MOSI_GPIO   GPIO_NUM_6  // MOSI
#define LCD_RES_GPIO    GPIO_NUM_19   // RES
#define LCD_DC_GPIO     GPIO_NUM_3   // DC
#define LCD_CS_GPIO     GPIO_NUM_20   // CS
#define LCD_BLK_GPIO    GPIO_NUM_8  // BLK

// 宏定义：设置/清除电平（SCLK/MOSI 由硬件 SPI 管理，不再使用位带宏）
#define LCD_RES_Clr()   gpio_set_level(LCD_RES_GPIO, 0)
#define LCD_RES_Set()   gpio_set_level(LCD_RES_GPIO, 1)

#define LCD_DC_Clr()    gpio_set_level(LCD_DC_GPIO, 0)
#define LCD_DC_Set()    gpio_set_level(LCD_DC_GPIO, 1)

#define LCD_CS_Clr()    gpio_set_level(LCD_CS_GPIO, 0)
#define LCD_CS_Set()    gpio_set_level(LCD_CS_GPIO, 1)

#define LCD_BLK_Clr()   gpio_set_level(LCD_BLK_GPIO, 0)
#define LCD_BLK_Set()   gpio_set_level(LCD_BLK_GPIO, 1)

void LCD_GPIO_Init(void);                // 初始化 GPIO + SPI 控制器
void LCD_Writ_Bus(u8 dat);               // SPI 写入一个字节
void LCD_Writ_Buf(const u8 *data, u32 len); // 批量数据写入（DMA）
void LCD_WR_DATA8(u8 dat);               // 写入一个字节
void LCD_WR_DATA(u16 dat);               // 写入两个字节
void LCD_WR_REG(u8 dat);                 // 写入一个指令
void LCD_Address_Set(u16 x1,u16 y1,u16 x2,u16 y2); // 设置坐标函数
void LCD_Init(void);                     // LCD 初始化

#endif
