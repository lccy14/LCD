#include "file_mgr.h"
#include "sd_card.h"

#include <ctype.h>
#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_attr.h"     /* EXT_RAM_BSS_ATTR：把大数组放到 PSRAM，省内部 DRAM */
#include "esp_log.h"
#include "media.h"        /* 图片/视频：判断扩展名，决定「打开」按钮干什么 */

/* 界面字体：LVGL 内置 montserrat 不含汉字，中文一律用 lv_font_cn_16 */
LV_FONT_DECLARE(lv_font_cn_16);

static const char *TAG = "FILEMGR";

#define FM_NAME_LEN   128         /* 文件名缓存（长文件名也够用，显示时再省略） */
#define FM_PATH_LEN   200
#define FM_TEXT_MAX   4096          /* 文本预览最多读取的字节数 */

/* 布局：状态栏 22px + App 标题栏 40px，内容区从 y=62 开始 */
#define FM_PATH_Y     64
#define FM_LIST_Y     96
#define FM_LIST_H     116
#define FM_STATUS_Y   216

/* 一行里的固定宽度：文件名 | 大小。
 * 文件名必须给定宽度，LV_LABEL_LONG_DOT 才会把超出的部分变成省略号；
 * 否则标签宽度是 auto，文字会换行，两行文字在固定行高里就会互相重叠。 */
#define FM_SIZE_LABEL_W  62

typedef struct {
    char     name[FM_NAME_LEN];
    uint32_t size;
    bool     is_dir;
} fm_entry_t;

/* 延迟命令：挂载/列目录/预览/删除都会阻塞，统一放到 file_mgr_periodic() 里做 */
typedef enum {
    FM_CMD_NONE = 0,
    FM_CMD_MOUNT,     /* 挂载 SD 卡 */
    FM_CMD_LIST,      /* 列出 s_cmd_path 目录 */
    FM_CMD_VIEW,      /* 预览 s_cmd_path 文本文件 */
    FM_CMD_IMAGE,     /* 用图片查看器打开 s_cmd_path */
    FM_CMD_VIDEO,     /* 播放 s_cmd_path（MJPEG/AVI） */
    FM_CMD_DELETE     /* 删除 s_cmd_path */
} fm_cmd_t;

/* 条目表和文本预览缓冲较大（合计约 9KB），放 PSRAM 省出内部 DRAM
 * ——内部 RAM 还要留给 SDMMC 的 DMA 缓冲和 LVGL 的 draw buf。 */
EXT_RAM_BSS_ATTR static fm_entry_t s_entries[FILE_MGR_MAX_ROWS];
EXT_RAM_BSS_ATTR static char       s_text[FM_TEXT_MAX + 1];

static int        s_cnt    = 0;      /* 当前显示出来的条目数 */
static int        s_total  = 0;      /* 目录里实际的条目数（可能 > 显示上限） */
static char       s_cwd[FM_PATH_LEN] = SD_MOUNT_POINT;
static fm_cmd_t   s_cmd = FM_CMD_NONE;
static char       s_cmd_path[FM_PATH_LEN];
static int        s_sel = -1;        /* 详情页对应的条目下标 */
static char       s_flash[32];       /* 一次性提示，显示一次后清空 */

/* ---------------------------------- 界面对象 ---------------------------------- */
static lv_obj_t *s_path_label  = NULL;
static lv_obj_t *s_status_label = NULL;
static lv_obj_t *s_list        = NULL;
static lv_obj_t *s_row[FILE_MGR_MAX_ROWS];
static lv_obj_t *s_row_name[FILE_MGR_MAX_ROWS];
static lv_obj_t *s_row_size[FILE_MGR_MAX_ROWS];
static lv_obj_t *s_detail      = NULL;
static lv_obj_t *s_d_name      = NULL;
static lv_obj_t *s_d_info      = NULL;
static lv_obj_t *s_d_open      = NULL;
static lv_obj_t *s_d_open_lbl  = NULL;   /* 「打开」按钮的文字（看图/播放/打开） */
static lv_obj_t *s_view        = NULL;
static lv_obj_t *s_view_label  = NULL;
static lv_obj_t *s_view_body   = NULL;
static int       s_name_w      = 222;    /* 文件名标签宽度(px)，建屏时按屏宽算 */

