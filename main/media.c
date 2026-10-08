#include "media.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
/* 下面两个是 LVGL 内部头（lvgl.h 没导出），看图必须用到：
 *   lv_image_cache_drop()      —— 换图时清掉上一张的解码结果（整帧占 w*h*4）
 *   lv_image_decoder_dsc_t     —— 自己先解一次，失败能立刻提示，而不是留一片黑
 *                                 （dsc 结构体定义在私有头里，公开头只有前向声明） */
#include "src/misc/cache/instance/lv_image_cache.h"
#include "src/draw/lv_image_decoder_private.h"
/* 排查用：直接调 lodepng 拿解码错误码（LVGL 内部日志没开，失败原因看不到） */
#include "src/libs/lodepng/lodepng.h"

#include "file_mgr.h"
#include "lcd.h"          /* LCD_W / LCD_H / g_framebuffer / LCD_Flush_All */
#include "sd_card.h"      /* SD_MOUNT_POINT */
#include "ui.h"           /* ui_set_status_bar_visible */

/* 图片 JPG 解码用 LVGL 自带的 Tiny JPEG 解码器（编译进 LVGL，符号可链接） */
#if !LV_USE_TJPGD
#error "图片 JPG 解码需要 LVGL 的 TJPGD：请开启 CONFIG_LV_USE_TJPGD=y"
#endif
#include "libs/tjpgd/tjpgd.h"

/* 界面字体：中文一律用 lv_font_cn_16 */
LV_FONT_DECLARE(lv_font_cn_16);

static const char *TAG = "MEDIA";

/* tjpgd 工作区（TJpgDec 建议 4096） */
#define JPEG_POOL_SIZE   4096

/* ------------------------------ 文件名判断 ------------------------------ */

static bool ext_is(const char *name, const char *ext)
{
    const char *dot = strrchr(name, '.');
    if (!dot) return false;
    return strcasecmp(dot, ext) == 0;
}

bool media_is_image(const char *name)
{
    return ext_is(name, ".jpg") || ext_is(name, ".jpeg") ||
           ext_is(name, ".png");
}

static const char *base_name(const char *path)
{
    const char *p = strrchr(path, '/');
    return p ? p + 1 : path;
}

/* 把 /sdcard/dir/a.jpg 变成 LVGL 的 "S:/dir/a.jpg"（'S' 盘 = /sdcard） */
static void make_lvgl_path(char *out, size_t n, const char *full)
{
    const char *rel = full;
    size_t m = strlen(SD_MOUNT_POINT);
    if (strncmp(full, SD_MOUNT_POINT, m) == 0) {
        rel = full + m;
    }
    if (rel[0] == '\0') {
        rel = "/";
    }
    snprintf(out, n, "S:%s", rel);
}

/* ------------------------------ 全局状态 ------------------------------ */

typedef enum { MEDIA_IDLE = 0, MEDIA_IMAGE } media_mode_t;

static media_mode_t s_mode = MEDIA_IDLE;
static lv_obj_t    *s_return_scr = NULL;      /* 退出时回到哪个界面 */
static char         s_path[256];              /* 当前文件完整路径 */
static char         s_lvgl_path[264];         /* 给 LVGL 的路径（必须常驻，LVGL 只存指针） */

/* ---- 图片查看界面 ---- */
static lv_obj_t *s_img_scr  = NULL;
static lv_obj_t *s_img_view = NULL;
static lv_obj_t *s_img_name = NULL;
static lv_obj_t *s_img_msg  = NULL;   /* 解码失败时的居中提示（别再让人看一片黑） */

/* ---- 图片：解码后的 RGB565 缓冲（PSRAM）+ 给 LVGL 的描述符 ---- */
static uint8_t      *s_img_buf = NULL;
static lv_image_dsc_t s_img_dsc = {0};
static int           s_jpeg_w = 0, s_jpeg_h = 0;   /* 最近一次解出来的原图尺寸（日志用） */

/* ---- JPEG 解码状态 ----
 * 关键：tjpgd 是流式解码（MCU 一块一块出），所以不管原图多大，
 * 都不需要整帧缓冲——边解边按目标尺寸抽稀写进缓冲就行。 */
