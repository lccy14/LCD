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

/* MJPEG 解码直接用 LVGL 自带的 Tiny JPEG 解码器（编译进 LVGL，符号可链接） */
#if !LV_USE_TJPGD
#error "MJPEG 播放需要 LVGL 的 TJPGD：请开启 CONFIG_LV_USE_TJPGD=y"
#endif
#include "libs/tjpgd/tjpgd.h"

/* 界面字体：中文一律用 lv_font_cn_16 */
LV_FONT_DECLARE(lv_font_cn_16);

static const char *TAG = "MEDIA";

/* tjpgd 工作区（TJpgDec 建议 4096） */
#define MJPEG_POOL_SIZE   4096
/* 单帧 JPEG 缓冲（PSRAM）：先只开 64KB，遇到更大的帧再按需 realloc，
 * 上限 512KB。320x240 的 MJPEG 一帧一般 5~15KB，没必要一上来就占掉半兆 PSRAM。 */
#define MJPEG_FRAME_BUF_MIN (64 * 1024)
#define MJPEG_FRAME_BUF_MAX (512 * 1024)
/* 超过这个大小的帧提示一下：该重新编码成 320x240 了 */
#define MJPEG_FRAME_WARN    (64 * 1024)
/* 裸 .mjpeg/.mjpg 没有容器/时间戳，帧率只能固定（微秒/帧，默认 15fps）。
 * 源片若是其他帧率：改这里，或用 ffmpeg -i x -c:v copy out.avi 套容器保留原帧率。 */
#define MJPEG_RAW_FRAME_US  66667

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
           ext_is(name, ".png") || ext_is(name, ".bmp");
}

