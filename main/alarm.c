#include "alarm.h"

#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"

static const char *TAG = "ALARM";

/* NVS 命名空间与键名（与 wifi 的 wificfg 分开，互不干扰）
 * 多闹钟用 h0/h1/h2、m0/m1/m2、e0/e1/e2 三组键保存；
 * 旧版本（单闹钟）的 hour/min/en 在首次启动时迁移到 idx 0 */
#define NVS_NS_ALARM  "alarmcfg"
static void key_h(char *buf, int idx) { snprintf(buf, 8, "h%d", idx); }
static void key_m(char *buf, int idx) { snprintf(buf, 8, "m%d", idx); }
static void key_e(char *buf, int idx) { snprintf(buf, 8, "e%d", idx); }

/* 首次使用（NVS 里没有记录）时的默认值：07:30 / 12:00 / 18:00，全部关闭 */
static alarm_conf_t s_alarms[ALARM_MAX] = {
    { .hour =  7, .minute = 30, .enabled = false },
    { .hour = 12, .minute =  0, .enabled = false },
    { .hour = 18, .minute =  0, .enabled = false },
};

/* 每个闹钟单独的「本分钟是否已触发」标记，避免 60 秒内反复弹窗 */
static bool s_fired[ALARM_MAX];

static void alarm_save_idx(int idx)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS_ALARM, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open for write failed");
        return;
    }
    char kh[8], km[8], ke[8];
    key_h(kh, idx); key_m(km, idx); key_e(ke, idx);
    nvs_set_i32(h, kh, s_alarms[idx].hour);
    nvs_set_i32(h, km, s_alarms[idx].minute);
    nvs_set_u8(h, ke, s_alarms[idx].enabled ? 1 : 0);
    nvs_commit(h);
    nvs_close(h);
}

void alarm_init(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS_ALARM, NVS_READONLY, &h) != ESP_OK) {
        /* 从没保存过：直接用默认值，不算错误 */
        ESP_LOGW(TAG, "no saved alarm, use default (all off)");
        return;
    }

    /* 老版本（单闹钟）迁移：若存在 hour/min/en 三个旧键，把它写到 idx 0 */
    int32_t old_h = -1, old_m = -1;
    uint8_t old_en = 0;
    bool has_old = (nvs_get_i32(h, "hour", &old_h) == ESP_OK &&
                    nvs_get_i32(h, "min",  &old_m) == ESP_OK &&
                    nvs_get_u8(h,  "en",   &old_en) == ESP_OK);
    if (has_old) {
        if (old_h >= 0 && old_h <= 23) s_alarms[0].hour = (int)old_h;
        if (old_m >= 0 && old_m <= 59) s_alarms[0].minute = (int)old_m;
        s_alarms[0].enabled = (old_en != 0);
        ESP_LOGI(TAG, "migrate legacy alarm -> idx0: %02d:%02d %s",
                 s_alarms[0].hour, s_alarms[0].minute,
                 s_alarms[0].enabled ? "on" : "off");
    }

    /* 读新的 h0/h1/h2 ... */
    for (int i = 0; i < ALARM_MAX; i++) {
        if (has_old && i == 0) continue;   /* 已用旧键覆盖，不再读 */
        int32_t hh = s_alarms[i].hour;
        int32_t mm = s_alarms[i].minute;
        uint8_t en = s_alarms[i].enabled ? 1 : 0;
        char kh[8], km[8], ke[8];
        key_h(kh, i); key_m(km, i); key_e(ke, i);
        nvs_get_i32(h, kh, &hh);
        nvs_get_i32(h, km, &mm);
        nvs_get_u8(h,  ke, &en);
        if (hh >= 0 && hh <= 23) s_alarms[i].hour = (int)hh;
        if (mm >= 0 && mm <= 59) s_alarms[i].minute = (int)mm;
        s_alarms[i].enabled = (en != 0);
    }
    nvs_close(h);

    for (int i = 0; i < ALARM_MAX; i++) {
        ESP_LOGI(TAG, "loaded[%d]: %02d:%02d %s", i,
                 s_alarms[i].hour, s_alarms[i].minute,
                 s_alarms[i].enabled ? "on" : "off");
    }
}

const alarm_conf_t *alarm_get(int idx)
{
    if (idx < 0 || idx >= ALARM_MAX) return &s_alarms[0];
    return &s_alarms[idx];
}

void alarm_set(int idx, int hour, int minute, bool enabled)
{
    if (idx < 0 || idx >= ALARM_MAX) return;
    /* 做范围收敛，避免 UI 传入越界值把设置写坏 */
    if (hour < 0) hour = 0;
    if (hour > 23) hour = 23;
    if (minute < 0) minute = 0;
    if (minute > 59) minute = 59;

    s_alarms[idx].hour = hour;
    s_alarms[idx].minute = minute;
    s_alarms[idx].enabled = enabled;

    alarm_save_idx(idx);
}

bool alarm_poll(const struct tm *now, int *out_idx)
{
    for (int i = 0; i < ALARM_MAX; i++) {
        const alarm_conf_t *a = &s_alarms[i];
        if (!a->enabled) continue;

        /* 不在设定的那一分钟：清掉触发标记，下次进入该分钟时可以重新触发 */
        if (now->tm_hour != a->hour || now->tm_min != a->minute) {
            s_fired[i] = false;
            continue;
        }

        if (s_fired[i]) continue;   /* 这一分钟已经弹过了 */

        s_fired[i] = true;
        if (out_idx) *out_idx = i;
        return true;
    }
    return false;
}

void alarm_dismiss(void)
{
    /* 标记所有闹钟本次已处理，本分钟内不再重复弹；
     * 离开该分钟后 alarm_poll 会自动复位 */
    for (int i = 0; i < ALARM_MAX; i++) {
        s_fired[i] = true;
    }
}
