#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
把任意图片(png/jpg/...)缩放裁切为 WxH，转成 LVGL v9 的 RGB565 未压缩 C 数组。

用法:
    python img2lvgl_rgb565.py <输入图> <输出.c> <变量前缀> <W> <H>

例:
    python tools/img2lvgl_rgb565.py wallpaper.png main/bg_home.c bg_home 320 240

生成内容:
    static const uint8_t <前缀>_map[]                 像素数据(RGB565, Flash rodata)
    const lv_image_dsc_t <前缀>_img;                  图片描述符

说明:
    - 使用 cover 方式(等比缩放后居中裁切)填满 WxH，不拉伸变形。
    - 字节序与 LVGL 官方 LVGLImage.py 的 ColorFormat.RGB565 一致(小端):
      16bit 值 p = (R>>3)<<11 | (G>>2)<<5 | (B>>3)，先写低字节再写高字节。
    - 未压缩，运行期无需解码器(LODEPNG/TJPGD 都无需开启)。
"""
import sys

from PIL import Image, ImageOps


def main():
    if len(sys.argv) != 6:
        print(__doc__)
        sys.exit(1)

    src, dst, name = sys.argv[1], sys.argv[2], sys.argv[3]
    w, h = int(sys.argv[4]), int(sys.argv[5])

    im = Image.open(src).convert("RGB")
    im = ImageOps.fit(im, (w, h), Image.LANCZOS)
    px = im.load()

    buf = bytearray()
    for y in range(h):
        for x in range(w):
            r, g, b = px[x, y]
            p = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
            buf.append(p & 0xFF)
            buf.append((p >> 8) & 0xFF)

    out = []
    out.append("/*******************************************************************************")
    out.append(" * 由 tools/img2lvgl_rgb565.py 自动生成，请勿手工修改")
    out.append(" * 源图: %s" % src)
    out.append(" * 尺寸: %dx%d  格式: RGB565(小端) 未压缩  LVGL v9" % (w, h))
    out.append(" ******************************************************************************/")
    out.append("")
    out.append("#ifdef __has_include")
    out.append("    #if __has_include(\"lvgl.h\")")
    out.append("        #ifndef LV_LVGL_H_INCLUDE_SIMPLE")
    out.append("            #define LV_LVGL_H_INCLUDE_SIMPLE")
    out.append("        #endif")
    out.append("    #endif")
    out.append("#endif")
    out.append("")
    out.append("#ifdef LV_LVGL_H_INCLUDE_SIMPLE")
    out.append("#include \"lvgl.h\"")
    out.append("#else")
    out.append("#include \"lvgl/lvgl.h\"")
    out.append("#endif")
    out.append("")
    out.append("static LV_ATTRIBUTE_LARGE_CONST const uint8_t %s_map[] = {" % name)

    row = []
    for byte in buf:
        row.append("0x%02x," % byte)
        if len(row) == 16:
            out.append("    " + " ".join(row))
            row = []
    if row:
        out.append("    " + " ".join(row))
    out.append("};")
    out.append("")
    out.append("const lv_image_dsc_t %s_img = {" % name)
    out.append("    .header = {")
    out.append("        .magic = LV_IMAGE_HEADER_MAGIC,")
    out.append("        .cf = LV_COLOR_FORMAT_RGB565,")
    out.append("        .flags = 0,")
    out.append("        .w = %d," % w)
    out.append("        .h = %d," % h)
    out.append("        .stride = %d," % (w * 2))
    out.append("        .reserved_2 = 0,")
    out.append("    },")
    out.append("    .data_size = sizeof(%s_map)," % name)
    out.append("    .data = %s_map," % name)
    out.append("};")
    out.append("")

    with open(dst, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(out))

    print("wrote %s : %dx%d, %d bytes pixel data" % (dst, w, h, len(buf)))


if __name__ == "__main__":
    main()
