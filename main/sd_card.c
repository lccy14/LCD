#include "sd_card.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>
#include <dirent.h>

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

/* 配置 host/slot：1 线模式把 D1~D3 显式设成 NC，避免误用默认引脚。 */
static void sd_config_host_slot(sdmmc_host_t *host, sdmmc_slot_config_t *slot,
                                int width, int freq_khz)
{
    *host = (sdmmc_host_t)SDMMC_HOST_DEFAULT();
    host->max_freq_khz = freq_khz;

    *slot = (sdmmc_slot_config_t)SDMMC_SLOT_CONFIG_DEFAULT();
    slot->width = width;
    slot->clk   = (gpio_num_t)SDMMC_PIN_CLK;
    slot->cmd   = (gpio_num_t)SDMMC_PIN_CMD;
    slot->d0    = (gpio_num_t)SDMMC_PIN_D0;
    if (width >= 4) {
        slot->d1 = (gpio_num_t)SDMMC_PIN_D1;
        slot->d2 = (gpio_num_t)SDMMC_PIN_D2;
        slot->d3 = (gpio_num_t)SDMMC_PIN_D3;
    } else {
        slot->d1 = GPIO_NUM_NC;
        slot->d2 = GPIO_NUM_NC;
        slot->d3 = GPIO_NUM_NC;
    }
    slot->cd = SDMMC_SLOT_NO_CD;      /* 卡座无 CD/WP 检测脚 */
    slot->wp = SDMMC_SLOT_NO_WP;
    slot->flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;   /* 外部没上拉时先靠内部上拉顶一下 */
}

/* 按指定总线宽度初始化 SDMMC 主机并挂载 FATFS。 */
static esp_err_t mount_with_width(int width, int freq_khz)
{
    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,   /* 绝不格式化用户的数据 */
        .max_files              = 5,
        .allocation_unit_size   = 16 * 1024,
        .disk_status_check_enable = false,
        .use_one_fat            = false,
    };

    sdmmc_host_t host;
    sdmmc_slot_config_t slot;
    sd_config_host_slot(&host, &slot, width, freq_khz);

    sdmmc_card_t *card = NULL;
    esp_err_t err = esp_vfs_fat_sdmmc_mount(SD_MOUNT_POINT, &host, &slot, &mount_cfg, &card);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%d 线 %d kHz 挂载失败: %s", width, freq_khz, esp_err_to_name(err));
        return err;
    }

    s_card = card;
    g_sdcard.mounted        = true;
    g_sdcard.width          = width;
    /* real_freq_khz 是驱动与卡协商后真正配置到控制器的频率：
     * 请求 40MHz 但卡不支持 HS 时，IDF 会降到 20MHz，这里必须记实际值，
     * 否则 UI/日志会误以为跑在 40MHz。极少数路径该字段为 0，回退到请求值。 */
    int real_khz = card->real_freq_khz > 0 ? card->real_freq_khz : freq_khz;
    g_sdcard.freq_khz       = real_khz;
    g_sdcard.sector_count   = card->csd.capacity;
    g_sdcard.capacity_bytes = ((uint64_t)card->csd.capacity) * card->csd.sector_size;
    /* cid.name 是 char[8]，可能没有结尾的 '\0'，用 %.8s 限制长度避免读越界 */
    snprintf(g_sdcard.card_name, sizeof(g_sdcard.card_name), "%.8s", (const char *)card->cid.name);

    ESP_LOGI(TAG, "已挂载 %s: %s, %d 线 请求%d kHz / 实际%d kHz, %llu MB, %u 扇区",
             SD_MOUNT_POINT, g_sdcard.card_name, width, freq_khz, real_khz,
             (unsigned long long)(g_sdcard.capacity_bytes / (1024 * 1024)),
             (unsigned)g_sdcard.sector_count);
    if (real_khz < freq_khz) {
        ESP_LOGW(TAG, "卡未跑满请求时钟：%d → %d kHz（卡不支持 HS 或驱动降级）", freq_khz, real_khz);
    }
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

/* 挂载尝试顺序（(宽度, 时钟)）按板级版本区分：
 * 第二版：1线40M → 1线4M → 4线4M → 4线40M（外接 Quad PSRAM，IO35/36/37 可用作 SD）。
 * 第三版：只 1 线（S3R8 的 IO35/36/37 被内部 PSRAM 占用，绝不能配成 D2/D3）。 */
typedef struct { int width; int freq_khz; } sd_try_t;
#if BOARD_REV >= 3
static const sd_try_t s_try_list[] = {
    { 1, SD_FREQ_KHZ },     /* 1 线 40MHz */
    { 1, 4000 },            /* 40MHz 不稳时回退 1 线 4MHz */
};
#else
static const sd_try_t s_try_list[] = {
    { 1, SD_FREQ_KHZ },
    { 1, 4000 },
    { 4, 4000 },
    { 4, SD_FREQ_KHZ },
};
#endif

#if BOARD_REV < 3
/* 4 线全部失败后的体检（仅第二版）：用 400kHz 探一次 D1~D3 是否接通。
 * 成功 → 走线通、只是高速信号质量差；失败 → D1/D2/D3 没接/接错，只能用 1 线。 */
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
#endif

