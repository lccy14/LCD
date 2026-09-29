#include "lvgl_port.h"
#include "lcd.h"
#include "lcd_init.h"
#include "esp_lcd_panel_ops.h"   /* esp_lcd_panel_draw_bitmap */
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
/* lv_draw_buf_handlers_t 的字段定义在私有头里（要改图片解码缓冲的分配回调） */
#include "src/draw/lv_draw_buf_private.h"

static const char *TAG = "LVGL_PORT";

/* ui.c 创建的 LVGL 主任务句柄, DMA 完成时通过 xTaskNotifyGive 唤醒它,
 * 替代 vTaskDelay 固定 10ms 等待, 滑动 FPS 46→80+。 */
extern TaskHandle_t g_lvgl_task;

/* 额外给 LVGL 挂的 PSRAM 内存池总大小（KB）。
 * 内置的 LV_MEM（sdkconfig: CONFIG_LV_MEM_SIZE_KILOBYTES，当前 16KB）装不下所有
 * App 屏幕的常驻对象——实测开 3~4 个 App 就用到 99%，maxfree 只剩 608 字节，
 * 之后 lv_malloc 失败会触发断言死循环，进而 task_wdt 复位。
 *
 * ★ 关键：TLSF 不能跨池合并，所以「LVGL 单次最大可分配块」= 单个池的大小。
 *   全用 64KB 小池堆到 512KB，maxfree 就永远只有 64KB —— 看图时 PNG 解码要
 *   320*240*4 = 307KB 连续块，必然失败，屏幕上就是一片黑（已实测踩坑）。
 *   所以第一块必须挂一整块大的，剩下的再用小块补足总量。
 *
 * 单池上限（见 lv_tlsf.c）：
 *     #define TLSF_MAX_POOL_SIZE (LV_MEM_SIZE + LV_MEM_POOL_EXPAND_SIZE)
 *     block_size_max = 1 << log2_ceil(TLSF_MAX_POOL_SIZE)
 * 现在 LV_MEM_SIZE=16KB、LV_MEM_POOL_EXPAND_SIZE=512KB → 上限 1MB，
 * 挂 512KB 单池没问题（EXPAND 只抬高上限，本身不占内存）。 */
/* 池子总量 = k_pool_chunks[] 之和 = 1024KB（PSRAM 空闲约 1.0MB，够用）
 *
 * 看图解码一次 PNG，LVGL 池里同时活着四块（都是 lodepng 的 lv_malloc）：
 *   文件缓冲    = 整个文件大小（lodepng_load_file 整读）        320x240 那张 = 162KB
 *   idat        = 压缩数据（≈ 文件大小）                                    ≈ 162KB
 *   解压扫描线  = w*h*4 + h                                                   307KB
 *   zlib window = 32KB
 * 合计 ≈ 663KB，再加上 App 常驻对象，池子得给到 1MB 左右才转得开。
 * 注意：解出来的那一帧（w*h*4）已经改走系统 PSRAM，不占这里的池。
 *
 * 384KB 那个池是给「文件/idat」这种中等块用的：TLSF 是最佳适配，
 * 有它接着，512KB 的大池就不会被切碎，扫描线那 307KB 才有地方落脚。 */
static const size_t k_pool_chunks[] = {
    512 * 1024,     /* 看图：扫描线（w*h*4）要连续空间，320x240 就是 307KB */
    384 * 1024,     /* 解码时同时活着的文件缓冲 + idat */
    64 * 1024,
    64 * 1024,
};

/* PARTIAL 渲染模式: 两个内部 RAM draw buf, 每个 320×40×2=25KB。
 * 内部 SRAM 访问速度比 PSRAM 快 5-10 倍, LVGL 渲染加速;
 * 两个 buf 让 LVGL 渲染下一块时上一块同时 DMA 发送, 双缓冲流水线。
 * 内部 RAM 空闲 240KB, 50KB 占用绰绰有余。 */
#define DISP_BUF_H    40
#define DISP_BUF_SIZE (LCD_W * DISP_BUF_H * sizeof(lv_color_t))

static lv_color_t *s_draw_buf1 = NULL;
static lv_color_t *s_draw_buf2 = NULL;

/* disp 句柄留存给 DMA 完成中断回调用（ISR 上下文调 lv_disp_flush_ready）。
 * 之前在 flush_cb 里同步调 lv_disp_flush_ready 是错的：esp_lcd_panel_draw_bitmap
 * 是异步的（事务推到 trans_queue 就返回），立刻让 LVGL 重写 framebuffer，
 * 而 DMA 还在从 framebuffer 取数据 → 撕裂/花屏。
 * 之前 CPU 字节序翻转循环恰好给 DMA 留了时间，凑巧掩盖了竞态；删除翻转后立刻暴露。
 * 正确做法：flush_cb 只触发 DMA，等 on_color_trans_done 中断里再通知 LVGL。 */
static lv_display_t *s_disp = NULL;

