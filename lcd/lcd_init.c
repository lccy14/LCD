#include "lcd_init.h"
#include "delay.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static spi_device_handle_t spi_handle;

/******************************************************************************
      函数说明：LCD GPIO + SPI 初始化 (ESP-IDF 硬件 SPI + DMA)
      入口数据：无
      返回值：  无
******************************************************************************/
void LCD_GPIO_Init(void)
{
    // RES / DC / CS / BLK 作为普通 GPIO 输出
    gpio_config_t io_conf = {};
    io_conf.intr_type    = GPIO_INTR_DISABLE;
    io_conf.mode         = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask = (1ULL << LCD_RES_GPIO) |
                           (1ULL << LCD_DC_GPIO)  |
                           (1ULL << LCD_CS_GPIO)  |
                           (1ULL << LCD_BLK_GPIO);
    io_conf.pull_down_en = 0;
    io_conf.pull_up_en   = 0;
    gpio_config(&io_conf);

    gpio_set_level(LCD_RES_GPIO, 1);
    gpio_set_level(LCD_DC_GPIO,  1);
    gpio_set_level(LCD_CS_GPIO,  1);
    gpio_set_level(LCD_BLK_GPIO, 1);

    // SPI 总线配置（SCLK / MOSI 交由控制器管理）
    spi_bus_config_t bus_cfg = {
        .mosi_io_num     = LCD_MOSI_GPIO,
        .miso_io_num     = -1,
        .sclk_io_num     = LCD_SCLK_GPIO,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = LCD_W * 2 + 8,          // 单行 DMA 上限（LCD_Flush 按行发送，无需整帧）
    };
    spi_bus_initialize(SPI3_HOST, &bus_cfg, SPI_DMA_CH_AUTO);

    // LCD 设备配置
    spi_device_interface_config_t dev_cfg = {
        .clock_speed_hz = 60 * 1000 * 1000,   // 实测稳定 60MHz
        .mode           = 0,                    // CPOL=0, CPHA=0
        .spics_io_num   = -1,                   // CS 手动控制
        .queue_size     = 1,
        .flags          = SPI_DEVICE_HALFDUPLEX,
    };
    spi_bus_add_device(SPI3_HOST, &dev_cfg, &spi_handle);
}


/******************************************************************************
      函数说明：LCD SPI 写入一个字节（自动管理 CS）
      入口数据：dat  要写入的数据
      返回值：  无
******************************************************************************/
void LCD_Writ_Bus(u8 dat)
{
    spi_transaction_t t = {
        .length    = 8,
        .tx_buffer = &dat,
    };
    LCD_CS_Clr();
    spi_device_polling_transmit(spi_handle, &t);
    LCD_CS_Set();
}


/******************************************************************************
      批量数据写入（DMA）
      整批只拉一次 CS；内部按 4092 字节分包，单包走 DMA 队列发送。
      相比逐字节 LCD_Writ_Bus，SPI 事务次数大幅下降，是刷屏提速的关键。
******************************************************************************/
void LCD_Writ_Buf(const u8 *data, u32 len)
{
    if (len == 0) return;
    const u32 chunk = 4092;   // SPI DMA 单次传输上限约 4092 字节
    LCD_CS_Clr();
    while (len > 0) {
        u32 send = (len > chunk) ? chunk : len;
        spi_transaction_t t = {
            .length    = send * 8,
            .tx_buffer = data,
        };
        spi_device_transmit(spi_handle, &t);
        data += send;
        len  -= send;
    }
    LCD_CS_Set();
}

/******************************************************************************
      函数说明：LCD写入一个字节数据
******************************************************************************/
void LCD_WR_DATA8(u8 dat)
{
    LCD_Writ_Bus(dat);
}


/******************************************************************************
      函数说明：LCD写入两个字节数据
******************************************************************************/
void LCD_WR_DATA(u16 dat)
{
    LCD_Writ_Bus(dat >> 8);
    LCD_Writ_Bus(dat);
}


/******************************************************************************
      函数说明：LCD写入命令
******************************************************************************/
void LCD_WR_REG(u8 dat)
{
    LCD_DC_Clr();
    LCD_Writ_Bus(dat);
    LCD_DC_Set();
}


/******************************************************************************
      函数说明：设置起始和结束地址
******************************************************************************/
void LCD_Address_Set(u16 x1, u16 y1, u16 x2, u16 y2)
{
    /* 直接映射到 GRAM 原点，覆盖全部可用像素（无偏移） */
    LCD_WR_REG(0x2a);
    LCD_WR_DATA(x1);
    LCD_WR_DATA(x2);
    LCD_WR_REG(0x2b);
    LCD_WR_DATA(y1);
    LCD_WR_DATA(y2);
    LCD_WR_REG(0x2c);
}


/******************************************************************************
      函数说明：LCD 初始化
******************************************************************************/
void LCD_Init(void)
{
    LCD_GPIO_Init();

    LCD_RES_Clr();
    delay_ms(100);
    LCD_RES_Set();
    delay_ms(100);

    LCD_BLK_Set();
    delay_ms(100);

    LCD_WR_REG(0x11);
    delay_ms(120);
    LCD_WR_REG(0x36);
    if (USE_HORIZONTAL == 0) LCD_WR_DATA8(0x00);
    else if (USE_HORIZONTAL == 1) LCD_WR_DATA8(0xC0);
    else if (USE_HORIZONTAL == 2) LCD_WR_DATA8(0x60);
    else LCD_WR_DATA8(0xA0);

    LCD_WR_REG(0x3A);
    LCD_WR_DATA8(0x05);

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

    LCD_WR_REG(0x21);

    LCD_WR_REG(0x29);
}
