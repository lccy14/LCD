#include "lvgl_port.h"
#include "lcd.h"
#include "lcd_init.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* LVGL 的 draw buffer 直接复用 lcd.c 的 g_framebuffer，零拷贝 */
static lv_color_t *g_lv_fb = (lv_color_t *)g_framebuffer;

/* flush 回调：LVGL 渲染完一帧后调用，我们把整块缓冲刷到屏幕 */
static void disp_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    (void)area;
    (void)px_map;

    /* 字节序修正：
     * LVGL 以小端 uint16_t 写入每个像素（如红色 0xF800 存为字节 [0x00,0xF8]），
     * 而 LCD_Writ_Buf 逐字节原样发送，ST7789 收到 [0x00,0xF8] 即 0x00F8 = 蓝色。
     * 非 LVGL 模式用 [hi,lo] 存成大端，故这里在发送前把每对字节翻转一次，
     * 使内存变为 [0xF8,0x00]，匹配 ST7789 期望的高字节在前序。
     * 注意 g_framebuffer 是 u8 数组，需按字节成对交换（不能用 uint16_t 访问，
     * 否则越界且只写单字节）。 */
    uint32_t byte_count = sizeof(g_framebuffer);   /* 总字节数 = 像素数 * 2 */
    for (uint32_t i = 0; i + 1 < byte_count; i += 2) {
        u8 tmp = g_framebuffer[i];
        g_framebuffer[i]     = g_framebuffer[i + 1];
        g_framebuffer[i + 1] = tmp;
    }

    LCD_Flush_All();
    lv_disp_flush_ready(disp);
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

    /* 注册显示驱动，draw buffer 直接指向 g_framebuffer（零拷贝） */
    lv_display_t *disp = lv_display_create(LCD_W, LCD_H);
    lv_display_set_buffers(disp, g_lv_fb, NULL, sizeof(g_framebuffer), LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(disp, disp_flush_cb);
    lv_display_set_rotation(disp, LV_DISP_ROTATION_0);

    /* 启动 1ms 周期 tick 定时器 */
    const esp_timer_create_args_t timer_args = {
        .callback = lv_tick_timer_cb,
        .name = "lv_tick",
    };
    esp_timer_handle_t tick_timer = NULL;
    esp_timer_create(&timer_args, &tick_timer);
    esp_timer_start_periodic(tick_timer, 1000);   // 1000us = 1ms
}