bool media_is_video(const char *name)
{
    return ext_is(name, ".avi") || ext_is(name, ".mjpeg") || ext_is(name, ".mjpg");
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

typedef enum { MEDIA_IDLE = 0, MEDIA_IMAGE, MEDIA_VIDEO } media_mode_t;

static media_mode_t s_mode = MEDIA_IDLE;
static lv_obj_t    *s_return_scr = NULL;      /* 退出时回到哪个界面 */
static char         s_path[256];              /* 当前文件完整路径 */
static char         s_lvgl_path[264];         /* 给 LVGL 的路径（必须常驻，LVGL 只存指针） */

/* ---- 图片查看界面 ---- */
static lv_obj_t *s_img_scr  = NULL;
static lv_obj_t *s_img_view = NULL;
static lv_obj_t *s_img_name = NULL;
static lv_obj_t *s_img_msg  = NULL;   /* 解码失败时的居中提示（别再让人看一片黑） */

/* ---- 视频播放界面 ---- */
static lv_obj_t *s_vid_scr  = NULL;
static lv_obj_t *s_vid_ctrl = NULL;
static lv_obj_t *s_vid_lbl  = NULL;

/* ---- 图片：解码后的 RGB565 缓冲（PSRAM）+ 给 LVGL 的描述符 ---- */
static uint8_t      *s_img_buf = NULL;
static lv_image_dsc_t s_img_dsc = {0};
static int           s_jpeg_w = 0, s_jpeg_h = 0;   /* 最近一次解出来的原图尺寸（日志用） */

/* ---- 视频运行状态 ---- */
static FILE      *s_avi = NULL;
static long       s_movi_start = 0, s_movi_end = 0, s_pos = 0;
static uint32_t   s_frame_us = 66667;         /* 默认 15fps */
static int        s_frame_idx = 0, s_frame_total = 0;
static bool       s_paused = false;
static int64_t    s_next_frame_us = 0;
static int        s_fail_streak = 0;        /* 连续解码失败次数 */
static bool       s_warned_not_jpeg = false; /* 「不是 JPEG」只提示一次，别刷屏 */
static bool       s_raw_mjpeg = false;       /* true=裸 .mjpeg/.mjpg 流（无 AVI 容器） */
static bool       s_first_flush = false;     /* 本次播放的第一帧（需先排空 LVGL 切屏事务） */

/* ---- JPEG 解码状态 ----
 * 关键：tjpgd 是流式解码（MCU 一块一块出），所以不管原图多大，
 * 都不需要整帧缓冲——边解边按目标尺寸抽稀写进 framebuffer 就行。 */
typedef struct {
    const uint8_t *mem;                       /* 内存数据源（视频帧） */
    size_t         size, pos;
    FILE          *f;                         /* 文件数据源（图片，可边读边解） */
} jpg_src_t;

typedef struct {
    uint8_t *dst;                             /* 目标缓冲（图片=PSRAM 的 RGB565 图，视频=g_framebuffer） */
    int      dst_w, dst_h;                    /* 目标缓冲的宽高（像素） */
    int ox, oy, ow, oh;                       /* 输出区域（已居中、保持比例） */
    int src_w, src_h;
    const uint16_t *dx_map;                   /* 源列 -> 目标列（避免每像素做除法） */
} blit_ctx_t;

static blit_ctx_t s_blit;
static uint32_t   s_blit_rows = 0;            /* 解码计数，用于周期性让出 CPU */
static uint8_t   *s_pool = NULL;              /* tjpgd 工作区（内部 RAM，快） */
static uint8_t   *s_jpg_buf = NULL;           /* 单帧 JPEG（PSRAM，按需增长） */
static size_t     s_jpg_buf_size = 0;

/* ------------------------------ AVI 解析 ------------------------------ */

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void avi_close(void)
{
    if (s_avi) {
        fclose(s_avi);
        s_avi = NULL;
    }
}

/* AVI 里的信息（帧率、总帧数、movi 位置、视频编码 fourcc） */
typedef struct {
    long     movi_start, movi_end;
    uint32_t frame_us;
    int      total_frames;
    uint32_t codec;             /* strf 里的 biCompression，'MJPG' 才是我们能解码的 */
    int      width, height;
} avi_info_t;

static void fcc_str(uint32_t fcc, char out[5])
{
    out[0] = (char)(fcc & 0xFF);
    out[1] = (char)((fcc >> 8) & 0xFF);
    out[2] = (char)((fcc >> 16) & 0xFF);
    out[3] = (char)((fcc >> 24) & 0xFF);
    out[4] = '\0';
}

/* 递归扫 RIFF 块：avih 在 LIST 'hdrl' 里，视频编码在 hdrl→strl→strf 里，
 * 帧数据在 LIST 'movi' 里。depth 限制递归层数，防止坏文件把栈跑穿。 */
static void avi_scan(long start, long end, int depth, avi_info_t *info)
{
    bool vids_pending = false;              /* 刚读过 'vids' 的 strh，下一个 strf 是视频格式 */

    long pos = start;
    while (pos + 8 <= end) {
        uint8_t ch[8];
        fseek(s_avi, pos, SEEK_SET);
        if (fread(ch, 1, 8, s_avi) != 8) break;
        uint32_t sz = rd32(ch + 4);

        if (memcmp(ch, "LIST", 4) == 0) {
            char type[4];
            if (fread(type, 1, 4, s_avi) != 4) break;
            if (memcmp(type, "movi", 4) == 0) {
                info->movi_start = pos + 12;
                info->movi_end   = pos + 8 + (long)sz;
                return;                     /* 后面只有索引，不用看了 */
            }
            if (depth < 3) {
                avi_scan(pos + 12, pos + 8 + (long)sz, depth + 1, info);
            }
        } else if (memcmp(ch, "avih", 4) == 0) {
            uint8_t avih[32];
            if (fread(avih, 1, 32, s_avi) == 32) {
                uint32_t uspf = rd32(avih);            /* dwMicroSecPerFrame */
                if (uspf >= 2000 && uspf <= 500000) info->frame_us = uspf;
                info->total_frames = (int)rd32(avih + 16);
            }
        } else if (memcmp(ch, "strh", 4) == 0) {
            uint8_t strh[8];
            if (fread(strh, 1, 8, s_avi) == 8) {
                vids_pending = (memcmp(strh, "vids", 4) == 0);
                if (vids_pending && !info->codec) {
                    /* 先记下 strh 里的 handler（有的文件 strf 缺失/是 RGB） */
                    uint32_t handler = rd32(strh + 4);
                    if (handler != 0) info->codec = handler;
                }
            }
        } else if (memcmp(ch, "strf", 4) == 0 && vids_pending) {
            uint8_t bih[40];
            if (fread(bih, 1, 40, s_avi) == 40) {
                info->width  = (int)(int32_t)rd32(bih + 4);
                info->height = (int)(int32_t)rd32(bih + 8);
                info->codec  = rd32(bih + 16);         /* biCompression */
            }
            vids_pending = false;
        }
        pos += 8 + (long)sz + ((sz & 1) ? 1 : 0);
    }
}

/* 打开 AVI：确认是 MJPEG 编码才让播，否则直接说清楚是什么编码 */
static bool avi_open(const char *path)
{
    s_raw_mjpeg = false;
    s_avi = fopen(path, "rb");
    if (!s_avi) {
        ESP_LOGW(TAG, "打开失败: %s", path);
        return false;
    }
    fseek(s_avi, 0, SEEK_END);
    long fsize = ftell(s_avi);
    fseek(s_avi, 0, SEEK_SET);

    uint8_t h[12];
    if (fread(h, 1, 12, s_avi) != 12 || memcmp(h, "RIFF", 4) != 0 || memcmp(h + 8, "AVI ", 4) != 0) {
        ESP_LOGW(TAG, "不是 AVI: %s", path);
        avi_close();
        return false;
    }

    avi_info_t info = {
        .movi_start = 0, .movi_end = 0,
        .frame_us = 66667,                /* 15fps 兜底 */
        .total_frames = 0, .codec = 0, .width = 0, .height = 0,
    };
    avi_scan(12, fsize, 0, &info);

    if (info.movi_start <= 0 || info.movi_end <= info.movi_start) {
        ESP_LOGW(TAG, "没找到 movi 数据块: %s", path);
        avi_close();
        return false;
    }

    /* 必须是 MJPEG（fourcc 'MJPG'）；H.264/XVID 这些软解跑不动，直接拒绝并说明白 */
    char fcc[5];
    fcc_str(info.codec, fcc);
    if (info.codec != 0x47504A4D) {          /* 0x47504A4D = 'MJPG' (小端读出来) */
        ESP_LOGE(TAG, "不支持的视频编码: %s（只支持 MJPEG）", fcc[0] ? fcc : "未知");
        ESP_LOGE(TAG, "请用: ffmpeg -i 原视频 -vf scale=320:240 -r 15 -c:v mjpeg -q:v 4 -an out.avi");
        avi_close();
        return false;
    }

    s_movi_start  = info.movi_start;
    s_movi_end    = info.movi_end;
    s_frame_us    = info.frame_us;
    s_frame_total = info.total_frames;
    s_pos         = s_movi_start;
    s_frame_idx   = 0;
    s_fail_streak = 0;
    s_warned_not_jpeg = false;
    ESP_LOGI(TAG, "AVI 打开: %s  %dx%d  %s  %u fps  总帧数 %d  movi[%ld,%ld]",
             path, info.width, info.height, fcc,
             (unsigned)(1000000u / s_frame_us), s_frame_total, s_movi_start, s_movi_end);
    return true;
}

/* 读下一帧 JPEG 到 s_jpg_buf，返回长度；false = 播完/出错 */
static bool avi_next_frame(size_t *out_len)
{
    while (s_pos + 8 <= s_movi_end) {
        uint8_t ch[8];
        fseek(s_avi, s_pos, SEEK_SET);
        if (fread(ch, 1, 8, s_avi) != 8) return false;
        uint32_t sz = rd32(ch + 4);
        long data = s_pos + 8;
        s_pos = data + (long)sz + ((sz & 1) ? 1 : 0);

        if ((memcmp(ch, "00dc", 4) == 0 || memcmp(ch, "00db", 4) == 0) && sz > 32) {
            /* 缓冲不够就扩容（高分辨率 MJPEG 一帧能到几百 KB） */
            if (sz > s_jpg_buf_size) {
                if (sz > MJPEG_FRAME_BUF_MAX) {
                    ESP_LOGW(TAG, "帧过大 %u 字节，跳过（请用 ffmpeg 转成 320x240 的 MJPEG）",
                             (unsigned)sz);
                    continue;
                }
                uint8_t *nb = (uint8_t *)heap_caps_realloc(s_jpg_buf, sz, MALLOC_CAP_SPIRAM);
                if (!nb) {
                    ESP_LOGW(TAG, "第 %d 帧 %u 字节，缓冲扩容失败，跳过",
                             s_frame_idx + 1, (unsigned)sz);
                    continue;
                }
                s_jpg_buf = nb;
                s_jpg_buf_size = sz;
                ESP_LOGI(TAG, "帧缓冲扩到 %u 字节", (unsigned)s_jpg_buf_size);
            }
            if (sz > MJPEG_FRAME_WARN) {
                ESP_LOGW(TAG, "第 %d 帧 %u 字节偏大，解码会慢；建议转成 320x240",
                         s_frame_idx + 1, (unsigned)sz);
            }
            fseek(s_avi, data, SEEK_SET);
            if (fread(s_jpg_buf, 1, sz, s_avi) != sz) return false;
            /* 帧开头必须是 JPEG 的 SOI（FF D8），不是就别浪费时间去解码 */
            if (s_jpg_buf[0] != 0xFF || s_jpg_buf[1] != 0xD8) {
                if (!s_warned_not_jpeg) {
                    s_warned_not_jpeg = true;
                    ESP_LOGW(TAG, "帧数据不是 JPEG（没有 FF D8 开头），这不是 MJPEG 文件");
                }
                continue;
            }
            *out_len = sz;
            s_frame_idx++;
            return true;
        }
    }
    return false;
}

/* ------------------------ 裸 MJPEG 流（.mjpeg/.mjpg） ------------------------ */

#define RAW_READ_CHUNK 1024

/* 裸流没有容器头：文件本身就是一串紧挨着的 JPEG（FFD8…FFD9）。
 * 帧率固定用 MJPEG_RAW_FRAME_US，总帧数未知（暂停界面只显示"第 N 帧"）。 */
static bool raw_open(const char *path)
{
    s_avi = fopen(path, "rb");
    if (!s_avi) {
        ESP_LOGW(TAG, "打开失败: %s", path);
        return false;
    }
    uint8_t soi[2];
    if (fread(soi, 1, 2, s_avi) != 2 || soi[0] != 0xFF || soi[1] != 0xD8) {
        ESP_LOGW(TAG, "不是裸 MJPEG（开头不是 FF D8）: %s", path);
        avi_close();
        return false;
    }
    fseek(s_avi, 0, SEEK_SET);               /* raw_next_frame 每次自己定位 SOI */

    s_raw_mjpeg   = true;
    s_movi_start  = 0;
    s_movi_end    = 0;
    s_pos         = 0;
    s_frame_us    = MJPEG_RAW_FRAME_US;
    s_frame_total = 0;
    s_frame_idx   = 0;
    s_fail_streak = 0;
    s_warned_not_jpeg = false;
    ESP_LOGI(TAG, "裸 MJPEG 打开: %s  固定 %u fps（容器无时间戳）",
             path, (unsigned)(1000000u / s_frame_us));
    return true;
}

/* 读下一帧 JPEG 到 s_jpg_buf，返回长度；false = 播完/出错。
 * 直接往帧缓冲尾部顺序读、原地找帧尾 FF D9（EOI）：JPEG 熵编码里的 FF 都按 FF00
 * 转义，所以 FF D9 只会出现在真正的帧尾，直接搜字节对即可，got_ff 只用来跨块。
 * 不用额外栈缓冲，也没有二次 memcpy；缓冲扩容/上限与 AVI 版一致，超大帧排空跳过。 */
static bool raw_next_frame(size_t *out_len)
{
    for (;;) {
        /* 1. 定位下一帧开头 FF D8（顺带容忍帧间杂字节；正常紧挨时只读 2 字节） */
        bool found_soi = false;
        int prev = -1, ch;
        while ((ch = fgetc(s_avi)) != EOF) {
            if (prev == 0xFF && ch == 0xD8) { found_soi = true; break; }
            prev = (ch == 0xFF) ? 0xFF : -1;
        }
        if (!found_soi) return false;

        /* 2. 直接读到帧缓冲尾部，原地扫 EOI（SOI 两字节先放进去） */
        size_t len = 0;
        s_jpg_buf[len++] = 0xFF;
        s_jpg_buf[len++] = 0xD8;
        bool got_ff = false;
        bool discard = false;
        bool have_frame = false;
        bool frame_ok = false;

        while (!have_frame) {
            if (!discard && len >= s_jpg_buf_size) {
                if (len >= MJPEG_FRAME_BUF_MAX) {
                    ESP_LOGW(TAG, "帧过大（>%d 字节），跳过（请用 ffmpeg 转成 320x240）",
                             MJPEG_FRAME_BUF_MAX);
                    discard = true;
                } else {
                    size_t ns = s_jpg_buf_size * 2;
                    if (ns > MJPEG_FRAME_BUF_MAX) ns = MJPEG_FRAME_BUF_MAX;
                    uint8_t *nb = (uint8_t *)heap_caps_realloc(s_jpg_buf, ns, MALLOC_CAP_SPIRAM);
                    if (!nb) {
                        ESP_LOGW(TAG, "第 %d 帧缓冲扩容失败，跳过", s_frame_idx + 1);
                        discard = true;
                    } else {
                        s_jpg_buf = nb;
                        s_jpg_buf_size = ns;
                        ESP_LOGI(TAG, "帧缓冲扩到 %u 字节", (unsigned)ns);
                    }
                }
            }

            /* 丢弃模式下往缓冲头部覆写（只为排空数据，内容不留） */
            uint8_t *dst = discard ? s_jpg_buf : (s_jpg_buf + len);
            size_t want = discard ? RAW_READ_CHUNK : (s_jpg_buf_size - len);
            if (want > RAW_READ_CHUNK) want = RAW_READ_CHUNK;
            size_t rd = fread(dst, 1, want, s_avi);
            if (rd == 0) return false;           /* 文件结束，最后一帧不完整 */

            size_t cut = rd;                     /* EOI 落在块内时，实际只消费到这里 */
            for (size_t i = 0; i < rd; i++) {
                if (got_ff && dst[i] == 0xD9) { cut = i + 1; have_frame = true; break; }
                got_ff = (dst[i] == 0xFF);
            }
            /* EOI 之后多读的字节（帧间间隙/下一帧开头）退还给流，避免漏帧 */
            if (have_frame && cut < rd) {
                fseek(s_avi, -((long)(rd - cut)), SEEK_CUR);
            }
            if (!discard) len += cut;
            if (have_frame) frame_ok = !discard;
        }

        if (frame_ok) {
            *out_len = len;
            s_frame_idx++;
            return true;
        }
        /* 超大帧已排空，外层循环继续找下一 SOI */
    }
}

/* --------------------------- MJPEG 解码 + 写帧缓冲 --------------------------- */

/* tjpgd 的数据源：内存（视频帧）或 FILE*（图片，边读边解，不占大缓冲）
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
            uint16_t c565 = (uint16_t)(((pix[0] >> 3) << 11) | ((pix[1] >> 2) << 5) | (pix[2] >> 3));
            uint32_t idx = (uint32_t)dx * 2;
            row[idx]     = (uint8_t)(c565 & 0xFF);     /* 低字节在前（小端） */
            row[idx + 1] = (uint8_t)(c565 >> 8);
            pix += 3;
        }
    }
    return 1;                                          /* 1 = 继续解码 */
}