static bool on_color_trans_done_cb(esp_lcd_panel_io_handle_t io,
                                  esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
    (void)io; (void)edata; (void)user_ctx;
    if (s_disp) {
        lv_disp_flush_ready(s_disp);   /* 告诉 LVGL：DMA 已发完，可重写 framebuffer */
    }
    /* 任务通知唤醒: 替代 vTaskDelay 10ms 空等。
     * DMA 完成立刻通知主循环, 滑动 FPS 46→80+。
     * ISR 上下文调 xTaskNotifyGive 是安全的中断 API。 */
    if (g_lvgl_task) {
        BaseType_t higher_priority_task_woken = pdFALSE;
        vTaskNotifyGiveFromISR(g_lvgl_task, &higher_priority_task_woken);
        if (higher_priority_task_woken == pdTRUE) {
            portYIELD_FROM_ISR();   /* 立即切换到 LVGL 任务, 不等下一个 tick */
        }
    }
    return false;
}

/* 实际刷屏次数计数器, ui.c 的 ui_fps_update 每秒读差值作为真实 FPS 显示。
 * 替代之前的主循环圈数计数(包括空转, 不准)。 */
uint32_t g_flush_cnt = 0;

/* flush 回调：LVGL 在 draw buf 里渲染完一块脏区后调用。
 * PARTIAL 模式: area 是脏区, px_map 是 LVGL 渲染好的像素(在 s_draw_buf1/2 里)。
 * 用 esp_lcd_panel_draw_bitmap 直接把 px_map DMA 到屏幕对应坐标,
 * 坐标左闭右开(area 是闭区间, 需 +1)。
 * 字节序由 LCD_CAM 硬件 LITTLE_ENDIAN 处理, 无需 CPU 翻转。 */
static void disp_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    (void)disp;
    if (area->x2 < area->x1 || area->y2 < area->y1) {
        /* 空区域直接通知完成, 避免 DMA 空事务 */
        lv_disp_flush_ready(disp);
        return;
    }
    g_flush_cnt++;   /* 真实刷屏计数, ui_fps_update 读 */
    esp_lcd_panel_draw_bitmap(g_lcd_panel,
                              area->x1, area->y1,
                              area->x2 + 1, area->y2 + 1,
                              px_map);
    /* 不在此处调 flush_ready —— 等 on_color_trans_done 中断里通知 LVGL */
}

/* 给 LVGL 追加内存池（lv_mem_add_pool() 会把每块加进 LVGL 的 TLSF 分配器）。
 * cap 是 heap_caps 能力：MALLOC_CAP_SPIRAM（外部 PSRAM）或 MALLOC_CAP_INTERNAL。
 * 按 k_pool_chunks[] 依次挂载（先大块后小块），返回实际挂上的总字节数。
 * 注意：LVGL 内部日志(LV_USE_LOG)默认关闭，add_pool 失败时其 printf 也看不到，
 * 所以这里每一步都用 ESP_LOG 打点，便于确认卡在哪一步。 */
static size_t lvgl_add_mem_pool(uint32_t cap, const char *cap_name)
{
    const size_t n = sizeof(k_pool_chunks) / sizeof(k_pool_chunks[0]);
    size_t added = 0;

    for (size_t i = 0; i < n; i++) {
        size_t chunk = k_pool_chunks[i];
        void *p = heap_caps_malloc(chunk, cap);
        if (p == NULL) {
            ESP_LOGW(TAG, "%s: malloc %uKB failed (已挂 %uKB)，继续试小块",
                     cap_name, (unsigned)(chunk / 1024), (unsigned)(added / 1024));
            continue;                            /* 大块挂不上就退到小块 */
        }
        if (lv_mem_add_pool(p, chunk) == NULL) {
            ESP_LOGE(TAG, "%s: lv_mem_add_pool(%uKB @ %p) 失败（单池上限？）",
                     cap_name, (unsigned)(chunk / 1024), p);
            heap_caps_free(p);
            continue;
        }
        added += chunk;
        ESP_LOGI(TAG, "%s: +%uKB pool @ %p (total %uKB)",
                 cap_name, (unsigned)(chunk / 1024), p, (unsigned)(added / 1024));
    }
    ESP_LOGI(TAG, "%s: 共挂 %uKB，单次最大可分配 <= %uKB（TLSF 不能跨池合并）",
             cap_name, (unsigned)(added / 1024), (unsigned)(k_pool_chunks[0] / 1024));
    return added;
}

/* 图片解码出来的那一帧改走系统 PSRAM 堆，不再占 LVGL 内存池。
 *
 * 原因：LVGL 解 PNG 时三块内存是同时活着的（都在 lv_malloc = LVGL 池里）：
 *   ① 文件缓冲   = 整个文件大小（lodepng_load_file 整读）
 *   ② 解压扫描线 = w*h*4 + h（idat 解压结果）
 *   ③ 解出的帧   = w*h*4（lv_draw_buf_create_ex）
 * ② 要等 postProcessScanlines 用完才释放，③ 在那之前就分配好了，
 * 所以 320x240 的 PNG 峰值 ≈ 文件 + 2*w*h*4 ≈ 777KB，比池总量还大，
 * 必然 err=83（memory allocation failed）。
 * ③ 走的是这里的 draw_buf 分配回调，把它挪到系统 PSRAM 后，
 * 池里只剩 ①+② ≈ 470KB，512KB 的池就装得下了。 */