/* ------------------------------ 小工具（路径/大小） ------------------------------ */

static void path_join(char *dst, size_t n, const char *dir, const char *name)
{
    size_t len = strlen(dir);
    if (len > 0 && dir[len - 1] == '/') {
        snprintf(dst, n, "%s%s", dir, name);
    } else {
        snprintf(dst, n, "%s/%s", dir, name);
    }
}

/* 回到上一级，但不能退到挂载点之外 */
static void path_parent(char *path)
{
    if (strcmp(path, SD_MOUNT_POINT) == 0) {
        return;
    }
    char *p = strrchr(path, '/');
    if (!p || p == path) {
        snprintf(path, FM_PATH_LEN, "%s", SD_MOUNT_POINT);
        return;
    }
    *p = '\0';
    if (strncmp(path, SD_MOUNT_POINT, strlen(SD_MOUNT_POINT)) != 0) {
        snprintf(path, FM_PATH_LEN, "%s", SD_MOUNT_POINT);
    }
}

/* ---------------- 长文件名省略（UTF-8 安全，保留尾部扩展名） ----------------
 * 16px 字号：ASCII 约 8px，中文/全角约 16px。名字太长时显示成
 * "前半~后半"（如 2026-12-31-CH1~001.csv），保证单行不换行、不重叠。
 *
 * 省略号用 ASCII 的 '~' 而不是 U+2026 "…"：当前 lv_font_cn_16 的生成范围是
 * 32-126,176,183,12288-12351,65281-65374,19968-40869，不含 0x2010-0x2027，
 * "…" 会显示成方框。（想用真省略号就把字库补上这段重生成，见 tools/cn_full_ranges.txt） */

