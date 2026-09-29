"""dump Snake.png 食物 sprite 的所有颜色分布, 找正确 chroma key."""
from PIL import Image
from collections import Counter

import os
img = Image.open(os.path.join(os.path.dirname(__file__), "Snake.png")).convert("RGBA")

# 食物 sprite 在 (3,2) -> (48..63, 48..63)
print("== Food sprite (48,48)..(63,63) color distribution ==")
cnt = Counter()
for y in range(48, 64):
    for x in range(48, 64):
        cnt[img.getpixel((x, y))] += 1
for c, n in cnt.most_common(15):
    print(f"  {c}: {n}")

# 蛇头 sprite (0,0)..(15,15)
print("\n== Head_UP sprite (0,0)..(15,15) color distribution ==")
cnt = Counter()
for y in range(0, 16):
    for x in range(0, 16):
        cnt[img.getpixel((x, y))] += 1
for c, n in cnt.most_common(15):
    print(f"  {c}: {n}")

# EMPTY sprite (3,3) -> (48..63, 48..63)? 不对, EMPTY 是 (3,3) -> (48..63, 48..63)
# 等等, 4x4 grid, EMPTY 是 idx 15 -> (col=3, row=3) -> (48..63, 48..63)
# 食物是 idx 14 -> (col=2, row=3) -> (32..47, 48..63)
print("\n== Food sprite is idx 14 -> (32,48)..(47,63) ==")
cnt = Counter()
for y in range(48, 64):
    for x in range(32, 48):
        cnt[img.getpixel((x, y))] += 1
for c, n in cnt.most_common(15):
    print(f"  {c}: {n}")
