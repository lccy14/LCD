#pragma once

#include <stdbool.h>
#include <stddef.h>

/* 图片查看（文件都在 SD 卡 /sdcard 上）
 *
 * 分工：
 *   图片(jpg/jpeg)   —— 自己用 tjpgd 流式解码（LVGL 按原始分辨率整帧解码，大图必崩），
 *                        边解边缩放到 320x240 的 PSRAM RGB565 缓冲，再包成 lv_image_dsc_t
 *                        交给 LVGL 显示（与桌面壁纸同一条渲染路径）
 *   图片(png)       —— 交给 LVGL 的解码器(LODEPNG)，路径 "S:/dir/a.png"
 *                        （'S' 盘 = /sdcard，见 sdkconfig），太大则提示先转 JPG
 *
 * 用法（与 ui.c 主循环配套）：
 *   media_image_open(path)  -> 打开看图
 *   media_stop()            -> 退出并回到打开它的界面
 */

bool media_is_image(const char *name);

void media_image_open(const char *path);

void media_stop(void);
bool media_is_active(void);