static void * img_buf_malloc(size_t size, lv_color_format_t color_format)
{
    LV_UNUSED(color_format);
    void * p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    if (p == NULL) {
        /* 万一 PSRAM 也不够，退回 LVGL 池，别比原来更糟 */
        ESP_LOGW(TAG, "图片缓冲 %u 字节 PSRAM 分配失败（空闲 %u，最大块 %u），退回 LVGL 池",
                 (unsigned)size,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
        return lv_malloc(size);
    }
    ESP_LOGI(TAG, "图片缓冲 %u 字节 → PSRAM @ %p", (unsigned)size, p);
    return p;
}

static void img_buf_free(void * buf)
{
    /* heap_caps_free 能按指针找到所属堆，退回池里分配的那块也能正常释放 */
    heap_caps_free(buf);
}

/* esp_timer 周期回调：每 1ms 给 LVGL 喂一次 tick */
static void lv_tick_timer_cb(void *arg)
{
    (void)arg;
    lv_tick_inc(1);
}

void lvgl_port_init(void)
{
    LCD_Init();          // 必须先初始化 SPI 与 LCD，否则后续 flush 的 spi_handle 无效
    lv_init();

    /* SD 卡文件接入 LVGL：'S' 盘（= /sdcard，见 sdkconfig 的 LV_USE_FS_POSIX /
     * LV_FS_POSIX_LETTER / LV_FS_POSIX_PATH）。之后 lv_image_set_src(img, "S:/dir/a.jpg")
     * 就能直接解码显示 SD 卡里的图。
     * 注意：lv_init() 内部已经调用过 lv_fs_posix_init()（见 lv_init.c），
     * 这里不能再注册一次，否则同一个盘符会被注册两遍。 */

    /* 给 LVGL 追加内存池：优先 PSRAM，失败则退回内部 DRAM，并逐级降容量重试。
     * 必须在 lv_init() 之后调用。 */
    size_t added = lvgl_add_mem_pool(MALLOC_CAP_SPIRAM, "PSRAM");
    if (added == 0) {
        ESP_LOGW(TAG, "PSRAM pool unavailable, falling back to internal DRAM");
        added = lvgl_add_mem_pool(MALLOC_CAP_INTERNAL, "INTERNAL");
    }

    /* 自验证：打印挂池后 LVGL 的实际总内存，确认是否真的生效 */
    lv_mem_monitor_t mon;
    lv_mem_monitor(&mon);
    ESP_LOGI(TAG, "LVGL total memory = %u bytes (extra pool added = %u)",
             (unsigned)mon.total_size, (unsigned)added);

    /* 图片解码结果（整帧 ARGB8888）改用系统 PSRAM，别挤 LVGL 池 */
    lv_draw_buf_handlers_t * img_hd = lv_draw_buf_get_image_handlers();
    if (img_hd) {
        img_hd->buf_malloc_cb = img_buf_malloc;
        img_hd->buf_free_cb   = img_buf_free;
        ESP_LOGI(TAG, "图片解码帧改走系统 PSRAM（LVGL 池只留文件+扫描线）");
    }

    /* 分配两个内部 RAM draw buf (PARTIAL 模式)。
     * 优先 MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA, 满足 GDMA 对源缓冲的可访问性要求。 */
    s_draw_buf1 = heap_caps_malloc(DISP_BUF_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    s_draw_buf2 = heap_caps_malloc(DISP_BUF_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (!s_draw_buf1 || !s_draw_buf2) {
        ESP_LOGE(TAG, "internal RAM draw buf alloc failed (need %u bytes each)",
                 (unsigned)DISP_BUF_SIZE);
        return;
    }
    ESP_LOGI(TAG, "draw_buf1 @ %p, draw_buf2 @ %p, each %u bytes (internal RAM)",
             s_draw_buf1, s_draw_buf2, (unsigned)DISP_BUF_SIZE);

    /* 注册显示驱动: PARTIAL 模式 + 双 draw buf。
     * LVGL 只渲染脏区到 draw buf, flush_cb 把这块 DMA 到屏幕对应坐标。 */
    s_disp = lv_display_create(LCD_W, LCD_H);
    lv_display_set_buffers(s_disp, s_draw_buf1, s_draw_buf2,
                           DISP_BUF_SIZE, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(s_disp, disp_flush_cb);
    lv_display_set_rotation(s_disp, LV_DISP_ROTATION_0);

    /* 注册 DMA 完成中断回调, 由它异步通知 LVGL 渲染下一块。
     * PARTIAL 模式下每块脏区 DMA 完成都触发此回调。 */
    lcd_set_color_trans_done_cb(on_color_trans_done_cb, NULL);

    /* 启动 1ms 周期 tick 定时器 */
    const esp_timer_create_args_t timer_args = {
        .callback = lv_tick_timer_cb,
        .name = "lv_tick",
    };
    esp_timer_handle_t tick_timer = NULL;
    esp_timer_create(&timer_args, &tick_timer);
    esp_timer_start_periodic(tick_timer, 1000);   // 1000us = 1ms
}
