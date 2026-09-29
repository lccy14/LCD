#include "lcd_init.h"
#include "delay.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_io_i80.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "LCD_INIT";

/* 内部句柄 */
static esp_lcd_panel_io_handle_t s_lcd_io = NULL;

/* 每个 I80 颜色事务 DMA 完成时 give 一次。视频播放直写 g_framebuffer 后
 * 必须等它，否则下一帧解码会和 DMA 抢同一块缓冲（队列深度 6 可攒多个事务）。
 * LVGL 路径不用它（走自己的 flush_ready 回调），多 give 无副作用。 */
static SemaphoreHandle_t s_trans_done_sem = NULL;

/* 返回 true=等到了一次 DMA 完成；false=超时无完成（队列已空） */
bool LCD_WaitFlushDone(uint32_t timeout_ms)
{
    if (!s_trans_done_sem) return false;
    return xSemaphoreTake(s_trans_done_sem, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

/* 取走所有已完成的通知（开始视频播放前清空历史事务计数用） */
void LCD_DrainFlushDone(void)
{
    if (s_trans_done_sem) {
        while (xSemaphoreTake(s_trans_done_sem, 0) == pdTRUE) { }
    }
}

/* 全局句柄，供 lcd.c / lvgl_port.c 使用 esp_lcd_panel_draw_bitmap */
esp_lcd_panel_handle_t g_lcd_panel = NULL;

/* I80 颜色传输完成回调（由 lvgl_port.c 注册，触发 lv_disp_flush_ready）
 * 设置为 NULL 时表示无回调。回调在中断上下文执行。 */
typedef bool (*lcd_color_trans_done_cb_t)(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t *, void *);
static lcd_color_trans_done_cb_t s_color_trans_done_cb = NULL;
static void *s_color_trans_done_ctx = NULL;

void lcd_set_color_trans_done_cb(bool (*cb)(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t *, void *), void *ctx)
{
    s_color_trans_done_cb = cb;
    s_color_trans_done_ctx = ctx;
}

/* I80 驱动的 on_color_trans_done 入口：通知等待者（视频）并转给上层（LVGL） */
static bool lcd_on_color_trans_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
    (void)io; (void)edata; (void)user_ctx;
    if (s_trans_done_sem) {
        BaseType_t higher_priority_task_woken = pdFALSE;
        /* 计数满（等待者不积极）时忽略即可 */
        (void)xSemaphoreGiveFromISR(s_trans_done_sem, &higher_priority_task_woken);
        if (higher_priority_task_woken == pdTRUE) {
            portYIELD_FROM_ISR();
        }
    }
    if (s_color_trans_done_cb) {
        return s_color_trans_done_cb(io, edata, s_color_trans_done_ctx);
    }
    return false;
}

/******************************************************************************
      函数说明：LCD GPIO + I80 并行总线初始化 (ESP-IDF LCD_CAM 外设 + DMA)
******************************************************************************/
void LCD_GPIO_Init(void)
{
    /* DMA 完成计数信号量：最大计数 >= I80 事务队列深度(6)+1 */
    if (!s_trans_done_sem) {
        s_trans_done_sem = xSemaphoreCreateCounting(8, 0);
    }

    /* 1. RST / BLK 作为普通 GPIO 输出 */
    gpio_config_t io_conf = {};
    io_conf.intr_type    = GPIO_INTR_DISABLE;
    io_conf.mode         = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask = (1ULL << LCD_RST_GPIO) |
                           (1ULL << LCD_BLK_GPIO);
    io_conf.pull_down_en = 0;
    io_conf.pull_up_en   = 0;
    gpio_config(&io_conf);

    LCD_RST_Set();
    LCD_BLK_Set();

    /* 2. 创建 I80 并行总线（8bit 数据 + DC/WR 控制；CS 由 Panel IO 管） */
    esp_lcd_i80_bus_handle_t i80_bus = NULL;
    esp_lcd_i80_bus_config_t bus_cfg = {
        .clk_src            = LCD_CLK_SRC_DEFAULT,
        .dc_gpio_num        = LCD_DC_GPIO,
        .wr_gpio_num        = LCD_WR_GPIO,
        .data_gpio_nums     = {
            LCD_D0_GPIO, LCD_D1_GPIO, LCD_D2_GPIO, LCD_D3_GPIO,
            LCD_D4_GPIO, LCD_D5_GPIO, LCD_D6_GPIO, LCD_D7_GPIO,
        },
        .bus_width          = 8,
        .max_transfer_bytes = (size_t)LCD_W * LCD_H * 2,
        .dma_burst_size     = 64,                 /* 必须 > 0，否则 DMA 链表池不分配 */
    };
    ESP_ERROR_CHECK(esp_lcd_new_i80_bus(&bus_cfg, &i80_bus));

    /* 3. 在 I80 总线上创建 Panel IO */
    esp_lcd_panel_io_i80_config_t io_cfg = {
        .cs_gpio_num           = LCD_CS_GPIO,         /* CS 由驱动控制 */
        .pclk_hz               = 15 * 1000 * 1000,     /* 15MHz：ST7789 8080 上限。花屏根因(DMA 异步通知)已修，可从 10MHz 提到 15MHz */
        .trans_queue_depth     = 6,                    /* 事务队列深度（测试代码用 4~10） */
        .lcd_cmd_bits          = 8,
        .lcd_param_bits        = 8,
        .dc_levels             = {
            .dc_idle_level  = 0,
            .dc_cmd_level   = 0,                       /* DC=0 表示命令 */
            .dc_dummy_level = 0,
            .dc_data_level  = 1,                       /* DC=1 表示数据 */
        },
        .on_color_trans_done  = lcd_on_color_trans_done,  /* DMA 完成时通知 LVGL */
        .user_ctx             = NULL,                  /* ctx 通过 s_color_trans_done_ctx 传 */
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i80(i80_bus, &io_cfg, &s_lcd_io));

    /* 4. 创建 ST7789 Panel */
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = LCD_RST_GPIO,
        .rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_RGB,
        .data_endian    = LCD_RGB_DATA_ENDIAN_LITTLE,  /* ST7789 用小端，LVGL 小端写入直接发，无需翻转 */
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(s_lcd_io, &panel_cfg, &g_lcd_panel));

    /* 5. 硬件复位 + 出厂默认参数初始化 */
    ESP_ERROR_CHECK(esp_lcd_panel_reset(g_lcd_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(g_lcd_panel));
    /* IDF 6.0 的 panel_init 不发 0x29 (DISPON)，必须单独调用才会发，否则屏幕黑屏 */
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(g_lcd_panel, true));

    /* 6. 设置旋转方向（USE_HORIZONTAL 决定横屏/竖屏） */
    if (USE_HORIZONTAL == 0) {
        esp_lcd_panel_mirror(g_lcd_panel, false, false);
        esp_lcd_panel_swap_xy(g_lcd_panel, false);
    } else if (USE_HORIZONTAL == 1) {
        esp_lcd_panel_mirror(g_lcd_panel, true, true);
        esp_lcd_panel_swap_xy(g_lcd_panel, false);
    } else if (USE_HORIZONTAL == 2) {
        esp_lcd_panel_mirror(g_lcd_panel, true, false);
        esp_lcd_panel_swap_xy(g_lcd_panel, true);
    } else {
        esp_lcd_panel_mirror(g_lcd_panel, false, true);
        esp_lcd_panel_swap_xy(g_lcd_panel, true);
    }
}


