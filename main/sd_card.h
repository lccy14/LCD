#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* SD 卡：ESP32-S3 的 SDMMC 主机（非 SPI）。
 * 引脚（见 wiring_diagram.txt）:
 *   CLK GPIO41 — 时钟线，主机输出，决定总线速率
 *   CMD GPIO42 — 命令线，双向
 *   D0  GPIO47 — 数据线 0，1 线模式下唯一的传输线
 *   D1/D2/D3 GPIO48/1/2 — 数据线 1~3，仅 4 线模式启用（带宽 ×4）
 *
 * 1 线模式只用 CLK/CMD/D0；4 线模式再启用 D1~D3。
 * 本模块先按 4 线挂载，失败自动回退 1 线，兼容只接了 D0 的卡座。
 */
#define SDMMC_PIN_CLK   41
#define SDMMC_PIN_CMD   42
#define SDMMC_PIN_D0    47
#define SDMMC_PIN_D1    48
#define SDMMC_PIN_D2    1
#define SDMMC_PIN_D3    2

#define SD_MOUNT_POINT  "/sdcard"
#define SD_FREQ_KHZ     10000        /* 当前总线速率 10MHz */

typedef struct {
    bool     mounted;           /* 是否已成功挂载 FATFS */
    int      width;             /* 实际总线宽度：1 或 4（0=未挂载） */
    int      freq_khz;          /* 实际时钟（kHz） */
    uint64_t capacity_bytes;    /* 卡总容量（字节） */
    uint32_t sector_count;      /* 扇区数（512 字节/扇区） */
    char     card_name[16];     /* CID 里的卡名 */
} sd_card_info_t;

/* 全局 SD 卡状态：UI 线程只读，挂载在 sd_card_mount() 里更新 */
extern sd_card_info_t g_sdcard;

/* 挂载 SD 卡（FATFS 挂到 SD_MOUNT_POINT）。
 * width4=true 时依次尝试：4线10MHz → 4线4MHz → 1线10MHz，成功即停；
 * width4=false 只用 1 线 10MHz。
 * 未格式化 / 无卡 / 接线错误都返回非 ESP_OK，不会格式化用户的数据。
 */
esp_err_t sd_card_mount(bool width4);

/* 卸载并释放 SDMMC 外设 */
void sd_card_unmount(void);

bool sd_card_is_mounted(void);

/* 查询 FATFS 空间：total / free 单位为字节，返回 false 表示未挂载 */
bool sd_card_get_space(uint64_t *total, uint64_t *free_bytes);