typedef struct {
    const uint8_t *mem;                       /* 内存数据源（图片用 .f，保留兼容） */
    size_t         size, pos;
    FILE          *f;                         /* 文件数据源（图片，可边读边解） */
} jpg_src_t;

typedef struct {
    uint8_t *dst;                             /* 目标缓冲（图片=PSRAM 的 RGB565 图） */
    int      dst_w, dst_h;                    /* 目标缓冲的宽高（像素） */
    int ox, oy, ow, oh;                       /* 输出区域（已居中、保持比例） */
    int src_w, src_h;
    const uint16_t *dx_map;                   /* 源列 -> 目标列（避免每像素做除法） */
} blit_ctx_t;

static blit_ctx_t s_blit;
static uint32_t   s_blit_rows = 0;            /* 解码计数，用于周期性让出 CPU */
static uint8_t   *s_pool = NULL;              /* tjpgd 工作区（内部 RAM，快） */

/* --------------------------- JPEG 解码 + 写缓冲 --------------------------- */

/* tjpgd 的数据源：FILE*（图片，边读边解，不占大缓冲）
 *
 * 关键：tjpgd 调用 jd->infunc(jd, NULL, len) 时是要求「跳过 len 字节」
 * （跳过 EXIF / APPn / 注释等它不认识的段，见 tjpgd.c: if(jd->infunc(jd, 0, len) != len) return JDR_INP;）。
 * 必须真的把文件指针往后移并返回 len，否则 tjpgd 认为流结束 → JDR_INP → 解码失败。
 * 手机/相机拍的 JPEG 几乎都带 EXIF，不做这个处理就是一片黑。 */
static size_t jpg_in(JDEC *jd, uint8_t *out, size_t n)
{
    jpg_src_t *s = (jpg_src_t *)jd->device;

    if (out == NULL) {                     /* 跳过 n 字节 */
        if (s->f) {
            if (fseek(s->f, (long)n, SEEK_CUR) != 0) return 0;
            return n;
        }
        size_t left = s->size - s->pos;
        if (n > left) n = left;
        s->pos += n;
        return n;
    }

    if (s->f) {
        return fread(out, 1, n, s->f);
    }
    size_t left = s->size - s->pos;
    if (n > left) n = left;
    if (n == 0) return 0;
    memcpy(out, s->mem + s->pos, n);
    s->pos += n;
    return n;
}

/* tjpgd 的逐块输出回调：RGB888 -> RGB565 写进 dst 缓冲。
 * 字节序：面板配置的是 LCD_RGB_DATA_ENDIAN_LITTLE（见 lcd_init.c），
 * 与 LVGL 一致 —— 低字节在前。 */
static int jpg_out(JDEC *jd, void *bitmap, JRECT *rect)
{
    (void)jd;
    const uint8_t *pix = (const uint8_t *)bitmap;      /* RGB888 */
    const blit_ctx_t *c = &s_blit;
    uint32_t rx = (uint32_t)rect->right - rect->left + 1;

    for (uint32_t y = rect->top; y <= rect->bottom; y++) {
        /* 每 32 行让出一次 CPU：大图解码要好几秒，期间空闲任务完全跑不到会触发任务看门狗 */
        if ((++s_blit_rows & 31u) == 0u) {
            vTaskDelay(1);
        }
        int dy = c->oy + (int)((uint64_t)y * c->oh / c->src_h);
        if (dy < 0 || dy >= c->dst_h) {
            pix += rx * 3;
            continue;
        }
        uint8_t *row = &c->dst[(uint32_t)dy * (uint32_t)c->dst_w * 2];
        for (uint32_t x = rect->left; x <= rect->right; x++) {
            int dx = c->dx_map[x];
            /* tjpgd 的 out 回调喂的是 BGR（见 tjpgd.c 里 B/G/R 顺序的注释），
             * 故 R 取 pix[2]、B 取 pix[0] */
            uint16_t c565 = (uint16_t)(((pix[2] >> 3) << 11) | ((pix[1] >> 2) << 5) | (pix[0] >> 3));
            uint32_t idx = (uint32_t)dx * 2;
            row[idx]     = (uint8_t)(c565 & 0xFF);     /* 低字节在前（小端） */
            row[idx + 1] = (uint8_t)(c565 >> 8);
            pix += 3;
        }
    }
    return 1;                                          /* 1 = 继续解码 */
}

