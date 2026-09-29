#pragma once

#include <stdbool.h>
#include <stddef.h>

/* 图片查看 / MJPEG 视频播放（文件都在 SD 卡 /sdcard 上）
 *
 * 分工：
 *   图片(jpg/jpeg)   —— 自己用 tjpgd 流式解码（LVGL 会按原始分辨率整帧解码，大图必崩），
 *                        边解边缩放到 320x160 的 PSRAM RGB565 缓冲，再包成 lv_image_dsc_t
 *                        交给 LVGL 显示（与桌面壁纸同一条渲染路径）
 *   图片(png/bmp)    —— 交给 LVGL 的解码器(LODEPNG/BMP)，路径 "S:/dir/a.png"
 *                        （'S' 盘 = /sdcard，见 sdkconfig），太大则提示先转 JPG
 *   视频(avi, MJPEG) —— 不走 LVGL：AVI 按容器 chunk 拆帧，裸 .mjpeg/.mjpg 按
 *                        JPEG 的 FFD8/FFD9 标记切帧（帧率固定，无时间戳），
 *                        用 tjpgd 逐帧解码成 RGB565 直接写 g_framebuffer，再 LCD_Flush_All() 刷屏。
 *                        原因：LVGL 会缓存解码结果，逐帧换图会爆内存且很慢。
 *
 * 用法（与 ui.c 主循环配套）：
 *   media_image_open(path) / media_video_play(path)  -> 打开
 *   media_periodic() 在主循环里调用（视频真正解码刷屏在这里）
 *   media_stop()     退出并回到打开它的界面
 */

bool media_is_image(const char *name);
bool media_is_video(const char *name);

void media_image_open(const char *path);
void media_video_play(const char *path);

void media_stop(void);
bool media_is_active(void);

/* 主循环调用：视频解码 + 刷屏（图片查看无需周期性处理） */
void media_periodic(void);
