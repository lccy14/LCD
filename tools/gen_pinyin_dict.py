"""从 pypinyin 的 pinyin_dict.json 生成 LVGL lv_ime_pinyin 用的简体拼音字典。

生成的字典按拼音首字母 a-z 排序（lv_ime_pinyin 的 init_pinyin_dict 依赖这个顺序），
每个拼音一个条目，候选字是该拼音下的所有汉字，末尾带 {NULL,NULL} 哨兵。

用法: python tools/gen_pinyin_dict.py
输出: main/lv_pinyin_dict_cn.c / main/lv_pinyin_dict_cn.h
"""
import json
import os
import re
import pypinyin

OUT_C = 'main/lv_pinyin_dict_cn.c'
OUT_H = 'main/lv_pinyin_dict_cn.h'

# 常用字优先表：这些字排在每个拼音候选的最前面，减少翻页
PRIORITY = list('的一是在不了有和人这中大为上个国我以要他时来用们生到作地于出就分对成会可主发年动同工也能下过子说产种面而方后多定行学法所民得经十三之进着等部度家电力里如水化高自二理起小物现实加量都两体制机当使点从业本去把性好应开它合还因由其些然前外天政四日那社义事平形相全表间样与关各重新线内数正心反你明看原又么利比或但质气第向道命此变条只没结解问意建月公无系军很情者最立代想已通并提直题党程展五果料象员革位入常文总次品活设及管特件长求老头基资边流路级少图山统接知较将组见计别她手角期根论运农指九区强答战里心');
# 去重保序
seen = set()
PRIORITY = [c for c in PRIORITY if not (c in seen or seen.add(c))]


def strip_tone(py):
    """把带声调拼音转成 lv_ime_pinyin 识别的无声调小写拼音。ü->v。"""
    py = py.lower().strip()
    # 声调字符映射（pypinyin 的 dict 用带声调字符）
    trans = str.maketrans(
        'āáǎàēéěèīíǐìōóǒòūúǔùǖǘǚǜü',
        'aaaaeeeeiiiioooouuuuvvvvv')
    py = py.translate(trans)
    # 去掉非字母（声调数字等）
    py = re.sub(r'[^a-z]', '', py)
    return py


def main():
    d = os.path.dirname(pypinyin.__file__)
    data = json.load(open(os.path.join(d, 'pinyin_dict.json'), encoding='utf-8'))

    # py -> set(汉字)
    py2chars = {}
    for codepoint_str, pys in data.items():
        try:
            cp = int(codepoint_str)
        except ValueError:
            continue
        # 只收常用汉字区与扩展 A（lv_font_cn_16 覆盖 4E00-9FA5）
        if not (0x4E00 <= cp <= 0x9FA5):
            continue
        ch = chr(cp)
        for raw in pys.split(','):
            py = strip_tone(raw)
            if not py or not py[0].isalpha() or py[0] not in 'abcdefghijklmnopqrstuvwxyz':
                continue
            py2chars.setdefault(py, set()).add(ch)

    print('拼音数:', len(py2chars), ' 汉字总数:', len(set().union(*py2chars.values())))

    # 每个拼音的候选字排序：优先表在前，其余按码点（近似常用度：GB2312 区位）
    def sort_key(ch):
        if ch in PRIORITY:
            return (0, PRIORITY.index(ch))
        return (1, ord(ch))

    entries = []
    for py in sorted(py2chars.keys()):
        chars = sorted(py2chars[py], key=sort_key)
        # 不设上限：pypinyin 覆盖 2 万字，全部收录（Flash 充足）
        entries.append((py, ''.join(chars)))

    # 生成 C 文件
    lines = [
        '/* 自动生成，勿手改。源: pypinyin pinyin_dict.json',
        ' * 生成命令: python tools/gen_pinyin_dict.py',
        ' * 按拼音首字母 a-z 排序（lv_ime_pinyin 索引要求） */',
        '#include "lv_pinyin_dict_cn.h"',
        '',
        'const lv_pinyin_dict_t lv_pinyin_dict_cn[] = {',
    ]
    for py, chars in entries:
        # C 字符串里的汉字直接 UTF-8 输出
        lines.append('    {"%s", "%s"},' % (py, chars))
    lines += ['    {NULL, NULL}', '};', '']

    hlines = [
        '#pragma once',
        '#ifdef __cplusplus',
        'extern "C" {',
        '#endif',
        '',
        'typedef struct {',
        '    const char * const py;',
        '    const char * const py_mb;',
        '} lv_pinyin_dict_t;',
        '',
        'extern const lv_pinyin_dict_t lv_pinyin_dict_cn[];',
        '',
        '#ifdef __cplusplus',
        '}',
        '#endif',
        '',
    ]

    with open(OUT_C, 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(lines))
    with open(OUT_H, 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(hlines))

    print('生成', OUT_C, '条目', len(entries), ' 字数', sum(len(c) for _, c in entries))
    print('大小约 %.1f KB' % (os.path.getsize(OUT_C) / 1024))

    # 自检：覆盖用户之前缺失的字
    missing = [c for c in '这说聊谢吗饭苹圳' if not any(c in cs for _, cs in entries)]
    print('之前缺失字仍缺失:', missing if missing else '无')


if __name__ == '__main__':
    main()
