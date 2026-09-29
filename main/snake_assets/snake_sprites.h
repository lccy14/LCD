#pragma once
#include <stdint.h>

/* Auto-generated from Snake.png (CC0 by eugeneloza, github.com/eugeneloza/SnakeGame).
 * 16x16 sprite, ARGB8888, 16 sprites in 4x4 grid.
 * Layout:
 *  0  HEAD_UP    1  HEAD_RIGHT  2  HEAD_DOWN  3  HEAD_LEFT
 *  4  BODY_HZ    5  BODY_VT     6  TURN_TL    7  TURN_TR
 *  8  TURN_BL    9  TURN_BR    10 TAIL_UP    11 TAIL_RIGHT
 * 12 TAIL_DOWN  13 TAIL_LEFT   14 FOOD       15 EMPTY
 */

#define SNAKE_SPRITE_PX  16
#define SNAKE_SPRITE_CNT  16

/* [sprite_index][pixel_index], 256 pixels each, ARGB8888 (0xAARRGGBB). */
extern const uint32_t snake_sprite_data[SNAKE_SPRITE_CNT][SNAKE_SPRITE_PX * SNAKE_SPRITE_PX];