/******************************************************************************
      底层字节发送（供 LCD_WR_REG / LCD_WR_DATA8 / LCD_Writ_Buf 调用）
******************************************************************************/

/* 写一个命令字节（DC=0） */
void LCD_WR_REG(u8 dat)
{
    esp_lcd_panel_io_tx_param(s_lcd_io, dat, NULL, 0);
}

/* 写一个字节数据（DC=1，用 tx_param 同步发，避免 tx_color 异步导致栈 buffer 失效） */
void LCD_WR_DATA8(u8 dat)
{
    esp_lcd_panel_io_tx_param(s_lcd_io, -1, &dat, 1);
}

/* 写两个字节数据（16bit 颜色，大端高字节在前） */
void LCD_WR_DATA(u16 dat)
{
    u8 buf[2] = {(u8)(dat >> 8), (u8)(dat & 0xFF)};
    esp_lcd_panel_io_tx_param(s_lcd_io, -1, buf, 2);
}

/* 单字节写（调试用，DC=1） */
void LCD_Writ_Bus(u8 dat)
{
    esp_lcd_panel_io_tx_param(s_lcd_io, -1, &dat, 1);
}

/* 批量 DMA 写（DC=1，不发命令——0x2C 已由 LCD_Address_Set 发送）
 * 注：批量数据走 tx_color 才会用 DMA，tx_param 是同步逐字节发，慢且占用 CPU。
 *      这里仍用 tx_color，但调用者必须保证 data 缓冲在事务完成前一直有效。 */
