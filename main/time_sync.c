#include "time_sync.h"
#include "esp_netif_sntp.h"
#include "esp_log.h"
#include <stdlib.h>     /* setenv / tzset */
#include <string.h>
#include <time.h>
#include <sys/time.h>

static const char *TAG = "TIMESYNC";

static bool s_inited = false;
static bool s_synced = false;

/* 中国标准时间 UTC+8，不启用夏令时。
 * 这里不依赖 newlib 的 TZ 环境变量解析，而是直接对 UTC 秒数加 8 小时偏移，
 * 避免部分 newlib 环境对 CST-8 解析不一致导致时间偏差。 */
#define TIMEZONE_OFFSET_HOURS   8
#define TIMEZONE_OFFSET_SEC     ((TIMEZONE_OFFSET_HOURS) * 3600)

/* 同时打印 UTC 和本地时间，方便从日志判断时区转换是否正确 */
static void s_log_time(const char *prefix, time_t now)
{
    struct tm utc, local;
    gmtime_r(&now, &utc);
    time_t local_t = now + TIMEZONE_OFFSET_SEC;
    gmtime_r(&local_t, &local);
    ESP_LOGI(TAG, "%s UTC=%04d-%02d-%02d %02d:%02d:%02d, LOCAL(CST)=%04d-%02d-%02d %02d:%02d:%02d",
             prefix,
             utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday,
             utc.tm_hour, utc.tm_min, utc.tm_sec,
             local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
             local.tm_hour, local.tm_min, local.tm_sec);
}

/* SNTP 同步成功回调：把校时结果写入系统时钟，并置位标志。
 * 注意：只要注册了 sync_cb，ESP-IDF 就不会再自动 settimeofday，
 * 必须在这里手动把 tv 写进系统时钟，否则 time() 始终是开机基准（1970）。 */
static void s_sync_cb(struct timeval *tv)
{
    if (tv) {
        settimeofday(tv, NULL);
    }
    s_synced = true;
    ESP_LOGI(TAG, "time synced via SNTP");
    s_log_time("after sync", time(NULL));
}

void time_sync_start(void)
{
    if (s_inited) {
        return;
    }
    s_inited = true;

    /* 仍保留 TZ 设置，供可能直接调用 localtime_r 的其它代码使用。
     * time_sync_localtime() 内部会用手动偏移，不依赖此环境变量。 */
    setenv("TZ", "CST-8", 1);
    tzset();

    /* 多个服务器兜底，任一可达即可校时。服务器列表必须用 ESP_SNTP_SERVER_LIST 包起来，
     * 否则逗号会被宏展开当成多个参数而报 “passed N arguments, but takes just 2”。 */
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(3,
        ESP_SNTP_SERVER_LIST("pool.ntp.org", "time.google.com", "cn.pool.ntp.org"));
    cfg.sync_cb = s_sync_cb;       /* 异步通知，不阻塞事件循环 */
    cfg.wait_for_sync = false;     /* 不使用阻塞等待信号量 */

    esp_err_t err = esp_netif_sntp_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "sntp init failed: 0x%x", err);
        s_inited = false;          /* 允许下次（重连）再试 */
        return;
    }
    ESP_LOGI(TAG, "SNTP started, waiting for sync");
}

bool time_sync_is_done(void)
{
    return s_synced;
}

bool time_sync_localtime(struct tm *tm)
{
    if (!s_synced) {
        return false;
    }
    time_t now = time(NULL);
    /* 手动按 UTC+8 转换，避免依赖 newlib 的 TZ 环境变量解析 */
    time_t local_now = now + TIMEZONE_OFFSET_SEC;
    gmtime_r(&local_now, tm);
    return true;
}
