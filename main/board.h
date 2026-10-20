#ifndef __BOARD_H
#define __BOARD_H

/* ============================================================
 *  板级引脚 / 设备地址 集中配置（BSP 层）
 * ------------------------------------------------------------
 *  换板子：只改本文件（必要再动 bsp.c）。
 *  驱动 / 服务 / 界面一律引用这里的宏，不得出现任何裸 GPIO 号，
 *  这样硬件接线变了只动这一层，上层一行不用改。
 *
 *  当前板：ST7789 1.14" 并口 8bit + FT6336 触摸(I2C) + SD(SDMMC 4线)。
 *  音频 / LED 暂未焊接，仅在底部留位置占位。
 * ============================================================ */

#include <stdint.h>
#include "driver/gpio.h"   /* GPIO_NUM_x 需要；v6 已无 driver/i2c.h，I2C 总线号留在 ft6336.h */

/* ---------- LCD：ST7789 并行 8bit 接口（ESP32-S3 GPIO） ---------- */
#define LCD_CS_GPIO     GPIO_NUM_37  /* 片选 */
#define LCD_DC_GPIO     GPIO_NUM_35  /* 数据/命令 RS */
#define LCD_WR_GPIO     GPIO_NUM_36  /* 写时钟 PCLK */
#define LCD_RST_GPIO    GPIO_NUM_21  /* 复位 */
#define LCD_BLK_GPIO    GPIO_NUM_38  /* 背光 */
#define LCD_D0_GPIO     GPIO_NUM_6   /* 数据总线 D0 */
#define LCD_D1_GPIO     GPIO_NUM_7
#define LCD_D2_GPIO     GPIO_NUM_17
#define LCD_D3_GPIO     GPIO_NUM_18
#define LCD_D4_GPIO     GPIO_NUM_8
#define LCD_D5_GPIO     GPIO_NUM_19
#define LCD_D6_GPIO     GPIO_NUM_20
#define LCD_D7_GPIO     GPIO_NUM_3
/* RD 引脚在并口只读屏幕必须接 3.3V，软件不控制 */

/* ---------- 触摸：FT6336（I2C，地址 0x38） ---------- */
#define FT6336_SDA_GPIO GPIO_NUM_5
#define FT6336_SCL_GPIO GPIO_NUM_4
#define FT6336_INT_GPIO GPIO_NUM_40
#define FT6336_I2C_ADDR 0x38         /* 部分模组为 0x39 */

/* ---------- SD 卡：SDMMC 4 线（裸 GPIO 号，sd_card.c 用 (gpio_num_t) 转换） ---------- */
#define SDMMC_PIN_CLK   41
#define SDMMC_PIN_CMD   42
#define SDMMC_PIN_D0    47
#define SDMMC_PIN_D1    48
#define SDMMC_PIN_D2    1
#define SDMMC_PIN_D3    2

/* ---------- 预留（暂未焊接，加功能/换板时在此登记） ----------
 * 音频 NS4160 (PWM/Sigma-Delta 方案): #define BOARD_AUDIO_PIN  GPIO_NUM_x
 * RGB LED WS2812 (RMT 方案)         : #define BOARD_LED_PIN    GPIO_NUM_x
 * 触摸备选 IQS333 (I2C)             : 复用 FT6336 的 SDA/SCL，改地址即可
 */

#endif /* __BOARD_H */