void LCD_Writ_Buf(const u8 *data, u32 len)
{
    if (len == 0) return;
    esp_lcd_panel_io_tx_color(s_lcd_io, -1, data, len);
}


/******************************************************************************
      函数说明：设置起始和结束地址
      手动发 0x2A(列) + 0x2B(行) + 0x2C(进入数据写入模式)
******************************************************************************/
void LCD_Address_Set(u16 x1, u16 y1, u16 x2, u16 y2)
{
    /* 列地址 */
    u8 col[4] = {(u8)(x1 >> 8), (u8)(x1 & 0xFF), (u8)(x2 >> 8), (u8)(x2 & 0xFF)};
    esp_lcd_panel_io_tx_param(s_lcd_io, 0x2A, col, 4);

    /* 行地址 */
    u8 row[4] = {(u8)(y1 >> 8), (u8)(y1 & 0xFF), (u8)(y2 >> 8), (u8)(y2 & 0xFF)};
    esp_lcd_panel_io_tx_param(s_lcd_io, 0x2B, row, 4);

    /* 0x2C 进入数据写入模式（不带参数） */
    esp_lcd_panel_io_tx_param(s_lcd_io, 0x2C, NULL, 0);
}


/******************************************************************************
      函数说明：LCD 初始化
      esp_lcd_panel_init() 已发送 ST7789 默认序列（0x01/0x11/0x3A/0x36/0x29），
      这里追加项目特有的 gamma / 电源调校命令。
******************************************************************************/
void LCD_Init(void)
{
    LCD_GPIO_Init();

    LCD_WR_REG(0xB2);
    LCD_WR_DATA8(0x0C);
    LCD_WR_DATA8(0x0C);
    LCD_WR_DATA8(0x00);
    LCD_WR_DATA8(0x33);
    LCD_WR_DATA8(0x33);

    LCD_WR_REG(0xB7);
    LCD_WR_DATA8(0x35);

    LCD_WR_REG(0xBB);
    LCD_WR_DATA8(0x19);

    LCD_WR_REG(0xC0);
    LCD_WR_DATA8(0x2C);

    LCD_WR_REG(0xC2);
    LCD_WR_DATA8(0x01);

    LCD_WR_REG(0xC3);
    LCD_WR_DATA8(0x12);

    LCD_WR_REG(0xC4);
    LCD_WR_DATA8(0x20);

    LCD_WR_REG(0xC6);
    LCD_WR_DATA8(0x0F);

    LCD_WR_REG(0xD0);
    LCD_WR_DATA8(0xA4);
    LCD_WR_DATA8(0xA1);

    LCD_WR_REG(0xE0);
    LCD_WR_DATA8(0xD0);
    LCD_WR_DATA8(0x04);
    LCD_WR_DATA8(0x0D);
    LCD_WR_DATA8(0x11);
    LCD_WR_DATA8(0x13);
    LCD_WR_DATA8(0x2B);
    LCD_WR_DATA8(0x3F);
    LCD_WR_DATA8(0x54);
    LCD_WR_DATA8(0x4C);
    LCD_WR_DATA8(0x18);
    LCD_WR_DATA8(0x0D);
    LCD_WR_DATA8(0x0B);
    LCD_WR_DATA8(0x1F);
    LCD_WR_DATA8(0x23);

    LCD_WR_REG(0xE1);
    LCD_WR_DATA8(0xD0);
    LCD_WR_DATA8(0x04);
    LCD_WR_DATA8(0x0C);
    LCD_WR_DATA8(0x11);
    LCD_WR_DATA8(0x13);
    LCD_WR_DATA8(0x2C);
    LCD_WR_DATA8(0x3F);
    LCD_WR_DATA8(0x44);
    LCD_WR_DATA8(0x51);
    LCD_WR_DATA8(0x2F);
    LCD_WR_DATA8(0x1F);
    LCD_WR_DATA8(0x1F);
    LCD_WR_DATA8(0x20);
    LCD_WR_DATA8(0x23);

    LCD_WR_REG(0x21);   /* 显示反转开 */

    ESP_LOGI(TAG, "LCD init done (parallel I80 @ 15MHz)");
}