/* tjpgd 的工作区（4KB，放内部 RAM 快一点），图片和视频共用 */
static bool ensure_pool(void)
{
    if (!s_pool) {
        s_pool = (uint8_t *)heap_caps_malloc(MJPEG_POOL_SIZE, MALLOC_CAP_INTERNAL);
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
    JRESULT rc = jd_prepare(&jd, jpg_in, s_pool, MJPEG_POOL_SIZE, src);
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
    /* PNG/BMP 会被 LVGL 整帧解码成 ARGB8888（宽*高*4），
     * 屏幕才 320x240，再大纯属浪费内存，直接提示转 JPG */
    const uint32_t max_px = (uint32_t)LCD_W * (uint32_t)LCD_H;   /* 76800 */
    if ((uint64_t)hdr.w * hdr.h > max_px) {
        img_fail("PNG/BMP 只支持到 %dx%d\n这张是 %dx%d，请转成 JPG",
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

/* ------------------------------ 视频播放界面 ------------------------------ */

static void vid_pause_cb(lv_event_t *e);
static void vid_resume_cb(lv_event_t *e);

static void build_vid_screen(void)
{
    s_vid_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_vid_scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_vid_scr, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_vid_scr, 0, 0);
    lv_obj_set_style_border_opa(s_vid_scr, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(s_vid_scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(s_vid_scr, LV_SCROLLBAR_MODE_OFF);

    /* 整屏透明按钮：轻触暂停（视频是直写 framebuffer 的，屏幕上盖不了实体控件） */
    lv_obj_t *tap = lv_button_create(s_vid_scr);
    lv_obj_set_size(tap, LCD_W, LCD_H);
    lv_obj_align(tap, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_opa(tap, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_opa(tap, LV_OPA_TRANSP, 0);
    lv_obj_set_style_shadow_opa(tap, LV_OPA_TRANSP, 0);
    lv_obj_add_event_cb(tap, vid_pause_cb, LV_EVENT_CLICKED, NULL);

    /* 暂停时才出现的控制条 */
    s_vid_ctrl = lv_obj_create(s_vid_scr);
    lv_obj_set_size(s_vid_ctrl, 240, 110);
    lv_obj_align(s_vid_ctrl, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(s_vid_ctrl, lv_color_hex(0x16161F), 0);
    lv_obj_set_style_bg_opa(s_vid_ctrl, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_vid_ctrl, 12, 0);
    lv_obj_set_style_border_opa(s_vid_ctrl, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(s_vid_ctrl, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_vid_ctrl, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_vid_ctrl, 10, 0);
    lv_obj_add_flag(s_vid_ctrl, LV_OBJ_FLAG_HIDDEN);

    s_vid_lbl = lv_label_create(s_vid_ctrl);
    lv_label_set_text(s_vid_lbl, "");
    lv_obj_set_style_text_font(s_vid_lbl, &lv_font_cn_16, 0);
    lv_obj_set_style_text_color(s_vid_lbl, lv_color_white(), 0);

    lv_obj_t *row = lv_obj_create(s_vid_ctrl);
    lv_obj_set_size(row, 220, 40);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    make_bar_btn(row, "继续", 96, vid_resume_cb);
    make_bar_btn(row, "退出", 96, media_close_btn_cb);
}

static void vid_pause_cb(lv_event_t *e)
{
    (void)e;
    if (s_mode != MEDIA_VIDEO || s_paused) return;
    s_paused = true;
    if (s_frame_total > 0) {
        lv_label_set_text_fmt(s_vid_lbl, "第 %d / %d 帧  已暂停", s_frame_idx, s_frame_total);
    } else {
        lv_label_set_text_fmt(s_vid_lbl, "第 %d 帧  已暂停", s_frame_idx);
    }
    lv_obj_clear_flag(s_vid_ctrl, LV_OBJ_FLAG_HIDDEN);
}

static void vid_resume_cb(lv_event_t *e)
{
    (void)e;
    lv_obj_add_flag(s_vid_ctrl, LV_OBJ_FLAG_HIDDEN);
    s_paused = false;
    s_next_frame_us = esp_timer_get_time();
}

static void media_toast(const char *text);   /* 定义在文件末尾 */

void media_video_play(const char *path)
{
    if (!s_jpg_buf) {
        s_jpg_buf = (uint8_t *)heap_caps_malloc(MJPEG_FRAME_BUF_MIN, MALLOC_CAP_SPIRAM);
        s_jpg_buf_size = s_jpg_buf ? MJPEG_FRAME_BUF_MIN : 0;
    }
    if (!s_jpg_buf || !ensure_pool()) {
        ESP_LOGE(TAG, "播放缓冲分配失败");
        media_toast("内存不足，无法播放");
        return;
    }

    /* .avi 走容器解析；.mjpeg/.mjpg 走裸流（按 FFD8/FFD9 切帧） */
    bool is_raw = !ext_is(path, ".avi");
    bool opened = is_raw ? raw_open(path) : avi_open(path);
    if (!opened) {
        /* 具体原因（编码不是 MJPEG / 打不开 / 没有 movi / 不是 JPEG 头）已打过日志 */
        ESP_LOGE(TAG, "无法播放: %s（见上面的原因）", path);
        media_toast(is_raw ? "无法播放：不是有效的裸 MJPEG 文件\n（文件开头应为 FF D8）"
                           : "无法播放：只支持 MJPEG 编码的 AVI\n"
                             "请用 ffmpeg 转: -c:v mjpeg（详见日志）");
        return;
    }

    snprintf(s_path, sizeof(s_path), "%s", path);
    if (!s_vid_scr) build_vid_screen();

    s_return_scr   = lv_screen_active();
    s_mode         = MEDIA_VIDEO;
    s_paused       = false;

    lv_obj_add_flag(s_vid_ctrl, LV_OBJ_FLAG_HIDDEN);
    lv_screen_load(s_vid_scr);
    ui_set_status_bar_visible(false);        /* 全屏播放，状态栏让位 */

    /* 不在此处清屏/刷屏：播放动作发生在 lv_timer_handler 内部，此刻塞一个读
     * g_framebuffer 的全屏 DMA 事务，会和 LVGL 切屏产生的一串 partial 事务
     * （队列深度 6）交错，下一圈解码再覆写 g_framebuffer 就和 DMA 抢缓冲 → 花屏。
     * 黑屏交给 s_vid_scr 自身渲染（partial 读的是内部 draw buf，不碰 g_framebuffer），
     * 第一帧解码成功后 jpeg_blit_to 会先清黑再整屏输出。 */
    s_first_flush  = true;
    s_next_frame_us = esp_timer_get_time();
}

/* ------------------------------ 对外接口 ------------------------------ */

/* 轻提示：播放失败时给用户一个看得见的反馈（4 秒后自动消失） */
static lv_obj_t *s_toast = NULL;
static lv_obj_t *s_toast_lbl = NULL;
static int64_t   s_toast_until = 0;

static void media_toast(const char *text)
{
    if (!s_toast) {
        s_toast = lv_obj_create(lv_layer_top());
        lv_obj_set_width(s_toast, LCD_W - 40);
        lv_obj_align(s_toast, LV_ALIGN_BOTTOM_MID, 0, -16);
        lv_obj_set_style_bg_color(s_toast, lv_color_hex(0x16161F), 0);
        lv_obj_set_style_bg_opa(s_toast, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(s_toast, 8, 0);
        lv_obj_set_style_border_opa(s_toast, LV_OPA_TRANSP, 0);
        lv_obj_set_style_pad_all(s_toast, 8, 0);

        s_toast_lbl = lv_label_create(s_toast);
        lv_obj_set_width(s_toast_lbl, LCD_W - 60);
        lv_label_set_long_mode(s_toast_lbl, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_font(s_toast_lbl, &lv_font_cn_16, 0);
        lv_obj_set_style_text_color(s_toast_lbl, lv_color_hex(0xFF6B6B), 0);
        lv_obj_set_style_text_align(s_toast_lbl, LV_TEXT_ALIGN_CENTER, 0);
    }
    lv_label_set_text(s_toast_lbl, text);
    lv_obj_clear_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
    s_toast_until = esp_timer_get_time() + 4000000;
}

void media_periodic(void)
{
    /* toast 自动消失（不管当前在看图还是播视频都要处理） */
    if (s_toast && s_toast_until != 0 && esp_timer_get_time() > s_toast_until) {
        lv_obj_add_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
        s_toast_until = 0;
    }

    if (s_mode != MEDIA_VIDEO || s_paused) return;

    int64_t now = esp_timer_get_time();
    if (now < s_next_frame_us) return;
    /* 落后超过 3 帧就丢帧，别越追越慢 */
    if (now - s_next_frame_us > (int64_t)s_frame_us * 3) {
        s_next_frame_us = now;
    } else {
        s_next_frame_us += s_frame_us;
    }

    size_t len = 0;
    int64_t t0 = esp_timer_get_time();
    bool got_frame = s_raw_mjpeg ? raw_next_frame(&len) : avi_next_frame(&len);
    int64_t t1 = esp_timer_get_time();
    if (!got_frame) {             /* 播完 */
        media_stop();
        return;
    }
    jpg_src_t src = { .mem = s_jpg_buf, .size = len, .pos = 0, .f = NULL };
    if (!jpeg_blit_to(&src, g_framebuffer, LCD_W, LCD_H, 0, 0, LCD_W, LCD_H)) {   /* 坏帧：跳过 */
        if (++s_fail_streak >= 10) {
            ESP_LOGE(TAG, "连续 %d 帧解码失败，停止播放（文件可能损坏或不是 MJPEG）", s_fail_streak);
            media_stop();
            return;
        }
        ESP_LOGW(TAG, "第 %d 帧解码失败", s_frame_idx);
        return;
    }
    int64_t t2 = esp_timer_get_time();
    s_fail_streak = 0;

    if (s_first_flush) {
        s_first_flush = false;
        /* 排空 LVGL 切屏残留事务：先清已完成计数，再等到连续 6ms 无完成（partial
         * 间隔约 2ms），保证下面的全屏事务前面没有别的事务排队/在传 */
        LCD_DrainFlushDone();
        while (LCD_WaitFlushDone(6)) { }
        ESP_LOGI(TAG, "首帧 %u 字节 %dx%d  读帧 %lldms 解码 %lldms",
                 (unsigned)len, s_jpeg_w, s_jpeg_h,
                 (long long)(t1 - t0) / 1000, (long long)(t2 - t1) / 1000);
        if (s_jpeg_w > LCD_W || s_jpeg_h > LCD_H) {
            ESP_LOGW(TAG, "视频分辨率 %dx%d 大于屏幕 %dx%d：软解缩放很慢，请用 "
                          "\"ffmpeg -i in -vf scale=%d:%d -r 15 -c:v mjpeg -q:v 4 -an out.avi\" 重转",
                     s_jpeg_w, s_jpeg_h, LCD_W, LCD_H, LCD_W, LCD_H);
        }
    }

    LCD_Flush_All();
    /* DMA 没发完前绝不能解码下一帧：解码直接写同一个 g_framebuffer，抢缓冲就是花屏。
     * 播放期间没有 LVGL partial 事务（视频屏是静态的），这个完成信号就是本次全屏事务。 */
    LCD_WaitFlushDone(200);

    /* 耗时统计：每 30 帧一条，卡顿在「读 SD」还是「tjgpd 解码」一目了然 */
    static int     stat_n = 0;
    static int64_t stat_read_us = 0, stat_dec_us = 0;
    static size_t  stat_bytes = 0;
    stat_n++;
    stat_read_us += t1 - t0;
    stat_dec_us  += t2 - t1;
    stat_bytes   += len;
    if (stat_n >= 30) {
        int64_t avg_read = stat_read_us / stat_n;   /* 微秒/帧 */
        int64_t avg_dec  = stat_dec_us / stat_n;
        ESP_LOGI(TAG, "近30帧平均: 读帧 %lldms 解码 %lldms 帧大小 %u 字节",
                 (long long)((avg_read + 500) / 1000),
                 (long long)((avg_dec + 500) / 1000),
                 (unsigned)(stat_bytes / stat_n));
        stat_n = 0; stat_read_us = 0; stat_dec_us = 0; stat_bytes = 0;
    }
}

void media_stop(void)
{
    if (s_mode == MEDIA_VIDEO) {
        avi_close();
        s_paused = false;
    }
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