static int utf8_cp_len(const char *p)
{
    unsigned char c = (unsigned char)p[0];
    if (c < 0x80)           return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

static int cp_px(const char *p)
{
    return ((unsigned char)p[0] < 0x80) ? 8 : 16;   /* ASCII 半宽 / 中日韩全角 */
}

/* 按字节上限拷贝名字，但不把 UTF-8 多字节字符从中间切断
 * （切一半的话末尾那个半截字符 LVGL 找不到字形，又会是一个方框） */
static void name_copy(char *dst, size_t dstsz, const char *src)
{
    size_t n = strlen(src);
    if (n >= dstsz) {
        n = dstsz - 1;
    }
    while (n > 0 && ((unsigned char)src[n] & 0xC0) == 0x80) {
        n--;                                  /* 回退到字符首字节 */
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* 把 in 省略到估算宽度 max_px 以内，结果写入 out */
static void elide_name(const char *in, char *out, size_t outsz, int max_px)
{
    size_t len = strlen(in);
    int    w = 0;
    size_t i = 0;
    while (i < len) {
        int l = utf8_cp_len(in + i);
        w += cp_px(in + i);
        i += (size_t)l;
    }
    if (w <= max_px || outsz < 8) {
        snprintf(out, outsz, "%s", in);
        return;
    }

    const int ellipsis_px = 8;     /* '~'，字库里有（"…" U+2026 没有，会显示方框） */
    const int tail_px     = 56;    /* 尾部保留约 7 个字母 / 3 个汉字 */

    /* 头部：从头取到预算用完 */
    int budget = max_px - ellipsis_px - tail_px;
    if (budget < 24) budget = 24;
    size_t h = 0;
    int    hw = 0;
    while (h < len && hw + cp_px(in + h) <= budget) {
        int l = utf8_cp_len(in + h);
        hw += cp_px(in + h);
        h  += (size_t)l;
    }

    /* 尾部：从末尾往回取到 tail_px（不越过头部） */
    size_t t = len;
    int    tw = 0;
    while (t > h) {
        size_t back = t - 1;
        while (back > h && ((unsigned char)in[back] & 0xC0) == 0x80) {
            back--;                       /* 回退到字符首字节 */
        }
        int lw = cp_px(in + back);
        if (tw + lw > tail_px) {
            break;
        }
        tw += lw;
        t = back;
    }

    snprintf(out, outsz, "%.*s~%s", (int)h, in, in + t);
}

/* 整数运算，避免使用 %f（newlib nano 下浮点 printf 可能不可用） */
static void fmt_size(char *out, size_t n, uint64_t bytes)
{
    const uint64_t KB = 1024;
    const uint64_t MB = 1024 * 1024;
    const uint64_t GB = 1024 * 1024 * 1024;
    if (bytes < KB) {
        snprintf(out, n, "%uB", (unsigned)bytes);
    } else if (bytes < MB) {
        snprintf(out, n, "%u.%uK", (unsigned)(bytes / KB), (unsigned)((bytes % KB) * 10 / KB));
    } else if (bytes < GB) {
        snprintf(out, n, "%u.%uM", (unsigned)(bytes / MB), (unsigned)((bytes % MB) * 10 / MB));
    } else {
        snprintf(out, n, "%u.%uG", (unsigned)(bytes / GB), (unsigned)((bytes % GB) * 10 / GB));
    }
}

static bool is_text_file(const char *name)
{
    static const char *exts[] = {
        ".txt", ".log", ".md", ".csv", ".json", ".xml", ".ini", ".cfg", ".conf",
        ".c", ".h", ".cpp", ".hpp", ".py", ".js", ".ts", ".html", ".css",
        ".sh", ".yml", ".yaml", ".lua", ".bat", ".sql"
    };
    const char *dot = strrchr(name, '.');
    if (!dot) {
        return false;
    }
    char low[16];
    size_t i = 0;
    for (; dot[i] != '\0' && i < sizeof(low) - 1; i++) {
        low[i] = (char)tolower((unsigned char)dot[i]);
    }
    low[i] = '\0';
    for (size_t k = 0; k < sizeof(exts) / sizeof(exts[0]); k++) {
        if (strcmp(low, exts[k]) == 0) {
            return true;
        }
    }
    return false;
}

/* 往 buf 末尾追加格式化文本，返回新的长度（已夹住不超过缓冲大小） */
static int buf_append(char *buf, size_t n, int pos, const char *fmt, ...)
{
    if (pos < 0) {
        pos = 0;
    }
    if ((size_t)pos >= n) {
        return (int)n - 1;
    }
    va_list ap;
    va_start(ap, fmt);
    int w = vsnprintf(buf + pos, n - (size_t)pos, fmt, ap);
    va_end(ap);
    if (w < 0) {
        return pos;
    }
    pos += w;
    if ((size_t)pos >= n) {
        pos = (int)n - 1;
    }
    return pos;
}

/* 文件夹排前面，其余按名称不区分大小写排序 */
static bool entry_less(const fm_entry_t *a, const fm_entry_t *b)
{
    if (a->is_dir != b->is_dir) {
        return a->is_dir;
    }
    return strcasecmp(a->name, b->name) < 0;
}

/* ------------------------------ 目录扫描与界面刷新 ------------------------------ */

static void fm_update_ui(void)
{
    if (!s_list || !s_path_label || !s_status_label) {
        return;                 /* 界面还没建好 */
    }

    /* 路径 */
    lv_label_set_text(s_path_label, s_cwd);

    /* 列表行：原地更新固定行池，不反复建对象 */
    for (int i = 0; i < FILE_MGR_MAX_ROWS; i++) {
        if (i < s_cnt) {
            char disp[FM_NAME_LEN + 8];
            elide_name(s_entries[i].name, disp, sizeof(disp), s_name_w);
            lv_label_set_text(s_row_name[i], disp);
            if (s_entries[i].is_dir) {
                lv_label_set_text(s_row_size[i], "文件夹");
                lv_obj_set_style_text_color(s_row_name[i], lv_color_hex(0xFFC46B), 0);
            } else {
                char sz[16];
                fmt_size(sz, sizeof(sz), s_entries[i].size);
                lv_label_set_text(s_row_size[i], sz);
                lv_obj_set_style_text_color(s_row_name[i], lv_color_white(), 0);
            }
            lv_obj_clear_flag(s_row[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_row[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    lv_obj_scroll_to_y(s_list, 0, LV_ANIM_OFF);

    /* 状态行：条目数 + SD 卡剩余空间（+ 一次性提示） */
    char buf[72];
    char tsz[16], fsz[16];
    uint64_t total = 0, freeb = 0;
    int pos = 0;
    pos = buf_append(buf, sizeof(buf), pos, "%d项", s_total);
    if (s_total > s_cnt) {
        pos = buf_append(buf, sizeof(buf), pos, "(前%d)", s_cnt);
    }
    if (sd_card_get_space(&total, &freeb)) {
        fmt_size(tsz, sizeof(tsz), total);
        fmt_size(fsz, sizeof(fsz), freeb);
        pos = buf_append(buf, sizeof(buf), pos, " 剩%s/%s", fsz, tsz);
    }
    if (s_flash[0] != '\0') {
        buf_append(buf, sizeof(buf), pos, " %s", s_flash);
        s_flash[0] = '\0';
    }
    lv_label_set_text(s_status_label, buf);
}

static void fm_list_dir(const char *path)
{
    s_cnt   = 0;
    s_total = 0;

    DIR *d = opendir(path);
    if (!d) {
        ESP_LOGW(TAG, "打开目录失败: %s", path);
        snprintf(s_flash, sizeof(s_flash), "打开失败");
        fm_update_ui();
        return;
    }

    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
            continue;
        }
        s_total++;
        if (s_cnt >= FILE_MGR_MAX_ROWS) {
            continue;             /* 超出显示上限的只计数，不存 */
        }
        fm_entry_t *it = &s_entries[s_cnt];
        /* d_name 最长 255（长文件名），截断到 FM_NAME_LEN 且保证不切断 UTF-8 字符 */
        name_copy(it->name, sizeof(it->name), e->d_name);

        char full[FM_PATH_LEN];
        path_join(full, sizeof(full), path, e->d_name);
        struct stat st;
        if (stat(full, &st) == 0) {
            it->is_dir = S_ISDIR(st.st_mode);
            it->size   = it->is_dir ? 0 : (uint32_t)st.st_size;
        } else {
            it->is_dir = false;
            it->size   = 0;
        }
        s_cnt++;
    }
    closedir(d);

    /* 插入排序：文件夹优先，其次按名称 */
    for (int i = 1; i < s_cnt; i++) {
        fm_entry_t tmp = s_entries[i];
        int j = i - 1;
        while (j >= 0 && entry_less(&tmp, &s_entries[j])) {
            s_entries[j + 1] = s_entries[j];
            j--;
        }
        s_entries[j + 1] = tmp;
    }

    ESP_LOGI(TAG, "列出 %s: %d 项（显示 %d）", path, s_total, s_cnt);
    fm_update_ui();
}

static void fm_view_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(s_flash, sizeof(s_flash), "打开失败");
        fm_update_ui();
        return;
    }
    size_t n = fread(s_text, 1, FM_TEXT_MAX, f);
    fclose(f);
    s_text[n] = '\0';

    const char *msg;
    if (memchr(s_text, '\0', n) != NULL) {
        msg = "二进制文件，无法预览";
    } else {
        /* 去掉 \r，避免 Windows 换行的 ^M */
        size_t w = 0;
        for (size_t r = 0; r < n; r++) {
            if (s_text[r] != '\r') {
                s_text[w++] = s_text[r];
            }
        }
        s_text[w] = '\0';
        msg = s_text;
    }

    lv_label_set_text(s_view_label, msg);
    lv_obj_scroll_to_y(s_view_body, 0, LV_ANIM_OFF);
    lv_obj_clear_flag(s_view, LV_OBJ_FLAG_HIDDEN);
}

static void fm_delete(const char *path)
{
    struct stat st;
    int r;
    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
        r = rmdir(path);
    } else {
        r = unlink(path);
    }
    ESP_LOGW(TAG, "删除 %s -> %d", path, r);
    snprintf(s_flash, sizeof(s_flash), r == 0 ? "已删除" : "删除失败");

    lv_obj_add_flag(s_detail, LV_OBJ_FLAG_HIDDEN);
    s_sel = -1;
    snprintf(s_cmd_path, sizeof(s_cmd_path), "%s", s_cwd);
    s_cmd = FM_CMD_LIST;          /* 删完自动刷新，下一圈执行 */
}

/* ---------------------------------- 事件回调 ---------------------------------- */

static void up_btn_cb(lv_event_t *e)
{
    (void)e;
    path_parent(s_cwd);
    snprintf(s_cmd_path, sizeof(s_cmd_path), "%s", s_cwd);
    s_cmd = FM_CMD_LIST;
}

static void refresh_btn_cb(lv_event_t *e)
{
    (void)e;
    if (!sd_card_is_mounted()) {
        s_cmd = FM_CMD_MOUNT;
    } else {
        snprintf(s_cmd_path, sizeof(s_cmd_path), "%s", s_cwd);
        s_cmd = FM_CMD_LIST;
    }
}

static void row_click_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= s_cnt) {
        return;
    }
    if (s_entries[i].is_dir) {
        /* 进入子目录：只置命令，真正的列目录在 file_mgr_periodic() 里做 */
        char sub[FM_PATH_LEN];
        path_join(sub, sizeof(sub), s_cwd, s_entries[i].name);
        snprintf(s_cmd_path, sizeof(s_cmd_path), "%s", sub);
        s_cmd = FM_CMD_LIST;
        return;
    }

    /* 文件：弹出详情页 */
    s_sel = i;
    const char *nm = s_entries[i].name;
    char sz[16];
    fmt_size(sz, sizeof(sz), s_entries[i].size);
    /* 详情页名字最多占两行：先按「两行宽度」中间省略，再交给 WRAP 折行，
     * 这样最多两行，不会顶到下面的信息行 */
    char disp[FM_NAME_LEN + 8];
    elide_name(nm, disp, sizeof(disp), lv_obj_get_width(s_d_name) * 2);
    lv_label_set_text(s_d_name, disp);

    /* 「打开」按钮：文本=预览，图片=看图，视频=播放，其它类型不支持 */
    const char *open_txt = NULL;
    if (media_is_image(nm))      open_txt = "看图";
    else if (media_is_video(nm)) open_txt = "播放";
    else if (is_text_file(nm))   open_txt = "打开";

    lv_label_set_text_fmt(s_d_info, "大小 %s\n%s", sz,
                          open_txt ? (media_is_image(nm) ? "图片，可查看" :
                                      media_is_video(nm) ? "MJPEG 视频，可播放" : "文本，可预览")
                                   : "暂不支持预览");
    if (open_txt) {
        lv_label_set_text(s_d_open_lbl, open_txt);
        lv_obj_clear_flag(s_d_open, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_d_open, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_clear_flag(s_detail, LV_OBJ_FLAG_HIDDEN);
}

static void detail_back_cb(lv_event_t *e)
{
    (void)e;
    lv_obj_add_flag(s_detail, LV_OBJ_FLAG_HIDDEN);
    s_sel = -1;
}

static void detail_open_cb(lv_event_t *e)
{
    (void)e;
    if (s_sel < 0 || s_sel >= s_cnt) {
        return;
    }
    char full[FM_PATH_LEN];
    path_join(full, sizeof(full), s_cwd, s_entries[s_sel].name);
    snprintf(s_cmd_path, sizeof(s_cmd_path), "%s", full);
    /* 按类型分发：文本→预览，图片→看图，视频→播放 */
    if (media_is_image(s_entries[s_sel].name))      s_cmd = FM_CMD_IMAGE;
    else if (media_is_video(s_entries[s_sel].name)) s_cmd = FM_CMD_VIDEO;
    else                                            s_cmd = FM_CMD_VIEW;
}

static void detail_del_cb(lv_event_t *e)
{
    (void)e;
    if (s_sel < 0 || s_sel >= s_cnt) {
        return;
    }
    char full[FM_PATH_LEN];
    path_join(full, sizeof(full), s_cwd, s_entries[s_sel].name);
    snprintf(s_cmd_path, sizeof(s_cmd_path), "%s", full);
    s_cmd = FM_CMD_DELETE;
}

static void view_back_cb(lv_event_t *e)
{
    (void)e;
    lv_obj_add_flag(s_view, LV_OBJ_FLAG_HIDDEN);
}

/* 通用小按钮 */
static lv_obj_t *make_small_btn(lv_obj_t *parent, const char *text, int x, int y, int w,
                                lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, w, 28);
    lv_obj_set_pos(btn, x, y);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, &lv_font_cn_16, 0);
    lv_obj_center(lbl);
    if (cb) {
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    }
    return btn;
}

/* 详情页 / 预览页共用的全屏浮层 */
static lv_obj_t *make_overlay(lv_obj_t *scr, const char *title, lv_event_cb_t back_cb)
{
    lv_obj_t *ov = lv_obj_create(scr);
    lv_obj_set_size(ov, lv_display_get_horizontal_resolution(NULL),
                    lv_display_get_vertical_resolution(NULL));
    lv_obj_align(ov, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(ov, lv_color_hex(0x101018), 0);
    lv_obj_set_style_bg_opa(ov, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(ov, 0, 0);
    lv_obj_set_style_border_opa(ov, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(ov, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(ov, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(ov, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *bar = lv_obj_create(ov);
    lv_obj_set_size(bar, lv_display_get_horizontal_resolution(NULL), 40);
    lv_obj_align(bar, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x16161F), 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_border_opa(bar, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(bar, 8, 0);
    lv_obj_set_style_pad_column(bar, 8, 0);

    lv_obj_t *bk = lv_button_create(bar);
    lv_obj_set_size(bk, 80, 32);
    lv_obj_t *bkl = lv_label_create(bk);
    lv_label_set_text(bkl, "< 返回");
    lv_obj_set_style_text_font(bkl, &lv_font_cn_16, 0);
    lv_obj_center(bkl);
    lv_obj_add_event_cb(bk, back_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *t = lv_label_create(bar);
    lv_label_set_text(t, title);
    lv_obj_set_style_text_font(t, &lv_font_cn_16, 0);
    lv_obj_set_style_text_color(t, lv_color_white(), 0);
    lv_obj_set_style_pad_left(t, 10, 0);
    return ov;
}

/* ---------------------------------- 对外接口 ---------------------------------- */

void build_file_manager_screen(lv_obj_t *scr)
{
    const int scr_w = (int)lv_display_get_horizontal_resolution(NULL);

    /* 文件名可用宽度 = 列表宽 - 行左右内边距 - 大小列 - 列间距 */
    s_name_w = scr_w - 8 - 20 - FM_SIZE_LABEL_W - 8;
    if (s_name_w < 80) {
        s_name_w = 80;
    }

    /* 当前路径（过长自动省略号） */
    s_path_label = lv_label_create(scr);
    lv_label_set_text(s_path_label, s_cwd);
    lv_obj_set_style_text_font(s_path_label, &lv_font_cn_16, 0);
    lv_obj_set_style_text_color(s_path_label, lv_color_hex(0x9AA0B5), 0);
    lv_obj_set_pos(s_path_label, 8, FM_PATH_Y + 4);
    lv_obj_set_width(s_path_label, 190);
    lv_label_set_long_mode(s_path_label, LV_LABEL_LONG_DOT);

    make_small_btn(scr, "上级", scr_w - 106, FM_PATH_Y, 46, up_btn_cb);
    make_small_btn(scr, "刷新", scr_w - 54, FM_PATH_Y, 46, refresh_btn_cb);

    /* 文件列表（可滚动，行池固定 32 行） */
    s_list = lv_obj_create(scr);
    lv_obj_set_size(s_list, scr_w - 8, FM_LIST_H);
    lv_obj_set_pos(s_list, 4, FM_LIST_Y);
    lv_obj_set_style_bg_opa(s_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_opa(s_list, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_list, 4, 0);
    lv_obj_set_style_pad_top(s_list, 2, 0);
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_OFF);

    for (int i = 0; i < FILE_MGR_MAX_ROWS; i++) {
        lv_obj_t *row = lv_obj_create(s_list);
        lv_obj_set_size(row, lv_pct(100), 34);
        lv_obj_set_style_bg_color(row, lv_color_hex(0x23232F), 0);
        lv_obj_set_style_radius(row, 8, 0);
        lv_obj_set_style_border_opa(row, LV_OPA_TRANSP, 0);
        /* 行本身不可滚动，否则拖动会被行吃掉，列表滚不动 */
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scrollbar_mode(row, LV_SCROLLBAR_MODE_OFF);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_hor(row, 10, 0);
        lv_obj_set_style_pad_column(row, 8, 0);

        /* 文件名：宽度固定 => 超长自动变省略号，绝不会换成两行 */
        lv_obj_t *nm = lv_label_create(row);
        lv_label_set_text(nm, "");
        lv_obj_set_style_text_font(nm, &lv_font_cn_16, 0);
        lv_obj_set_style_text_color(nm, lv_color_white(), 0);
        lv_obj_set_width(nm, s_name_w);
        lv_label_set_long_mode(nm, LV_LABEL_LONG_DOT);
        lv_obj_clear_flag(nm, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(nm, LV_OBJ_FLAG_CLICKABLE);

        /* 大小 / 文件夹：右对齐，宽度固定，避免把名字挤到换行 */
        lv_obj_t *sz = lv_label_create(row);
        lv_label_set_text(sz, "");
        lv_obj_set_style_text_font(sz, &lv_font_cn_16, 0);
        lv_obj_set_style_text_color(sz, lv_color_hex(0x9AA0B5), 0);
        lv_obj_set_width(sz, FM_SIZE_LABEL_W);
        lv_obj_set_style_text_align(sz, LV_TEXT_ALIGN_RIGHT, 0);
        lv_label_set_long_mode(sz, LV_LABEL_LONG_DOT);
        lv_obj_clear_flag(sz, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(sz, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_add_event_cb(row, row_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);

        s_row[i]      = row;
        s_row_name[i] = nm;
        s_row_size[i] = sz;
    }

    /* 底部状态行 */
    s_status_label = lv_label_create(scr);
    lv_label_set_text(s_status_label, "等待 SD 卡...");
    lv_obj_set_style_text_font(s_status_label, &lv_font_cn_16, 0);
    lv_obj_set_style_text_color(s_status_label, lv_color_hex(0x9AA0B5), 0);
    lv_obj_set_pos(s_status_label, 8, FM_STATUS_Y);
    lv_obj_set_width(s_status_label, scr_w - 16);
    lv_label_set_long_mode(s_status_label, LV_LABEL_LONG_DOT);

    /* ---------------- 文件详情浮层 ---------------- */
    s_detail = make_overlay(scr, "文件详情", detail_back_cb);

    s_d_name = lv_label_create(s_detail);
    lv_label_set_text(s_d_name, "");
    lv_obj_set_style_text_font(s_d_name, &lv_font_cn_16, 0);
    lv_obj_set_style_text_color(s_d_name, lv_color_white(), 0);
    lv_obj_set_pos(s_d_name, 12, 46);          /* 最多两行：46 ~ 82 */
    lv_obj_set_width(s_d_name, scr_w - 24);
    lv_label_set_long_mode(s_d_name, LV_LABEL_LONG_WRAP);

    s_d_info = lv_label_create(s_detail);
    lv_label_set_text(s_d_info, "");
    lv_obj_set_style_text_font(s_d_info, &lv_font_cn_16, 0);
    lv_obj_set_style_text_color(s_d_info, lv_color_hex(0x9AA0B5), 0);
    lv_obj_set_pos(s_d_info, 12, 96);          /* 让开名字那两行，避免重叠 */
    lv_obj_set_width(s_d_info, scr_w - 24);
    lv_label_set_long_mode(s_d_info, LV_LABEL_LONG_WRAP);

    s_d_open = lv_button_create(s_detail);
    lv_obj_set_size(s_d_open, 90, 38);
    lv_obj_set_pos(s_d_open, 16, 178);
    s_d_open_lbl = lv_label_create(s_d_open);
    lv_label_set_text(s_d_open_lbl, "打开");
    lv_obj_set_style_text_font(s_d_open_lbl, &lv_font_cn_16, 0);
    lv_obj_center(s_d_open_lbl);
    lv_obj_add_event_cb(s_d_open, detail_open_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *del_btn = lv_button_create(s_detail);
    lv_obj_set_size(del_btn, 90, 38);
    lv_obj_set_pos(del_btn, 115, 178);
    lv_obj_t *del_lbl = lv_label_create(del_btn);
    lv_label_set_text(del_lbl, "删除");
    lv_obj_set_style_text_font(del_lbl, &lv_font_cn_16, 0);
    lv_obj_center(del_lbl);
    lv_obj_add_event_cb(del_btn, detail_del_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *cls_btn = lv_button_create(s_detail);
    lv_obj_set_size(cls_btn, 90, 38);
    lv_obj_set_pos(cls_btn, 214, 178);
    lv_obj_t *cls_lbl = lv_label_create(cls_btn);
    lv_label_set_text(cls_lbl, "关闭");
    lv_obj_set_style_text_font(cls_lbl, &lv_font_cn_16, 0);
    lv_obj_center(cls_lbl);
    lv_obj_add_event_cb(cls_btn, detail_back_cb, LV_EVENT_CLICKED, NULL);

    /* ---------------- 文本预览浮层 ---------------- */
    s_view = make_overlay(scr, "文本预览", view_back_cb);

    s_view_body = lv_obj_create(s_view);
    lv_obj_set_size(s_view_body, scr_w - 8, 194);
    lv_obj_set_pos(s_view_body, 4, 42);
    lv_obj_set_style_bg_opa(s_view_body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_opa(s_view_body, LV_OPA_TRANSP, 0);
    lv_obj_set_scroll_dir(s_view_body, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_view_body, LV_SCROLLBAR_MODE_OFF);

    s_view_label = lv_label_create(s_view_body);
    lv_label_set_text(s_view_label, "");
    lv_obj_set_style_text_font(s_view_label, &lv_font_cn_16, 0);
    lv_obj_set_style_text_color(s_view_label, lv_color_white(), 0);
    lv_obj_set_width(s_view_label, scr_w - 20);
    lv_label_set_long_mode(s_view_label, LV_LABEL_LONG_WRAP);
}

bool file_mgr_media_neighbor(const char *cur, int dir, char *out, size_t out_n)
{
    /* 先定位当前文件在列表里的位置 */
    int cur_idx = -1;
    for (int i = 0; i < s_cnt; i++) {
        char full[FM_PATH_LEN];
        path_join(full, sizeof(full), s_cwd, s_entries[i].name);
        if (strcmp(full, cur) == 0) {
            cur_idx = i;
            break;
        }
    }
    if (cur_idx < 0) {
        return false;                      /* 不在当前目录里（例如从别处打开的） */
    }

    /* 只找同类型的：图片↔图片，视频↔视频 */
    bool want_img = media_is_image(s_entries[cur_idx].name);
    for (int i = cur_idx + dir; i >= 0 && i < s_cnt; i += dir) {
        bool is_img = media_is_image(s_entries[i].name);
        bool is_vid = media_is_video(s_entries[i].name);
        if (want_img ? !is_img : !is_vid) {
            continue;
        }
        char full[FM_PATH_LEN];
        path_join(full, sizeof(full), s_cwd, s_entries[i].name);
        snprintf(out, out_n, "%s", full);
        return true;
    }
    return false;
}

void file_mgr_on_open(void)
{
    if (!sd_card_is_mounted()) {
        s_cmd = FM_CMD_MOUNT;
    } else {
        snprintf(s_cmd_path, sizeof(s_cmd_path), "%s", s_cwd);
        s_cmd = FM_CMD_LIST;
    }
}

void file_mgr_periodic(void)
{
    if (s_cmd == FM_CMD_NONE) {
        return;
    }
    fm_cmd_t cmd = s_cmd;
    s_cmd = FM_CMD_NONE;

    switch (cmd) {
    case FM_CMD_MOUNT:
        if (s_status_label) {
            lv_label_set_text(s_status_label, "正在挂载 SD 卡...");
        }
        if (sd_card_mount(true) == ESP_OK) {
            snprintf(s_cwd, sizeof(s_cwd), "%s", SD_MOUNT_POINT);
            snprintf(s_cmd_path, sizeof(s_cmd_path), "%s", s_cwd);
            s_cmd = FM_CMD_LIST;              /* 挂上后立刻列根目录 */
        } else {
            snprintf(s_flash, sizeof(s_flash), "未挂载，点刷新重试");
            s_cnt = 0;
            s_total = 0;
            fm_update_ui();
        }
        break;
    case FM_CMD_LIST:
        snprintf(s_cwd, sizeof(s_cwd), "%s", s_cmd_path);
        fm_list_dir(s_cwd);
        break;
    case FM_CMD_VIEW:
        fm_view_file(s_cmd_path);
        break;
    case FM_CMD_IMAGE:
        lv_obj_add_flag(s_detail, LV_OBJ_FLAG_HIDDEN);   /* 进看图，详情页收起来 */
        media_image_open(s_cmd_path);
        break;
    case FM_CMD_VIDEO:
        lv_obj_add_flag(s_detail, LV_OBJ_FLAG_HIDDEN);
        media_video_play(s_cmd_path);
        break;
    case FM_CMD_DELETE:
        fm_delete(s_cmd_path);
        break;
    default:
        break;
    }
}
