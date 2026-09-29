#include "sd_card.h"

#include <stdio.h>
#include <string.h>
#include <sys/statvfs.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "SD";

/* SDMMC 的每一次命令传输都要临时申请一小块 DMA 内存（内部 SRAM）。
 * 若此时内部 RAM 被吃光，就会报 sdmmc_send_cmd_send_scr: not enough mem (0x101)，
 * 连 8 字节都申请不到。打印各区域余量，便于定位到底是哪块内存不够。 */
static void sd_log_heap(const char *when)
{
    ESP_LOGI(TAG, "[%s] 内存: 内部 free=%u/largest=%u  DMA free=%u/largest=%u  PSRAM free=%u",
             when,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

sd_card_info_t g_sdcard = {0};

static sdmmc_card_t *s_card = NULL;

/* 按指定总线宽度初始化 SDMMC 主机并挂载 FATFS。
 * 1 线模式把 D1~D3 显式设成 NC，避免误用默认引脚。 */
static esp_err_t mount_with_width(int width, int freq_khz)
{
    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,   /* 绝不格式化用户的数据 */
        .max_files              = 5,
        .allocation_unit_size   = 16 * 1024,
        .disk_status_check_enable = false,
        .use_one_fat            = false,
    };

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = freq_khz;

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = width;
    slot.clk   = (gpio_num_t)SDMMC_PIN_CLK;
    slot.cmd   = (gpio_num_t)SDMMC_PIN_CMD;
    slot.d0    = (gpio_num_t)SDMMC_PIN_D0;
    if (width >= 4) {
        slot.d1 = (gpio_num_t)SDMMC_PIN_D1;
        slot.d2 = (gpio_num_t)SDMMC_PIN_D2;
        slot.d3 = (gpio_num_t)SDMMC_PIN_D3;
    } else {
        slot.d1 = GPIO_NUM_NC;
        slot.d2 = GPIO_NUM_NC;
        slot.d3 = GPIO_NUM_NC;
    }
    slot.cd = SDMMC_SLOT_NO_CD;      /* 卡座无 CD/WP 检测脚 */
    slot.wp = SDMMC_SLOT_NO_WP;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;   /* 外部没上拉时先靠内部上拉顶一下 */

    sdmmc_card_t *card = NULL;
    esp_err_t err = esp_vfs_fat_sdmmc_mount(SD_MOUNT_POINT, &host, &slot, &mount_cfg, &card);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%d 线 %d kHz 挂载失败: %s", width, freq_khz, esp_err_to_name(err));
        return err;
    }

    s_card = card;
    g_sdcard.mounted        = true;
    g_sdcard.width          = width;
    g_sdcard.freq_khz       = freq_khz;
    g_sdcard.sector_count   = card->csd.capacity;
    g_sdcard.capacity_bytes = ((uint64_t)card->csd.capacity) * card->csd.sector_size;
    /* cid.name 是 char[8]，可能没有结尾的 '\0'，用 %.8s 限制长度避免读越界 */
    snprintf(g_sdcard.card_name, sizeof(g_sdcard.card_name), "%.8s", (const char *)card->cid.name);

    ESP_LOGI(TAG, "已挂载 %s: %s, %d 线 %d kHz, %llu MB, %u 扇区",
             SD_MOUNT_POINT, g_sdcard.card_name, width, freq_khz,
             (unsigned long long)(g_sdcard.capacity_bytes / (1024 * 1024)),
             (unsigned)g_sdcard.sector_count);
    return ESP_OK;
}

/* 按指定宽度挂载，NO_MEM 时退避重试几次（其它任务可能正在占用/刚释放内存）。
 * 其余错误（无卡、接线、不是 FAT32）重试没有意义，直接返回。 */
#define SD_MOUNT_RETRY   3
#define SD_RETRY_DELAY_MS 300

static esp_err_t mount_with_retry(int width, int freq_khz)
{
    esp_err_t err = ESP_FAIL;
    for (int i = 0; i < SD_MOUNT_RETRY; i++) {
        err = mount_with_width(width, freq_khz);
        if (err == ESP_OK || err != ESP_ERR_NO_MEM) {
            return err;
        }
        ESP_LOGW(TAG, "%d 线模式内存不足，%d ms 后重试 (%d/%d)",
                 width, SD_RETRY_DELAY_MS, i + 1, SD_MOUNT_RETRY);
        vTaskDelay(pdMS_TO_TICKS(SD_RETRY_DELAY_MS));
    }
    return err;
}

