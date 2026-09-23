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

static const char *TAG = "LVGL_PORT";

/* ui.c 创建的 LVGL 主任务句柄, DMA 完成时通过 xTaskNotifyGive 唤醒它,
 * 替代 vTaskDelay 固定 10ms 等待, 滑动 FPS 46→80+。 */
extern TaskHandle_t g_lvgl_task;

/* 额外给 LVGL 挂的 PSRAM 内存池总大小（KB）。
 * 内置的 LV_MEM（sdkconfig: CONFIG_LV_MEM_SIZE_KILOBYTES，当前 64KB）装不下所有
 * App 屏幕的常驻对象——实测开 3~4 个 App 就用到 99%，maxfree 只剩 608 字节，
 * 之后 lv_malloc 失败会触发断言死循环，进而 task_wdt 复位。
 *
 * 注意：LVGL 的 TLSF 对「单个」池有大小上限（见 lv_tlsf.c）：
 *     #define TLSF_MAX_POOL_SIZE (LV_MEM_SIZE + LV_MEM_POOL_EXPAND_SIZE)
 *     block_size_max = 1 << log2_ceil(TLSF_MAX_POOL_SIZE)
 * 当前 LV_MEM_SIZE=64KB 且 LV_MEM_POOL_EXPAND_SIZE=0，单池上限就是 64KB，
 * 一次性挂 128KB / 256KB 会被 lv_mem_add_pool() 拒绝（已实测确认）。
 * 因此按 LV_POOL_CHUNK_KB 一块分批挂载，累计到 LV_POOL_TOTAL_KB。 */
#define LV_POOL_TOTAL_KB   256
#define LV_POOL_CHUNK_KB   64

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
 * TLSF 单池有 64KB 上限，所以按 LV_POOL_CHUNK_KB 分块循环挂载，
 * 直到累计到 LV_POOL_TOTAL_KB；返回实际挂上的总字节数（可能小于目标）。
 * 注意：LVGL 内部日志(LV_USE_LOG)默认关闭，add_pool 失败时其 printf 也看不到，
 * 所以这里每一步都用 ESP_LOG 打点，便于确认卡在哪一步。 */
static size_t lvgl_add_mem_pool(uint32_t cap, const char *cap_name)
{
    const size_t target = (size_t)LV_POOL_TOTAL_KB * 1024;
    const size_t chunk  = (size_t)LV_POOL_CHUNK_KB * 1024;
    size_t added = 0;

    while (added < target) {
        void *p = heap_caps_malloc(chunk, cap);
        if (p == NULL) {
            ESP_LOGW(TAG, "%s: malloc %uKB chunk failed (added %uKB so far)",
                     cap_name, (unsigned)LV_POOL_CHUNK_KB, (unsigned)(added / 1024));
            break;
        }
        if (lv_mem_add_pool(p, chunk) == NULL) {
            ESP_LOGE(TAG, "%s: lv_mem_add_pool(%uKB @ %p) failed",
                     cap_name, (unsigned)LV_POOL_CHUNK_KB, p);
            heap_caps_free(p);
            break;
        }
        added += chunk;
        ESP_LOGI(TAG, "%s: +%uKB pool @ %p (total %uKB)",
                 cap_name, (unsigned)LV_POOL_CHUNK_KB, p, (unsigned)(added / 1024));
    }
    return added;
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
