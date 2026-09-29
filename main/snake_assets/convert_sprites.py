"""把 Snake.png (64x64, 4x4 网格, 16x16 每 sprite) 切成 16 个 ARGB8888 C 数组。

白色背景作为 chroma key 转透明, 输出 snake_sprites.c/h。
LV_COLOR_FORMAT_ARGB8888 在内存中是 B,G,R,A (小端 uint32 = 0xAARRGGBB)。
"""
from PIL import Image
import sys, os

SRC = os.path.join(os.path.dirname(__file__), "Snake.png")
OUT_C = os.path.join(os.path.dirname(__file__), "snake_sprites.c")
OUT_H = os.path.join(os.path.dirname(__file__), "snake_sprites.h")

SPRITE = 16
COLS, ROWS = 4, 4
TOTAL = COLS * ROWS  # 16

img = Image.open(SRC).convert("RGBA")
W, H = img.size
assert (W, H) == (SPRITE * COLS, SPRITE * ROWS), f"unexpected size {W}x{H}"
print(f"Snake.png: {W}x{H}")

# 取一个像素判断背景色 (右下角应该是空白)
bg = img.getpixel((W - 1, H - 1))
print(f"background sample: {bg}")

def is_bg(rgba):
    # spritesheet 本身就是 alpha=0 透明背景, 只看 alpha 通道
    return rgba[3] == 0

# 提取 16 个 sprite
sprites = []  # each: list of 256 uint32
for idx in range(TOTAL):
    sx = (idx % COLS) * SPRITE
    sy = (idx // COLS) * SPRITE
    pixels = []
    for py in range(SPRITE):
        for px in range(SPRITE):
            r, g, b, a = img.getpixel((sx + px, sy + py))
            if a == 0 or is_bg((r, g, b, a)):
                # 透明
                pixels.append(0x00000000)
            else:
                # ARGB8888 uint32: alpha<<24 | red<<16 | green<<8 | blue
                pixels.append((a << 24) | (r << 16) | (g << 8) | b)
    sprites.append(pixels)

# 写 .h
with open(OUT_H, "w", encoding="utf-8") as f:
    f.write("#pragma once\n")
    f.write("#include <stdint.h>\n\n")
    f.write("/* Auto-generated from Snake.png (CC0 by eugeneloza, github.com/eugeneloza/SnakeGame).\n")
    f.write(" * 16x16 sprite, ARGB8888, 16 sprites in 4x4 grid.\n")
    f.write(" * Layout:\n")
    f.write(" *  0  HEAD_UP    1  HEAD_RIGHT  2  HEAD_DOWN  3  HEAD_LEFT\n")
    f.write(" *  4  BODY_HZ    5  BODY_VT     6  TURN_TL    7  TURN_TR\n")
    f.write(" *  8  TURN_BL    9  TURN_BR    10 TAIL_UP    11 TAIL_RIGHT\n")
    f.write(" * 12 TAIL_DOWN  13 TAIL_LEFT   14 FOOD       15 EMPTY\n")
    f.write(" */\n\n")
    f.write("#define SNAKE_SPRITE_PX  16\n")
    f.write("#define SNAKE_SPRITE_CNT  16\n\n")
    f.write("/* [sprite_index][pixel_index], 256 pixels each, ARGB8888 (0xAARRGGBB). */\n")
    f.write("extern const uint32_t snake_sprite_data[SNAKE_SPRITE_CNT][SNAKE_SPRITE_PX * SNAKE_SPRITE_PX];\n")

# 写 .c
with open(OUT_C, "w", encoding="utf-8") as f:
    f.write("/* Auto-generated from Snake.png (CC0 by eugeneloza, github.com/eugeneloza/SnakeGame). */\n")
    f.write("/* DO NOT EDIT - regenerate via convert_sprites.py. */\n")
    f.write('#include "snake_sprites.h"\n\n')
    f.write("const uint32_t snake_sprite_data[SNAKE_SPRITE_CNT][SNAKE_SPRITE_PX * SNAKE_SPRITE_PX] = {\n")
    for i, px in enumerate(sprites):
        f.write(f"  /* [{i}] */ {{\n    ")
        for j, v in enumerate(px):
            f.write(f"0x{v:08X}")
            if j < len(px) - 1:
                f.write(",")
            if (j + 1) % 16 == 0:
                f.write("\n    ")
            else:
                f.write(" ")
        f.write("\n  }")
        if i < len(sprites) - 1:
            f.write(",")
        f.write("\n")
    f.write("};\n")

# 统计非空像素, 验证切图正确
print("\nSprite non-transparent pixel counts:")
labels = ["HEAD_UP","HEAD_RT","HEAD_DN","HEAD_LT",
          "BODY_HZ","BODY_VT","TURN_TL","TURN_TR",
          "TURN_BL","TURN_BR","TAIL_UP","TAIL_RT",
          "TAIL_DN","TAIL_LT","FOOD","EMPTY"]
for i, px in enumerate(sprites):
    nonzero = sum(1 for v in px if v != 0)
    print(f"  [{i:2d}] {labels[i]:9s}: {nonzero:3d}/256 px")

print(f"\nWrote {OUT_C}")
print(f"Wrote {OUT_H}")
