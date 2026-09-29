#pragma once

#include "lvgl.h"

/* 文件管理 App：浏览 SD 卡（SDMMC + FATFS）上的目录、查看文本文件、删除文件。
 *
 * 用法（与 ui.c 的懒加载机制配套）：
 *   1) ui.c 首次打开 App 时：make_app_screen("文件管理") + build_file_manager_screen(scr)
 *   2) 立刻调用 file_mgr_on_open()，请求挂载 SD 卡并列出根目录
 *   3) 主循环里每圈调用 file_mgr_periodic()，真正执行挂载/列目录/读文件/删除
 *      （这些操作会阻塞，不能放进 LVGL 输入事件回调里做，否则卡看门狗）
 */

/* 单屏最多显示的条目数：受 LVGL 对象数量限制（每行 3 个对象），
 * 超过 FM_MAX_ROWS 的目录只显示前 FM_MAX_ROWS 项，状态栏会提示。 */
#define FILE_MGR_MAX_ROWS 32

/* 构建界面（只调用一次，屏幕对象由 ui.c 的 make_app_screen 创建） */
void build_file_manager_screen(lv_obj_t *scr);

/* 在当前目录里找上一个/下一个「同类」媒体文件（图片↔图片，视频↔视频）。
 * cur: 当前文件完整路径；dir: -1 上一个 / +1 下一个；out: 结果路径。
 * 用于图片查看器的「上一张 / 下一张」。 */
bool file_mgr_media_neighbor(const char *cur, int dir, char *out, size_t out_n);

/* 进入 App：请求挂载 + 列根目录 */
void file_mgr_on_open(void);

/* 主循环调用：执行延迟的挂载/列目录/预览/删除，并刷新界面 */
void file_mgr_periodic(void);
