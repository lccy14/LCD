"""检查源码里会显示出来的中文/符号是否都在 lv_font_cn_16 的字库范围内。

用法: python tools/check_font_glyphs.py [字库范围(可选)]

只检查 C 字符串字面量（注释、ASCII 艺术里的制表符不算）。
"""
import glob
import re
import sys

# lv_font_cn_16.c 头部记录的实际生成范围
DEFAULT_RANGES = [
    (0x20, 0x7E),      # ASCII
    (0xB0, 0xB0),      # °
    (0xB7, 0xB7),      # ·
    (0x3000, 0x303F),  # CJK 标点
    (0xFF01, 0xFF5E),  # 全角
    (0x4E00, 0x9FA5),  # CJK 基本汉字
]


def in_font(cp, ranges):
    return any(a <= cp <= b for a, b in ranges)


def main():
    ranges = DEFAULT_RANGES
    hits = {}
    for path in sorted(glob.glob('main/*.c')):
        with open(path, encoding='utf-8') as f:
            lines = f.read().split('\n')
        for lineno, line in enumerate(lines, 1):
            for lit in re.findall(r'"((?:[^"\\]|\\.)*)"', line):
                for ch in lit:
                    if ord(ch) < 0x80:
                        continue
                    if not in_font(ord(ch), ranges):
                        hits.setdefault(ch, []).append('%s:%d' % (path, lineno))

    out = []
    if not hits:
        out.append('全部字符都在字库内')
    for ch, loc in sorted(hits.items(), key=lambda kv: -len(kv[1])):
        out.append('U+%04X  次数=%d  %s' % (ord(ch), len(loc), ' '.join(loc[:5])))
    text = '\n'.join(out)
    print(text)
    with open('missing_glyphs.txt', 'w', encoding='utf-8') as f:
        f.write(text + '\n')
    return 0 if not hits else 1


if __name__ == '__main__':
    sys.exit(main())