/* 挂载尝试顺序（(宽度, 时钟)）：
 *   1) 4 线 10MHz   —— 最快
 *   2) 4 线 4MHz    —— ESP32-S3 的 SDMMC 全部走 GPIO 矩阵，没有 IO MUX 专用脚，
 *                      D0~D3 的走线延时不一致时 4 线会 CRC 失败（err 0x109），
 *                      降速通常就能过（驱动也会打印 input line delay not supported）
 *   3) 1 线 10MHz   —— 只留 D0，速度慢但对走线最宽容
 */
typedef struct { int width; int freq_khz; } sd_try_t;
static const sd_try_t s_try_list[] = {
    { 4, SD_FREQ_KHZ },
    { 4, 4000 },
    { 1, SD_FREQ_KHZ },
};

/* 4 线全部失败后的「体检」：用 400kHz（SD 初始化速率，几乎不受走线影响）探一次。
 *   成功 → D1~D3 是接通的，只是高速下信号质量差（缺 10k 上拉 / 走线不等长）；
 *   失败 → D1/D2/D3(GPIO48/1/2) 大概率没接或接错，只能一直用 1 线。
 * 探测成功后立刻卸载，不留在 400kHz（比 1 线 10MHz 还慢得多）。 */
static void probe_4bit_lowspeed(void)
{
    if (mount_with_width(4, SDMMC_FREQ_PROBING) == ESP_OK) {
        ESP_LOGW(TAG, "体检: 4 线在 %d kHz 能通 → D1~D3 已接通，是高速信号质量问题；"
                      "给 CMD/D0~D3 各加 10k 上拉、走线尽量等长后可跑 4 线",
                 SDMMC_FREQ_PROBING);
        sd_card_unmount();
    } else {
        ESP_LOGW(TAG, "体检: 4 线在 %d kHz 也不通 → 检查 D1/D2/D3 = GPIO%d/%d/%d 是否接错、虚焊或未接",
                 SDMMC_FREQ_PROBING, SDMMC_PIN_D1, SDMMC_PIN_D2, SDMMC_PIN_D3);
    }
}

esp_err_t sd_card_mount(bool width4)
{
    if (g_sdcard.mounted) {
        return ESP_OK;
    }

    sd_log_heap("挂载前");

    esp_err_t err = ESP_FAIL;
    for (size_t i = 0; i < sizeof(s_try_list) / sizeof(s_try_list[0]); i++) {
        int w = s_try_list[i].width;
        int f = s_try_list[i].freq_khz;
        if (w == 4 && !width4) {
            continue;                       /* 调用方只要 1 线 */
        }
        err = mount_with_retry(w, f);
        if (err == ESP_OK) {
            return ESP_OK;
        }
        if (w == 4 && err == ESP_ERR_INVALID_CRC) {
            ESP_LOGW(TAG, "4 线 %d kHz 数据 CRC 错误：D1~D3 走线/上拉不良，尝试降速", f);
        }
        /* 4 线档位全部试完仍失败（即将退回 1 线）时，做一次 400kHz 体检 */
        if (w == 4 && f == 4000 && width4) {
            probe_4bit_lowspeed();
        }
    }

    if (err != ESP_OK) {
        memset(&g_sdcard, 0, sizeof(g_sdcard));
        ESP_LOGE(TAG, "SD 卡挂载失败: %s", esp_err_to_name(err));
        if (err == ESP_ERR_NO_MEM) {
            ESP_LOGE(TAG, "内部 RAM(DMA) 不足：请改在开机时挂载，或减小 LVGL 内部内存池");
        } else {
            ESP_LOGE(TAG, "请检查接线/上拉电阻/卡是否为 FAT32");
        }
        sd_log_heap("挂载失败后");
    }
    return err;
}

void sd_card_unmount(void)
{
    if (!g_sdcard.mounted) {
        return;
    }
    esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, s_card);
    s_card = NULL;
    memset(&g_sdcard, 0, sizeof(g_sdcard));
    ESP_LOGI(TAG, "已卸载 %s", SD_MOUNT_POINT);
}

bool sd_card_is_mounted(void)
{
    return g_sdcard.mounted;
}

bool sd_card_get_space(uint64_t *total, uint64_t *free_bytes)
{
    if (!g_sdcard.mounted) {
        return false;
    }
    struct statvfs st;
    if (statvfs(SD_MOUNT_POINT, &st) != 0) {
        return false;
    }
    uint64_t unit = (uint64_t)st.f_frsize;
    if (unit == 0) {
        unit = (uint64_t)st.f_bsize;
    }
    if (total)      *total      = unit * (uint64_t)st.f_blocks;
    if (free_bytes) *free_bytes = unit * (uint64_t)st.f_bfree;
    return true;
}