esp_err_t sd_card_mount(bool width4)
{
    if (g_sdcard.mounted) {
        return ESP_OK;
    }

    sd_log_heap("挂载前");

#if BOARD_REV >= 3
    (void)width4;   /* 第三版 SD 固定 1 线，忽略调用方的 width4 参数 */
#endif
    esp_err_t err = ESP_FAIL;
    for (size_t i = 0; i < sizeof(s_try_list) / sizeof(s_try_list[0]); i++) {
        int w = s_try_list[i].width;
        int f = s_try_list[i].freq_khz;
#if BOARD_REV < 3
        if (w == 4 && !width4) continue;   /* 调用方只要 1 线 */
#endif
        err = mount_with_retry(w, f);
        if (err == ESP_OK) {
            return ESP_OK;
        }
#if BOARD_REV < 3
        if (w == 4 && err == ESP_ERR_INVALID_CRC) {
            ESP_LOGW(TAG, "4 线 %d kHz 数据 CRC 错误：D1~D3 走线/上拉不良，尝试降速", f);
        }
        if (w == 4 && f == 4000 && width4) {
            probe_4bit_lowspeed();
        }
#endif
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

/* 清理形如 prefixNNNN.ext 的旧文件，只保留编号最大的 keep 个。
 * 文件名编号递增（REC_0001.wav / TTS_0001.mp3），编号越大越新。 */
int sd_prune_files(const char *prefix, const char *ext, int keep)
{
    if (!g_sdcard.mounted || !prefix || !ext || keep < 0) return 0;

    DIR *d = opendir(SD_MOUNT_POINT);
    if (!d) { ESP_LOGW(TAG, "prune: opendir failed"); return 0; }

    /* 收集匹配文件：编号 + 完整路径。上限 512 个，够日常使用 */
    typedef struct { int id; char name[64]; } file_entry_t;
    file_entry_t files[512];
    int count = 0;
    size_t plen = strlen(prefix), elen = strlen(ext);

    struct dirent *e;
    while ((e = readdir(d)) != NULL && count < 512) {
        const char *nm = e->d_name;
        size_t nl = strlen(nm);
        if (nl <= plen + elen) continue;                       /* 太短，跳过 */
        if (strncmp(nm, prefix, plen) != 0) continue;           /* 前缀不符 */
        if (strcmp(nm + nl - elen, ext) != 0) continue;         /* 扩展名不符 */
        /* 中间必须是纯数字编号 */
        const char *num = nm + plen;
        size_t nlen = nl - plen - elen;
        int id = 0;
        bool digits = true;
        for (size_t i = 0; i < nlen; i++) {
            if (num[i] < '0' || num[i] > '9') { digits = false; break; }
            id = id * 10 + (num[i] - '0');
        }
        if (!digits || nlen == 0) continue;
        files[count].id = id;
        /* 只取解析出的 prefix+数字+ext 部分，不拷整个 d_name，避免截断警告 */
        snprintf(files[count].name, sizeof(files[count].name), "%s/%.*s",
                 SD_MOUNT_POINT, (int)nl, nm);
        count++;
    }
    closedir(d);

    if (count <= keep) return 0;   /* 未超限，不删 */

    /* 冒泡排序：编号小的（旧的）在前 */
    for (int i = 0; i < count - 1; i++) {
        for (int j = 0; j < count - 1 - i; j++) {
            if (files[j].id > files[j + 1].id) {
                file_entry_t t = files[j]; files[j] = files[j + 1]; files[j + 1] = t;
            }
        }
    }

    int removed = 0;
    for (int i = 0; i < count - keep; i++) {
        if (remove(files[i].name) == 0) removed++;
        else ESP_LOGW(TAG, "prune: remove %s failed", files[i].name);
    }
    if (removed) ESP_LOGI(TAG, "pruned %d old files (%s*%s), kept %d", removed, prefix, ext, keep);
    return removed;
}

/* ---- USB MSC 独占用：裸 SDMMC 初始化（不挂 FATFS） ---- */
static int s_last_width = 1;
static int s_last_freq  = SD_FREQ_KHZ;

/* 记录当前已验证成功的总线参数，供 MSC 模式复用（unmount 会清 g_sdcard） */
void sd_card_save_params(void)
{
    if (g_sdcard.width > 0) {
        s_last_width = g_sdcard.width;
        s_last_freq  = g_sdcard.freq_khz > 0 ? g_sdcard.freq_khz : SD_FREQ_KHZ;
    }
}

esp_err_t sd_card_raw_init(sdmmc_card_t **out_card)
{
    if (!out_card) return ESP_ERR_INVALID_ARG;
    *out_card = NULL;
    sdmmc_host_t host;
    sdmmc_slot_config_t slot;
    sd_config_host_slot(&host, &slot, s_last_width, s_last_freq);

    sdmmc_card_t *card = (sdmmc_card_t *)calloc(1, sizeof(sdmmc_card_t));
    if (!card) { ESP_LOGE(TAG, "raw init: calloc card failed"); return ESP_ERR_NO_MEM; }
    esp_err_t err = sdmmc_host_init();
    if (err != ESP_OK) { ESP_LOGE(TAG, "raw init: host_init %s", esp_err_to_name(err)); free(card); return err; }
    err = sdmmc_host_init_slot(host.slot, &slot);
    if (err != ESP_OK) { ESP_LOGE(TAG, "raw init: init_slot %s", esp_err_to_name(err)); sdmmc_host_deinit(); free(card); return err; }
    err = sdmmc_card_init(&host, card);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "raw init: card_init %s (%d线 %d kHz)", esp_err_to_name(err), s_last_width, s_last_freq);
        sdmmc_host_deinit(); free(card); return err;
    }
    *out_card = card;
    ESP_LOGI(TAG, "raw init OK: %d线 %d kHz, %u 扇区", s_last_width, s_last_freq,
             (unsigned)card->csd.capacity);
    return ESP_OK;
}

void sd_card_raw_deinit(sdmmc_card_t *card)
{
    sdmmc_host_deinit();
    free(card);
    ESP_LOGI(TAG, "raw deinit OK");
}
