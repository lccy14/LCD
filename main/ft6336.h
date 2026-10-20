#ifndef __FT6336_H
#define __FT6336_H

#include <stdint.h>
#include <stdbool.h>
#include "board.h"   /* 引脚 / I2C 地址集中定义，换板只改 board.h */

#define FT6336_I2C_PORT I2C_NUM_0    /* I2C 控制器编号：驱动层选哪条总线，非 GPIO 接线 */

/* 一个触摸点的状态 */
typedef struct {
    bool     pressed;   /* 是否按下 */
    uint16_t x;         /* 原始 x 坐标 */
    uint16_t y;         /* 原始 y 坐标 */
} touch_point_t;

esp_err_t ft6336_init(void);
esp_err_t ft6336_read(touch_point_t *tp);

/* 查 INT 引脚电平: true=触摸中(低电平), false=无触摸(高电平)。
 * 用于在 touch_read_cb 里快速过滤,无触摸时不读 I2C 省时间。 */
bool ft6336_touched(void);

#endif /* __FT6336_H */
