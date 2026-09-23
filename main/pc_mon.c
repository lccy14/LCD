#include "pc_mon.h"
#include "wifi_scan.h"      /* g_wifi_state */
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "esp_timer.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "PCMON";

/* PC 端服务地址：改成你电脑在局域网里的 IP（运行 pc_mon_server.py 会打印出来） */
#ifndef PC_MON_SERVER_IP
#define PC_MON_SERVER_IP "192.168.1.118"
#endif
#define PC_MON_SERVER_PORT 8123
#define PC_MON_RECV_TIMEOUT_MS 800

pc_mon_data_t g_pcmon = {0};

/* 把 src 前 len 字节拷进 dst（去尾保证以 \0 结尾） */
static void field_copy(char *dst, size_t n, const char *src, size_t len)
{
    if (len > n - 1) len = n - 1;
    size_t i = 0;
    for (; i < len && src[i]; i++) dst[i] = src[i];
    dst[i] = '\0';
}

/* 把 AIDA64 常见的英文标签翻译成中文（CJK 以 UTF-8 存储）。
 * 策略：整串精确匹配优先；未命中再按关键字替换（CPU/GPU/温度/风扇…）。
 * 仍命中不了的标签保持英文原样，避免乱码。 */
static const char *translate_label(const char *en)
{
    static const struct { const char *en; const char *cn; } exact[] = {
        { "CPU Package",      "CPU 封装" },
        { "CPU IA Cores",     "CPU 大核" },
        { "CPU GT Cores",     "CPU 核显" },
        { "CPU Uncore",       "CPU 非核心" },
        { "CPU PLL",          "CPU 锁相环" },
        { "CPU Core",         "CPU 核心" },
        { "CPU VID",          "CPU 电压" },
        { "Vcore",            "核心电压" },
        { "System",           "系统温度" },
        { "Aux",              "辅助温度" },
        { "VRM MOS",          "供电温度" },
        { "PCH",              "南桥温度" },
        { "DIMM",             "内存条" },
        { "Used Memory",      "已用内存" },
        { "Available Memory", "可用内存" },
        { "CPU Utilization",  "CPU 占用" },
        { "GPU Utilization",  "显卡占用" },
        { "CPU Frequency",    "CPU 频率" },
        { "GPU Frequency",    "显卡频率" },
        { "CPU Power",        "CPU 功耗" },
        { "GPU Power",        "显卡功耗" },
    };
    for (int i = 0; i < (int)(sizeof(exact) / sizeof(exact[0])); i++) {
        if (strcmp(en, exact[i].en) == 0) return exact[i].cn;
    }

    static const struct { const char *k; const char *cn; } kw[] = {
        { "Temperature", "温度" }, { "Temp", "温度" },
        { "Fan",         "风扇" },
        { "Voltage",     "电压" },
        { "Utilization", "占用" }, { "Usage", "占用" },
        { "Memory",      "内存" },
        { "Clock",       "频率" }, { "Frequency", "频率" },
        { "Power",       "功耗" },
        { "GPU",         "显卡" },
        { "Core",        "核心" },
        { "Package",     "封装" },
        { "Drive",       "硬盘" },
    };
    static char out[PC_MON_LABEL_LEN];
    strncpy(out, en, sizeof(out) - 1);
    out[sizeof(out) - 1] = '\0';
    for (int i = 0; i < (int)(sizeof(kw) / sizeof(kw[0])); i++) {
        char *p = strstr(out, kw[i].k);
        if (!p) continue;
        size_t klen = strlen(kw[i].k);
        size_t clen = strlen(kw[i].cn);
        size_t olen = strlen(out);
        /* 替换后总长度需落在缓冲内，否则跳过本次替换 */
        if (olen - klen + clen > sizeof(out) - 1) continue;
        memmove(p + clen, p + klen, olen - (size_t)(p - out) - klen + 1);
        memcpy(p, kw[i].cn, clen);
    }
    return out;
}

/* 解析一行 "label|value|unit"，写入 s。仅当 label 非空时返回 true */
static bool parse_line(const char *line, pc_sensor_t *s)
{
    const char *p1 = strchr(line, '|');
    if (!p1) return false;
    const char *p2 = strchr(p1 + 1, '|');
    if (!p2) return false;
    field_copy(s->label, PC_MON_LABEL_LEN, line, (size_t)(p1 - line));
    field_copy(s->value, PC_MON_VALUE_LEN, p1 + 1, (size_t)(p2 - (p1 + 1)));
    field_copy(s->unit, PC_MON_UNIT_LEN, p2 + 1, strlen(p2 + 1));
    /* 把英文标签翻译成中文后再落库，未命中则保持原英文 */
    const char *cn = translate_label(s->label);
    field_copy(s->label, PC_MON_LABEL_LEN, cn, strlen(cn));
    return s->label[0] != '\0';
}

