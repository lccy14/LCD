#pragma once
#include "lvgl.h"

/* 贪吃蛇: 上下左右滑动控制方向, 吃食物加分, 撞墙/撞自己 = 死亡 */
void snake_create_screen(void);
void snake_update(void);
bool snake_is_active(void);
