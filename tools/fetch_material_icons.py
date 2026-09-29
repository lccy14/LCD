#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
从 Google 官方 material-design-icons 仓库下载 Material Icons 的 PNG 原图。

授权: Apache License 2.0 (Google)，可商用、可再分发。
仓库: https://github.com/google/material-design-icons

用法:
    python tools/fetch_material_icons.py [输出目录]

下载的图标为「透明背景 + 黑色字形」的标准 Material PNG，
之后由 tools/img2icons_argb8888.py 转成 LVGL 可用的带透明通道数据。
"""
import json
import os
import sys
import urllib.request

REPO = "google/material-design-icons"
REF = "3.0.1"          # 该版本目录里仍保留各种密度的 PNG 原图
OUT_DIR = sys.argv[1] if len(sys.argv) > 1 else os.path.join("assets", "icons")

# 界面需要的图标: (本地文件名, Material 图标名)
WANTED = [
    ("wifi",     "signal_wifi_4_bar"),
    ("settings", "settings"),
    ("clock",    "access_time"),
    ("music",    "music_note"),
    ("game",     "videogame_asset"),
    ("weather",  "wb_sunny"),
    ("novel",    "book"),
    ("pcmon",    "desktop_windows"),
    ("files",    "folder"),      # 文件管理 App
]


def get_tree():
    """拉取仓库完整文件树(GitHub trees API)"""
    url = "https://api.github.com/repos/%s/git/trees/%s?recursive=1" % (REPO, REF)
    req = urllib.request.Request(url, headers={"User-Agent": "python-urllib"})
    with urllib.request.urlopen(req, timeout=60) as resp:
        data = json.load(resp)
    if "tree" not in data:
        raise RuntimeError("GitHub Trees API 返回异常: %s" % str(data)[:300])
    return data["tree"]


def pick_png(tree, icon_name):
    """挑一张分辨率最合适的 PNG: 优先 2x_web(96px)，其次 drawable-xxxhdpi"""
    cands = []
    for item in tree:
        if item.get("type") != "blob":
            continue
        path = item["path"]
        if not path.endswith(".png"):
            continue
        base = os.path.basename(path)
        # 精确匹配 ic_<name>_black_48dp.png，避免误命中相似名
        if base != "ic_%s_black_48dp.png" % icon_name:
            continue
        if "2x_web" in path:
            cands.append((100, path))
        elif "3x_web" in path:
            cands.append((90, path))
        elif "drawable-xxxhdpi" in path:
            cands.append((80, path))
        elif "drawable-xxhdpi" in path:
            cands.append((70, path))
        elif "1x_web" in path:
            cands.append((60, path))
    if not cands:
        return None
    cands.sort(key=lambda t: -t[0])
    return cands[0][1]


def download(path, dest):
    url = "https://raw.githubusercontent.com/%s/%s/%s" % (REPO, REF, path.replace("\\", "/"))
    req = urllib.request.Request(url, headers={"User-Agent": "python-urllib"})
    with urllib.request.urlopen(req, timeout=60) as resp:
        data = resp.read()
    with open(dest, "wb") as f:
        f.write(data)
    return len(data)


def main():
    os.makedirs(OUT_DIR, exist_ok=True)
    print("拉取 %s@%s 文件树 ..." % (REPO, REF))
    tree = get_tree()
    print("共 %d 个条目" % len(tree))

    ok, failed = 0, []
    for local_name, icon_name in WANTED:
        path = pick_png(tree, icon_name)
        if not path:
            print("  [缺失] %-8s (Material 名: %s)" % (local_name, icon_name))
            failed.append(local_name)
            continue
        dest = os.path.join(OUT_DIR, "%s.png" % local_name)
        try:
            size = download(path, dest)
            print("  [OK]   %-8s <- %s  (%d bytes)" % (local_name, path, size))
            ok += 1
        except Exception as e:
            print("  [失败] %-8s %s : %s" % (local_name, path, e))
            failed.append(local_name)

    print("\n完成: 成功 %d / 共 %d" % (ok, len(WANTED)))
    if failed:
        print("未获取到:", ", ".join(failed))
    print("输出目录:", os.path.abspath(OUT_DIR))


if __name__ == "__main__":
    main()