static void mark_error(void)
{
    g_pcmon.error = true;
    g_pcmon.valid = false;
    g_pcmon.count = 0;
    g_pcmon.version++;
}

/* UI 通过这个开关控制是否拉取数据：只有停留在「电脑监控」界面时才联网轮询。
 * 离开界面后任务会阻塞在 ulTaskNotifyTake 上，不再占用 CPU、不再发网络包。 */
static volatile bool s_active = false;
static TaskHandle_t  s_task   = NULL;

void pc_mon_set_active(bool active)
{
    s_active = active;

    if (!active) {
        /* 离开界面：立即作废旧数据，回来时重新拉取，避免用户看到过期数值 */
        g_pcmon.valid = false;
        g_pcmon.error = false;
        g_pcmon.count = 0;
        g_pcmon.version++;
    }

    /* 唤醒任务：进入界面时立刻拉一次，不必等满一个间隔 */
    if (s_task) {
        xTaskNotifyGive(s_task);
    }
}

static void pc_mon_task(void *arg)
{
    (void)arg;
    char buf[4096];

    for (;;) {
        /* 不在监控界面时不联网也不空转：阻塞等待 UI 唤醒，CPU 占用为 0。
         * 在监控界面时按 1 秒间隔拉取；期间若界面退出，会被 notify 提前唤醒并跳过下一轮。 */
        if (!s_active) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        } else {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
        }

        if (g_wifi_state != WIFI_STA_CONNECTED) {
            if (g_pcmon.valid || g_pcmon.error) {
                g_pcmon.valid = false;
                g_pcmon.error = false;
                g_pcmon.count = 0;
                g_pcmon.version++;          /* 触发 UI 提示“等待连接” */
            }
            continue;
        }

        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) {
            mark_error();
            continue;
        }

        struct sockaddr_in srv = {0};
        srv.sin_family = AF_INET;
        srv.sin_port   = htons(PC_MON_SERVER_PORT);
        if (inet_pton(AF_INET, PC_MON_SERVER_IP, &srv.sin_addr) != 1) {
            ESP_LOGE(TAG, "bad server IP: %s", PC_MON_SERVER_IP);
            close(sock);
            mark_error();
            continue;
        }

        /* 接收超时，避免服务端不关闭时一直阻塞 */
        struct timeval tv = { .tv_sec = 0, .tv_usec = PC_MON_RECV_TIMEOUT_MS * 1000 };
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        if (connect(sock, (struct sockaddr *)&srv, sizeof(srv)) != 0) {
            close(sock);
            mark_error();
            continue;
        }

        /* 读到服务端关闭或超时为止 */
        int total = 0, r;
        while (total < (int)sizeof(buf) - 1 &&
               (r = recv(sock, buf + total, sizeof(buf) - 1 - total, 0)) > 0) {
            total += r;
        }
        close(sock);

        if (total <= 0) {
            mark_error();
            continue;
        }
        buf[total] = '\0';

        /* 按行解析 */
        pc_sensor_t tmp[PC_MON_MAX_SENSORS];
        int n = 0;
        char *save = NULL;
        for (char *line = strtok_r(buf, "\n", &save);
             line && n < PC_MON_MAX_SENSORS;
             line = strtok_r(NULL, "\n", &save)) {
            if (parse_line(line, &tmp[n])) n++;
        }

        /* 一次性提交，避免 UI 读到一半 */
        g_pcmon.count = n;
        for (int i = 0; i < n; i++) g_pcmon.sensors[i] = tmp[i];
        g_pcmon.updated_ms = esp_timer_get_time() / 1000;
        g_pcmon.valid = true;
        g_pcmon.error = false;
        g_pcmon.version++;
        ESP_LOGI(TAG, "got %d sensors", n);
    }
}

void pc_mon_init(void)
{
    /* 保存任务句柄：pc_mon_set_active 靠它把任务从阻塞中唤醒 */
    xTaskCreate(pc_mon_task, "pc_mon", 8192, NULL, 3, &s_task);
}