/* tjpgd 的工作区（4KB，放内部 RAM 快一点） */
static bool ensure_pool(void)
{
    if (!s_pool) {
        s_pool = (uint8_t *)heap_caps_malloc(JPEG_POOL_SIZE, MALLOC_CAP_INTERNAL);
    }
    if (!s_pool) {
        ESP_LOGE(TAG, "tjpgd 工作区分配失败");
        return false;
    }
    return true;
}

static const char *jres_str(JRESULT r)
{
    switch (r) {
    case JDR_OK:    return "OK";
    case JDR_INTR:  return "被输出函数中断";
    case JDR_INP:   return "数据错误/提前结束";
    case JDR_MEM1:  return "工作区不足";
    case JDR_MEM2:  return "输入缓冲不足";
    case JDR_PAR:   return "参数错误";
    case JDR_FMT1:  return "格式错误(数据损坏)";
    case JDR_FMT2:  return "格式不支持";
    case JDR_FMT3:  return "不支持的 JPEG 标准(如渐进式)";
    default:        return "未知";
    }
}

/* 把一张 JPEG 解码到 dst 缓冲的指定矩形里（保持比例、居中、四周黑边）。
 * dst/dst_w/dst_h 是目标缓冲，ax/ay/aw/ah 是里面允许绘制的区域。 */
static bool jpeg_blit_to(jpg_src_t *src, uint8_t *dst, int dst_w, int dst_h,
                         int ax, int ay, int aw, int ah)
{
    if (!ensure_pool()) return false;

    JDEC jd;
    JRESULT rc = jd_prepare(&jd, jpg_in, s_pool, JPEG_POOL_SIZE, src);
    if (rc != JDR_OK) {
        ESP_LOGW(TAG, "jd_prepare: %s (%d)", jres_str(rc), (int)rc);
        return false;
    }
    int w = jd.width, h = jd.height;
    if (w <= 0 || h <= 0) return false;
    s_jpeg_w = w;
    s_jpeg_h = h;

    /* 在给定区域里按原比例尽量放大，居中 */
    int ow = w, oh = h;
    if (ow > aw) ow = aw;
    if (oh > ah) oh = ah;
    if ((int64_t)ow * h > (int64_t)oh * w) ow = (int)((int64_t)oh * w / h);
    else                                    oh = (int)((int64_t)ow * h / w);
    int ox = ax + (aw - ow) / 2;
    int oy = ay + (ah - oh) / 2;

    uint16_t *dx = (uint16_t *)heap_caps_malloc((size_t)w * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    if (!dx) {
        ESP_LOGE(TAG, "dx_map 分配失败 (%d 列)", w);
        return false;
    }
    for (int x = 0; x < w; x++) {
        dx[x] = (uint16_t)(ox + (int)((int64_t)x * ow / w));
    }

    /* 先把这块区域刷黑，比屏幕小的图四周留黑边 */
    for (int y = ay; y < ay + ah; y++) {
        memset(&dst[((uint32_t)y * (uint32_t)dst_w + ax) * 2], 0, (size_t)aw * 2);
    }

    s_blit_rows = 0;
    s_blit.dst = dst; s_blit.dst_w = dst_w; s_blit.dst_h = dst_h;
    s_blit.ox = ox; s_blit.oy = oy;
    s_blit.ow = ow; s_blit.oh = oh;
    s_blit.src_w = w; s_blit.src_h = h;
    s_blit.dx_map = dx;

    rc = jd_decomp(&jd, jpg_out, 0);
    heap_caps_free(dx);
    if (rc != JDR_OK) {
        ESP_LOGW(TAG, "jd_decomp: %s (%d)  [%dx%d]", jres_str(rc), (int)rc, w, h);
        return false;
    }
    return true;
}

/* ------------------------------ 图片查看界面 ------------------------------ */

static void img_show(const char *path);

static void img_prev_cb(lv_event_t *e)
{
    (void)e;
    char np[256];
    if (file_mgr_media_neighbor(s_path, -1, np, sizeof(np))) img_show(np);
}

static void img_next_cb(lv_event_t *e)
{
    (void)e;
    char np[256];
    if (file_mgr_media_neighbor(s_path, 1, np, sizeof(np))) img_show(np);
}

static void media_close_btn_cb(lv_event_t *e)
{
    (void)e;
    media_stop();
}

static lv_obj_t *make_bar_btn(lv_obj_t *parent, const char *text, int w,
                              lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, w, 32);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, &lv_font_cn_16, 0);
    lv_obj_center(lbl);
    if (cb) lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    return btn;
}

