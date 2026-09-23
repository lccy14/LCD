#ifndef __FT6336_H
#define __FT6336_H

#include <stdint.h>
#include <stdbool.h>
#include "driver/gpio.h"

/* 引脚（ESP32 侧接线，按用户最初指定 SDA=35/SCL=36/INT=37） */
#define FT6336_SDA_GPIO  GPIO_NUM_35
#define FT6336_SCL_GPIO  GPIO_NUM_36
#define FT6336_INT_GPIO  GPIO_NUM_37

/* I2C 地址：FT6336 通常为 0x38（部分模组 0x39） */
#define FT6336_I2C_ADDR  0x38
#define FT6336_I2C_PORT  I2C_NUM_0

/* 一个触摸点的状态 */
typedef struct {
    bool     pressed;   /* 是否按下 */
    uint16_t x;         /* 原始 x 坐标 */
    uint16_t y;         /* 原始 y 坐标 */
} touch_point_t;

esp_err_t ft6336_init(void);
esp_err_t ft6336_read(touch_point_t *tp);

#endif /* __FT6336_H */
