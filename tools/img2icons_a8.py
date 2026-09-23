#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
把一组透明背景的图标 PNG 转成 LVGL v9 的 A8（纯透明通道）图片数据，打包成一个 C 文件。

为什么要 A8:
  - 只存 alpha，1 字节/像素，40x40 图标仅 1.6KB，8 个共约 13KB；
  - 无 RGB 字节序问题（ARGB8888/RGB565 都要处理大小端，易翻车）；
  - 显示时用 image_recolor 着色成任意颜色（本项目染成白色，压在彩色圆角方块上）。

用法:
    python tools/img2icons_a8.py <图标目录> <输出.c> <目标边长>

例:
    python tools/img2icons_a8.py assets/icons main/app_icons.c 40
"""
import os
import struct
import sys

from PIL import Image

HEADER = """/*******************************************************************************
 * 由 tools/img2icons_a8.py 自动生成，请勿手工修改
 * 图标来源: Google Material Icons (Apache License 2.0)
 *           https://github.com/google/material-design-icons
 * 格式: A8 (纯 alpha 通道)，运行时用 image_recolor 染成白色
 ******************************************************************************/

#ifdef __has_include
    #if __has_include("lvgl.h")
        #ifndef LV_LVGL_H_INCLUDE_SIMPLE
            #define LV_LVGL_H_INCLUDE_SIMPLE
        #endif
    #endif
#endif

#ifdef LV_LVGL_H_INCLUDE_SIMPLE
#include "lvgl.h"
#else
#include "lvgl/lvgl.h"
#endif

"""


def alpha_rows(im, size):
    """把图片缩放到 size x size，返回逐行 alpha 列表"""
    im = im.convert("RGBA")
    im = im.resize((size, size), Image.LANCZOS)
    alpha = im.split()[-1]               # 取 alpha 通道
    data = alpha.tobytes()
    return [data[y * size:(y + 1) * size] for y in range(size)]


def emit(name, size, rows):
    lines = []
    lines.append("static LV_ATTRIBUTE_LARGE_CONST const uint8_t %s_map[] = {" % name)
    for row in rows:
        parts = ["0x%02x," % b for b in row]
        for i in range(0, len(parts), 16):
            lines.append("    " + " ".join(parts[i:i + 16]))
    lines.append("};")
    lines.append("")
    lines.append("const lv_image_dsc_t %s = {" % name)
    lines.append("    .header = {")
    lines.append("        .magic = LV_IMAGE_HEADER_MAGIC,")
    lines.append("        .cf = LV_COLOR_FORMAT_A8,")
    lines.append("        .flags = 0,")
    lines.append("        .w = %d," % size)
    lines.append("        .h = %d," % size)
    lines.append("        .stride = %d," % size)
    lines.append("        .reserved_2 = 0,")
    lines.append("    },")
    lines.append("    .data_size = sizeof(%s_map)," % name)
    lines.append("    .data = %s_map," % name)
    lines.append("};")
    lines.append("")
    return lines


def main():
    if len(sys.argv) != 4:
        print(__doc__)
        sys.exit(1)

    src_dir, dst, size = sys.argv[1], sys.argv[2], int(sys.argv[3])

    files = sorted(f for f in os.listdir(src_dir) if f.lower().endswith(".png"))
    if not files:
        print("目录里没有 PNG:", src_dir)
        sys.exit(1)

    out = [HEADER]
    total = 0
    for f in files:
        base = os.path.splitext(f)[0]
        # LVGL 变量名统一成 ico_<名字>，避免与其它符号冲突
        name = "ico_" + base.replace("-", "_").replace(" ", "_")
        im = Image.open(os.path.join(src_dir, f))
        rows = alpha_rows(im, size)
        out += emit(name, size, rows)
        n = size * size
        total += n
        print("  %-18s <- %-16s %dx%d  %d bytes" % (name, f, size, size, n))

    with open(dst, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("\n".join(out).rstrip() + "\n")

    print("\n生成 %s : %d 个图标, 合计 %d bytes (%.1f KB)" % (dst, len(files), total, total / 1024.0))


if __name__ == "__main__":
    main()