static void build_img_screen(void)
{
    s_img_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_img_scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_img_scr, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_img_scr, 0, 0);
    lv_obj_set_style_border_opa(s_img_scr, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(s_img_scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(s_img_scr, LV_SCROLLBAR_MODE_OFF);

    /* 顶栏 */
    lv_obj_t *bar = lv_obj_create(s_img_scr);
    lv_obj_set_size(bar, LCD_W, 40);
    lv_obj_align(bar, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x16161F), 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_border_opa(bar, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(bar, 8, 0);
    lv_obj_set_style_pad_column(bar, 8, 0);
    make_bar_btn(bar, "关闭", 70, media_close_btn_cb);

    s_img_name = lv_label_create(bar);
    lv_label_set_text(s_img_name, "");
    lv_obj_set_style_text_font(s_img_name, &lv_font_cn_16, 0);
    lv_obj_set_style_text_color(s_img_name, lv_color_white(), 0);
    lv_obj_set_width(s_img_name, LCD_W - 110);
    lv_label_set_long_mode(s_img_name, LV_LABEL_LONG_DOT);

    /* 图片本体（居中，缩放由 lv_image_set_scale 处理） */
    s_img_view = lv_image_create(s_img_scr);
    lv_obj_align(s_img_view, LV_ALIGN_CENTER, 0, 0);

    /* 失败提示（居中，默认隐藏） */
    s_img_msg = lv_label_create(s_img_scr);
    lv_obj_set_width(s_img_msg, LCD_W - 40);
    lv_obj_align(s_img_msg, LV_ALIGN_CENTER, 0, 0);
    lv_label_set_long_mode(s_img_msg, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_img_msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_img_msg, &lv_font_cn_16, 0);
    lv_obj_set_style_text_color(s_img_msg, lv_color_hex(0xFF6B6B), 0);
    lv_label_set_text(s_img_msg, "");
    lv_obj_add_flag(s_img_msg, LV_OBJ_FLAG_HIDDEN);

    /* 底栏：上一张 / 下一张 */
    lv_obj_t *bb = lv_obj_create(s_img_scr);
    lv_obj_set_size(bb, LCD_W, 40);
    lv_obj_align(bb, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_bg_color(bb, lv_color_hex(0x16161F), 0);
    lv_obj_set_style_radius(bb, 0, 0);
    lv_obj_set_style_border_opa(bb, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(bb, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bb, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(bb, 8, 0);
    make_bar_btn(bb, "< 上一张", 100, img_prev_cb);
    make_bar_btn(bb, "下一张 >", 100, img_next_cb);
}

/* 图片可视区：上下各留 40px 的标题栏/底栏 */
#define IMG_AREA_Y   40
#define IMG_AREA_H   (LCD_H - 80)
#define IMG_AREA_W   LCD_W

/* JPEG：自己用 tjpgd 流式解码（LVGL 是按原始分辨率整帧解码的，1200 万像素的图
 * 它会直接分配失败 → 一片黑），解完写进 PSRAM 的 RGB565 缓冲，
 * 再包成 lv_image_dsc_t 交给 LVGL 显示 —— 走的是和桌面壁纸同一条已验证的渲染路径，
 * 不用去碰 framebuffer，也就没有字节序和「被 LVGL 刷黑」的时序问题。 */
static bool img_show_jpeg(const char *path)
{
    if (!s_img_buf) {
        s_img_buf = (uint8_t *)heap_caps_malloc((size_t)IMG_AREA_W * IMG_AREA_H * 2,
                                                MALLOC_CAP_SPIRAM);
        if (!s_img_buf) {
            ESP_LOGE(TAG, "看图缓冲分配失败 (%d 字节)", IMG_AREA_W * IMG_AREA_H * 2);
            return false;
        }
    }

    FILE *f = fopen(path, "rb");
    if (!f) return false;

    int64_t t0 = esp_timer_get_time();
    jpg_src_t src = { .mem = NULL, .size = 0, .pos = 0, .f = f };
    bool ok = jpeg_blit_to(&src, s_img_buf, IMG_AREA_W, IMG_AREA_H,
                           0, 0, IMG_AREA_W, IMG_AREA_H);
    fclose(f);
    if (!ok) return false;
    ESP_LOGI(TAG, "图片 %s: %dx%d -> %dx%d, 解码 %lld ms",
             path, s_jpeg_w, s_jpeg_h, IMG_AREA_W, IMG_AREA_H,
             (long long)((esp_timer_get_time() - t0) / 1000));

    /* 包成 LVGL 图片描述符（RGB565，与面板的小端一致） */
    s_img_dsc.header.magic      = LV_IMAGE_HEADER_MAGIC;
    s_img_dsc.header.cf         = LV_COLOR_FORMAT_RGB565;
    s_img_dsc.header.flags      = 0;
    s_img_dsc.header.w          = IMG_AREA_W;
    s_img_dsc.header.h          = IMG_AREA_H;
    s_img_dsc.header.stride     = IMG_AREA_W * 2;
    s_img_dsc.header.reserved_2 = 0;
    s_img_dsc.data_size         = (uint32_t)IMG_AREA_W * IMG_AREA_H * 2;
    s_img_dsc.data              = s_img_buf;

    /* 清缓存：s_img_dsc 这个指针每次都复用（内容换了、地址不变），
     * 若上一张被缓存住，LVGL 会直接拿旧数据，切换图片时不刷新 */
    lv_image_cache_drop(NULL);
    lv_image_set_scale(s_img_view, LV_SCALE_NONE);
    lv_obj_clear_flag(s_img_view, LV_OBJ_FLAG_HIDDEN);
    lv_image_set_src(s_img_view, &s_img_dsc);
    /* 每次都指向同一个 s_img_dsc，LVGL 的 header 缓存可能认为没变化，手动标脏 */
    lv_obj_invalidate(s_img_view);
    return true;
}

/* 显示失败：把原因写在屏幕中央（红色）并隐藏图片控件 */
static void img_fail(const char *fmt, ...)
{
    char msg[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    ESP_LOGE(TAG, "看图失败: %s", msg);
    lv_obj_add_flag(s_img_view, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(s_img_msg, msg);
    lv_obj_clear_flag(s_img_msg, LV_OBJ_FLAG_HIDDEN);
}

/* LVGL 解码失败时不给原因（内部日志还没开），这里用 lodepng 原样再解一遍，
 * 把错误码打出来，顺带看看解码前后的内存情况 —— 只为排查用 */
static void png_diag(const char *lvgl_path)
{
    lv_fs_file_t f;
    if (lv_fs_open(&f, lvgl_path, LV_FS_MODE_RD) != LV_FS_RES_OK) {
        ESP_LOGE(TAG, "诊断: lv_fs_open 失败 %s", lvgl_path);
        return;
    }
    uint32_t sz = 0;
    lv_fs_seek(&f, 0, LV_FS_SEEK_END);
    lv_fs_tell(&f, &sz);
    lv_fs_seek(&f, 0, LV_FS_SEEK_SET);
    uint8_t *buf = (uint8_t *)lv_malloc(sz);
    uint32_t br = 0;
    lv_fs_res_t r = buf ? lv_fs_read(&f, buf, sz, &br) : LV_FS_RES_HW_ERR;
    lv_fs_close(&f);
    ESP_LOGE(TAG, "诊断: 文件大小=%u 读到=%u buf=%p", (unsigned)sz, (unsigned)br, buf);
    if (!buf || r != LV_FS_RES_OK) {
        if (buf) lv_free(buf);
        return;
    }

    lv_mem_monitor_t m;
    lv_mem_monitor(&m);
    ESP_LOGE(TAG, "诊断: 解码前 最大连续块=%u 空闲=%u 碎片=%u%%",
             (unsigned)m.free_biggest_size, (unsigned)m.free_size, (int)m.frag_pct);

    unsigned char *out = NULL;
    unsigned w = 0, h = 0;
    unsigned err = lodepng_decode32(&out, &w, &h, buf, br);
    ESP_LOGE(TAG, "诊断: lodepng err=%u (%s) out=%p %ux%u",
             err, lodepng_error_text(err), out, w, h);

    if (out) lv_draw_buf_destroy((lv_draw_buf_t *)out);
    lv_free(buf);

    lv_mem_monitor(&m);
    ESP_LOGE(TAG, "诊断: 解码后 最大连续块=%u 空闲=%u 碎片=%u%%",
             (unsigned)m.free_biggest_size, (unsigned)m.free_size, (int)m.frag_pct);
}

static void img_show(const char *path)
{
    snprintf(s_path, sizeof(s_path), "%s", path);
    make_lvgl_path(s_lvgl_path, sizeof(s_lvgl_path), path);
    lv_label_set_text(s_img_name, base_name(path));
    lv_obj_add_flag(s_img_msg, LV_OBJ_FLAG_HIDDEN);

    /* 先看文件本身（大小/是不是真的 JPEG），别让解码器背锅 */
    struct stat st;
    if (stat(path, &st) != 0) {
        img_fail("打不开文件");
        return;
    }
    ESP_LOGI(TAG, "打开图片 %s (%ld 字节)", path, (long)st.st_size);
    if (st.st_size < 128) {
        img_fail("文件太小 (%ld 字节)", (long)st.st_size);
        return;
    }

    /* JPEG 走自己的解码器（大图也能看），PNG/BMP 交给 LVGL */
    bool is_jpeg = ext_is(path, ".jpg") || ext_is(path, ".jpeg");
    if (is_jpeg) {
        FILE *f = fopen(path, "rb");
        uint8_t head[2] = {0, 0};
        if (f) {
            (void)fread(head, 1, 2, f);
            fclose(f);
        }
        if (head[0] != 0xFF || head[1] != 0xD8) {
            img_fail("不是 JPEG 文件 (头 %02X %02X)", (unsigned)head[0], (unsigned)head[1]);
            return;
        }
        if (img_show_jpeg(path)) {
            lv_obj_add_flag(s_img_msg, LV_OBJ_FLAG_HIDDEN);
            return;
        }
        ESP_LOGW(TAG, "自己的 JPEG 解码器失败，回退 LVGL: %s", path);
    }

    lv_image_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    if (lv_image_decoder_get_info(s_lvgl_path, &hdr) != LV_RESULT_OK || hdr.w == 0 || hdr.h == 0) {
        img_fail("无法识别的图片\n%s", s_lvgl_path);
        return;
    }
    /* PNG 交给 LVGL 整帧解码成 ARGB8888（宽*高*4），屏幕才 320x240，
     * 再大纯属浪费内存，直接提示转 JPG */
    const uint32_t max_px = (uint32_t)LCD_W * (uint32_t)LCD_H;   /* 76800 */
    if ((uint64_t)hdr.w * hdr.h > max_px) {
        img_fail("PNG 只支持到 %dx%d\n这张是 %dx%d，请转成 JPG",
                 LCD_W, LCD_H, (int)hdr.w, (int)hdr.h);
        return;
    }

    /* ★ LVGL 解 PNG 时，池里同时活着四块（都是 lodepng 的 lv_malloc）：
     *   ① 文件缓冲   = 整个文件大小（lodepng_load_file 整读）
     *   ② idat       ≈ 文件大小（压缩数据，解压时还在）
     *   ③ 解压扫描线 = w*h*4 + h
     *   ④ zlib window = 32KB
     * 解出的那一帧（w*h*4）走 draw_buf 回调，lvgl_port.c 里已改到系统 PSRAM，不占池。
     * 所以要查两处：池够不够放 ①②③④、PSRAM 够不够放那一帧。 */
    lv_mem_monitor_t mon;
    lv_mem_monitor(&mon);
    const uint32_t frame     = hdr.w * hdr.h * 4u;                            /* 解出的帧：系统 PSRAM */
    const uint32_t scanline  = frame + (uint32_t)hdr.h;                       /* 解压扫描线：池 */
    const uint32_t pool_need = (uint32_t)st.st_size * 2u                      /* ①文件 + ②idat */
                             + scanline                                       /* ③ */
                             + 32u * 1024u;                                   /* ④zlib window */
    const uint32_t psram     = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "LVGL 解码 %dx%d：帧 %u 走 PSRAM(空闲 %u)，池里要 %u(文件+idat %ld*2, 扫描线 %u, window 32K)，池空闲 %u 最大块 %u",
             (int)hdr.w, (int)hdr.h, (unsigned)frame, (unsigned)psram,
             (unsigned)pool_need, (long)st.st_size * 2, (unsigned)scanline,
             (unsigned)mon.free_size, (unsigned)mon.free_biggest_size);
    if (scanline > mon.free_biggest_size || pool_need > mon.free_size || frame > psram) {
        img_fail("内存不足\n帧需 PSRAM %u(空闲 %u)\n池需 %u(空闲 %u, 最大块 %u)\n建议转成 JPG 再看",
                 (unsigned)frame, (unsigned)psram, (unsigned)pool_need,
                 (unsigned)mon.free_size, (unsigned)mon.free_biggest_size);
        return;
    }

    /* 缩放：塞进可视区（256 = 100%） */
    const int maxw = LCD_W - 8;
    const int maxh = LCD_H - 80;
    uint32_t zoom = 256;
    if (hdr.w > (uint32_t)maxw || hdr.h > (uint32_t)maxh) {
        uint32_t zw = (uint32_t)maxw * 256u / hdr.w;
        uint32_t zh = (uint32_t)maxh * 256u / hdr.h;
        zoom = (zw < zh) ? zw : zh;
        if (zoom < 32) zoom = 32;
    }
    /* 换图前清掉上一张的解码缓存：缓存里那张整帧还占着 w*h*4 */
    lv_image_cache_drop(NULL);

    /* LVGL 是「绘制时才解码」，解码失败只会留下一片黑，这里先自己解一次，
     * 失败马上给提示；解完留在缓存里，后面真正绘制时不会再解一遍 */
    lv_image_decoder_dsc_t dsc;
    lv_result_t rc = lv_image_decoder_open(&dsc, s_lvgl_path, NULL);
    if (rc != LV_RESULT_OK) {
        png_diag(s_lvgl_path);
        img_fail("LVGL 解码失败\n%s %dx%d（约需 %u 字节）",
                 base_name(path), (int)hdr.w, (int)hdr.h, (unsigned)frame);
        return;
    }
    ESP_LOGI(TAG, "LVGL 解码成功 %dx%d cf=%d 解码后 %u 字节 zoom=%u",
             (int)dsc.header.w, (int)dsc.header.h, (int)dsc.header.cf,
             (unsigned)((const lv_draw_buf_t *)dsc.decoded)->data_size, (unsigned)zoom);
    lv_image_decoder_close(&dsc);

    lv_image_set_scale(s_img_view, zoom);
    lv_obj_clear_flag(s_img_view, LV_OBJ_FLAG_HIDDEN);
    lv_image_set_src(s_img_view, s_lvgl_path);   /* PNG/BMP：交给 LVGL 解码 */
}

void media_image_open(const char *path)
{
    if (!s_img_scr) build_img_screen();
    s_return_scr = lv_screen_active();
    s_mode = MEDIA_IMAGE;
    lv_screen_load(s_img_scr);
    ui_set_status_bar_visible(false);        /* 全屏看图，状态栏让位（退出时恢复） */
    img_show(path);
}

/* ------------------------------ 对外接口 ------------------------------ */

void media_stop(void)
{
    /* 退出时清掉 LVGL 的图片解码缓存：一张 320x240 的 PNG 解码后就占 307KB PSRAM */
    lv_image_cache_drop(NULL);
    s_mode = MEDIA_IDLE;
    ui_set_status_bar_visible(true);
    if (s_return_scr) {
        lv_screen_load(s_return_scr);
    }
}

bool media_is_active(void)
{
    return s_mode != MEDIA_IDLE;
}
