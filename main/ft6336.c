#include "ft6336.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>

static const char *TAG = "FT6336";

static i2c_master_bus_handle_t s_bus = NULL;
static i2c_master_dev_handle_t s_dev = NULL;

/* I2C 超时：不能用 -1（无限等待）。
 * 触摸读取跑在 LVGL 任务里，一旦总线异常/设备掉线，无限等待会直接冻死整个 UI 与触摸。
 * 限时失败后本次读取作废，下个周期重试，最坏情况只是丢一帧坐标。 */
#define FT6336_I2C_TIMEOUT pdMS_TO_TICKS(20)

/* 向 FT6336 写一个寄存器（用于读之前设置寄存器地址） */
static esp_err_t ft6336_write_reg(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(s_dev, buf, sizeof(buf), FT6336_I2C_TIMEOUT);
}

/* 从 FT6336 读 len 个字节，起始寄存器为 reg */
static esp_err_t ft6336_read_reg(uint8_t reg, uint8_t *buf, size_t len)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, buf, len, FT6336_I2C_TIMEOUT);
}

/* 初始化失败时扫一遍 I2C 总线，把有 ACK 的地址都打出来。
 * 用来区分三类问题：一个设备都没有(供电/接线/上拉)、地址不同(换了触摸 IC)、
 * 地址对但时序不对。只在失败路径执行一次，不影响正常运行。 */
static void ft6336_scan_bus(void)
{
    char list[160] = {0};
    size_t used = 0;
    for (uint8_t a = 0x08; a <= 0x77; a++) {
        if (i2c_master_probe(s_bus, a, 5) == ESP_OK) {
            int n = snprintf(list + used, sizeof(list) - used, " 0x%02X", a);
            if (n < 0 || (size_t)n >= sizeof(list) - used) break;
            used += (size_t)n;
        }
    }
    ESP_LOGE(TAG, "I2C 扫描 SCL=GPIO%d SDA=GPIO%d 命中:%s",
             (int)FT6336_SCL_GPIO, (int)FT6336_SDA_GPIO,
             used ? list : " (无设备 —— 检查供电/接线/上拉)");
}

esp_err_t ft6336_init(void)
{
    /* 1. INT 必须先配置好，再释放复位。
     *    部分 FT6x36 在 RST 上升沿采样 INT 电平来选择 I2C 地址(0x38/0x39)，
     *    若先复位再配 INT，地址可能落到备选值上。 */
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << FT6336_INT_GPIO),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

#ifdef FT6336_RST_GPIO
    /* 2. 第二版 PCB 把复位脚接到了 MCU（TP_RES）。FT6336 低电平复位，
     *    拉低 10ms 再拉高释放。第一版 RST 直连 3V3，无此宏则跳过。
     *    ⚠️ 拉高后芯片要跑内部固件，通常 100~300ms 才回 I2C ACK；
     *       绝不能只等 20ms 就读 chip_id（之前识别不到就是这个原因）。
     *       下面用 probe 轮询等就绪，芯片一 ACK 就继续，不空等固定时长。 */
    gpio_config_t rst_conf = {
        .pin_bit_mask = (1ULL << FT6336_RST_GPIO),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&rst_conf);
    gpio_set_level(FT6336_RST_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(FT6336_RST_GPIO, 1);
#endif

    /* 3. 创建 I2C 主机（ESP-IDF v6 新 API） */
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

    /* 4. 轮询等待就绪，并自动识别 0x38 / 0x39。
     *    probe 只发地址不读寄存器，最适合判断"芯片活没活"。
     *    15 次 × 30ms ≈ 450ms 窗口，覆盖复位后的启动时间。 */
    const uint16_t cand[] = { FT6336_I2C_ADDR, FT6336_I2C_ADDR_ALT };
    uint16_t addr = 0;
    for (int i = 0; i < 15 && !addr; i++) {
        for (size_t k = 0; k < sizeof(cand) / sizeof(cand[0]); k++) {
            if (i2c_master_probe(s_bus, cand[k], 20) == ESP_OK) {
                addr = cand[k];
                break;
            }
        }
        if (!addr) vTaskDelay(pdMS_TO_TICKS(30));
    }
    if (!addr) {
        ESP_LOGE(TAG, "复位后 450ms 内 I2C 无应答 (RST/供电/接线?)");
        ft6336_scan_bus();
        return ESP_ERR_NOT_FOUND;
    }

    /* 5. 用实际探测到的地址注册设备 */
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = addr,
        /* FT6336 支持 400kHz 快速模式；100kHz 时单次读 5 字节约 0.9ms，
         * 提到 400kHz 后约 0.25ms，直接省掉每帧 0.65ms 的 I2C 时间。 */
        .scl_speed_hz    = 400000,
    };
    ret = i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "add device 0x%02X failed: %d", addr, ret);
        return ESP_ERR_NOT_FOUND;
    }

    /* 6. 读 chip_id 也留 3 次重试，兼容固件刚起来时偶发 NACK */
    uint8_t id[2] = {0};
    bool id_ok = false;
    for (int i = 0; i < 3 && !id_ok; i++) {
        id_ok = (i2c_master_transmit_receive(s_dev, (uint8_t[]){0xA8}, 1, id, 2,
                                             FT6336_I2C_TIMEOUT) == ESP_OK);
        if (!id_ok) vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (!id_ok) {
        ESP_LOGE(TAG, "FT6336 read chip_id failed at 0x%02X", addr);
        ft6336_scan_bus();
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "FT6336 found at addr 0x%02X, chip_id=%02X%02X",
             addr, id[0], id[1]);

    /* 7. 软复位（进入正常模式） */
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

    /* 用 D 级别：I 级别会在手指按住期间每秒刷 30 条串口日志，拖慢主循环 */
    ESP_LOGD(TAG, "touch detected x=%d y=%d", x, y);

    tp->pressed = true;
    tp->x = x;
    tp->y = y;
    return ESP_OK;
}

/* 查 INT 引脚电平, 无触摸时高电平, 触摸中低电平(FT6336 默认配置)。
 * touch_read_cb 用它快速过滤, 无触摸时不读 I2C, 滑动响应延迟从 ~1ms 降到 ~1µs。 */
bool ft6336_touched(void)
{
    return gpio_get_level(FT6336_INT_GPIO) == 0;
}
