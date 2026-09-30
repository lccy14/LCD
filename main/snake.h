#pragma once
#include "lvgl.h"

/* 贪吃蛇: 右侧方向键控制(按下即转向并立即走一步), 吃食物加分, 撞墙/撞自己 = 死亡 */
void snake_create_screen(void);
void snake_update(void);
bool snake_is_active(void);
