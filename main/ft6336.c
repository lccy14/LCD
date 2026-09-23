#include "ft6336.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "FT6336";

static i2c_master_bus_handle_t s_bus = NULL;
static i2c_master_dev_handle_t s_dev = NULL;

/* 向 FT6336 写一个寄存器（用于读之前设置寄存器地址） */
static esp_err_t ft6336_write_reg(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(s_dev, buf, sizeof(buf), -1);
}

/* 从 FT6336 读 len 个字节，起始寄存器为 reg */
static esp_err_t ft6336_read_reg(uint8_t reg, uint8_t *buf, size_t len)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, buf, len, -1);
}

esp_err_t ft6336_init(void)
{
    /* 1. 配置 INT 引脚为输入（FT6336 触摸时拉低；此处轮询，仅配置不接中断） */
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << FT6336_INT_GPIO),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    /* 2. 创建 I2C 主机（ESP-IDF v6 新 API） */
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port       = FT6336_I2C_PORT,
        .sda_io_num     = FT6336_SDA_GPIO,
        .scl_io_num     = FT6336_SCL_GPIO,
        .clk_source     = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t ret = i2c_new_master_bus(&bus_cfg, &s_bus);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2c_new_master_bus failed: %d", ret);
        return ret;
    }

    /* 3. 固定地址 0x38 注册设备 */
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = FT6336_I2C_ADDR,
        .scl_speed_hz    = 100000,
    };
    if (i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev) != ESP_OK) {
        ESP_LOGE(TAG, "add device 0x%02X failed", FT6336_I2C_ADDR);
        return ESP_ERR_NOT_FOUND;
    }
    uint8_t id[2] = {0};
    if (i2c_master_transmit_receive(s_dev, (uint8_t[]){0xA8}, 1, id, 2, -1) == ESP_OK) {
        ESP_LOGI(TAG, "FT6336 found at addr 0x%02X, chip_id=%02X%02X",
                 FT6336_I2C_ADDR, id[0], id[1]);
    } else {
        ESP_LOGE(TAG, "FT6336 read chip_id failed (check wiring/RST/power)");
        return ESP_ERR_NOT_FOUND;
    }

    /* 4. 软复位（进入正常模式） */
    ft6336_write_reg(0xAE, 0x00);
    vTaskDelay(pdMS_TO_TICKS(10));

    ESP_LOGI(TAG, "FT6336 init ok");
    return ESP_OK;
}

esp_err_t ft6336_read(touch_point_t *tp)
{
    if (!tp || !s_dev) {
        return ESP_ERR_INVALID_STATE;
    }
    tp->pressed = false;

    /* 0x02: TD_STATUS（低 4 位 = 当前触摸点数），其后 0x03~0x06 为第一点坐标 */
    uint8_t buf[5] = {0};
    esp_err_t ret = ft6336_read_reg(0x02, buf, sizeof(buf));
    if (ret != ESP_OK) {
        ESP_LOGD(TAG, "read 0x02 fail %d", ret);
        return ret;
    }

    uint8_t points = buf[0] & 0x0F;
    ESP_LOGD(TAG, "TD_STATUS=0x%02X points=%d raw=%02X %02X %02X %02X",
             buf[0], points, buf[1], buf[2], buf[3], buf[4]);

    if (points == 0) {
        return ESP_OK;                      // 无触摸
    }

    /* 第一点坐标寄存器布局：
     *   0x03 X_H (高 4 位有效), 0x04 X_L
     *   0x05 Y_H (高 4 位有效), 0x06 Y_L  */
    uint8_t xh = buf[1];   // 0x03
    uint8_t xl = buf[2];   // 0x04
    uint8_t yh = buf[3];   // 0x05
    uint8_t yl = buf[4];   // 0x06

    uint16_t x = (uint16_t)(((xh & 0x0F) << 8) | xl);
    uint16_t y = (uint16_t)(((yh & 0x0F) << 8) | yl);

    ESP_LOGI(TAG, "touch detected x=%d y=%d", x, y);

    tp->pressed = true;
    tp->x = x;
    tp->y = y;
    return ESP_OK;
}
