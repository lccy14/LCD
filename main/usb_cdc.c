#include "usb_cdc.h"

#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_cdc_acm.h"
#include "usb_msc.h"
#include "usb_uac.h"
#include "usb_ncm.h"

static const char *TAG = "USB_CDC";

/* ---------------- 纯 CDC 描述符 ----------------
 * 只声明 CDC ACM（接口 0/1），不带 MSC/UAC。端点：
 *   0x82 IN  notification（8 字节）
 *   0x01 OUT / 0x81 IN  data
 * 与 UAC 分时切换，不会同时激活，故端点号可复用。 */
#define CDC_ITF_CNT      2
#define CDC_NOTIF_EP     0x82
#define CDC_DATA_EP      0x01
#define CDC_CFG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN)

static const tusb_desc_device_t s_cdc_dev_desc = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = TUSB_CLASS_MISC,
    .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol    = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = 0x303A,
    .idProduct          = 0x8002,
    .bcdDevice          = 0x0100,
    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,
    .bNumConfigurations = 0x01,
};

static const uint8_t s_cdc_cfg_desc[] = {
    TUD_CONFIG_DESCRIPTOR(1, CDC_ITF_CNT, 0, CDC_CFG_TOTAL_LEN, 0x00, 100),
    TUD_CDC_DESCRIPTOR(0, 4, CDC_NOTIF_EP, 8, CDC_DATA_EP, 0x80 | CDC_DATA_EP, 64),
};

static const char *s_cdc_strings[] = {
    (char[]){0x09, 0x04},   /* 0: LANGID */
    "ESP32-S3",             /* 1: Manufacturer */
    "LCD USB Debug",        /* 2: Product */
    NULL,                   /* 3: Serial（运行时填 MAC） */
    "USB Debug Console",    /* 4: CDC interface */
};

/* ---------------- 状态 ---------------- */
static bool              s_active = false;
static bool              s_attached = false;
static tinyusb_config_t  s_tusb_cfg = {0};   /* 异步任务只存指针，必须 static */
static TaskHandle_t      s_task = NULL;

/* ---------------- ESP_LOG 日志镜像到 CDC ----------------
 * 钩子在任何任务上下文被调用，绝不能在此直接操作 CDC（会阻塞/重入）。
 * 做法：格式化到静态缓冲，由 cdc_task 定期刷出；同时仍写原 UART console。 */
#define CDC_LOG_BUF_SIZE  2048
static char          s_log_buf[CDC_LOG_BUF_SIZE];
static size_t        s_log_len = 0;
static portMUX_TYPE  s_log_mux = portMUX_INITIALIZER_UNLOCKED;
static vprintf_like_t s_old_vprintf = NULL;   /* 保存原 UART 输出函数，stop 时恢复 */
static bool          s_log_hooked = false;

static int cdc_log_vprintf(const char *fmt, va_list args)
{
    /* 先格式化一份到缓冲给 CDC（va_list 只能用一次，另一份给原 UART） */
    char tmp[256];
    va_list args2;
    va_copy(args2, args);
    int len = vsnprintf(tmp, sizeof(tmp), fmt, args);
    if (len > 0) {
        size_t n = (size_t)len;
        if (n > sizeof(tmp) - 1) n = sizeof(tmp) - 1;   /* 单条日志截断 */
        portENTER_CRITICAL(&s_log_mux);
        if (s_log_len + n <= sizeof(s_log_buf)) {
            memcpy(s_log_buf + s_log_len, tmp, n);
            s_log_len += n;
        } else {
            /* 缓冲满：丢弃本条（日志尽力而为，绝不阻塞调用者） */
        }
        portEXIT_CRITICAL(&s_log_mux);
    }
    /* 原 UART console 照常输出（保留原调试口） */
    int ret = 0;
    if (s_old_vprintf) ret = s_old_vprintf(fmt, args2);
    va_end(args2);
    return ret;
}

static void cdc_event_cb(tinyusb_event_t *event, void *arg)
{
    (void)arg;
    if (event->id == TINYUSB_EVENT_ATTACHED) {
        s_attached = true;
        ESP_LOGI(TAG, "USB 已连接到电脑（CDC 调试口）");
    } else if (event->id == TINYUSB_EVENT_DETACHED) {
        s_attached = false;
    }
}

/* ---------------- CDC 任务：收命令回显 + 周期状态上报 ---------------- */
static void cdc_task(void *arg)
{
    (void)arg;
    uint8_t rx[128];
    char    line[64];
    static char log_tx[CDC_LOG_BUF_SIZE];
    TickType_t last_report = xTaskGetTickCount();

    while (s_active) {
        /* 0. 把 ESP_LOG 镜像缓冲刷到 CDC（临界区只做拷贝，USB 操作在锁外） */
        size_t log_n = 0;
        portENTER_CRITICAL(&s_log_mux);
        if (s_log_len > 0) {
            log_n = s_log_len;
            memcpy(log_tx, s_log_buf, log_n);
            s_log_len = 0;
        }
        portEXIT_CRITICAL(&s_log_mux);
        if (log_n > 0) {
            tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0, (const uint8_t *)log_tx, log_n);
            tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, 0);
        }

        /* 1. 读取电脑发来的数据，原样回显 */
        size_t got = 0;
        if (tinyusb_cdcacm_read(TINYUSB_CDC_ACM_0, rx, sizeof(rx), &got) == ESP_OK && got > 0) {
            tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0, rx, got);
            tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, 0);
        }

        /* 2. 每 5 秒上报一次设备状态（仅连接后） */
        if (s_attached && (xTaskGetTickCount() - last_report) >= pdMS_TO_TICKS(5000)) {
            last_report = xTaskGetTickCount();
            int len = snprintf(line, sizeof(line),
                        "[ESP32-S3] INT free=%uKB big=%uKB | PSRAM free=%uKB\r\n",
                        (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                        (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024),
                        (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
            if (len > 0) {
                tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0, (const uint8_t *)line, len);
                tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, 0);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    s_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t usb_cdc_start(void)
{
    if (s_active) return ESP_OK;

    /* 与 U盘 / 声卡互斥：先停掉释放 USB */
    usb_msc_stop();
    usb_uac_stop();
    usb_ncm_stop();

    /* 1. 安装 USB device driver，传纯 CDC 描述符 */
    s_tusb_cfg = TINYUSB_DEFAULT_CONFIG(cdc_event_cb, NULL);
    s_tusb_cfg.descriptor.device = &s_cdc_dev_desc;
    s_tusb_cfg.descriptor.string = s_cdc_strings;
    s_tusb_cfg.descriptor.string_count = sizeof(s_cdc_strings) / sizeof(s_cdc_strings[0]);
    s_tusb_cfg.descriptor.full_speed_config = s_cdc_cfg_desc;
    esp_err_t err = tinyusb_driver_install(&s_tusb_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "tinyusb install failed: %s", esp_err_to_name(err));
        return err;
    }

    /* 2. 初始化 CDC ACM 接口 */
    tinyusb_config_cdcacm_t acm_cfg = {
        .cdc_port = TINYUSB_CDC_ACM_0,
        .callback_rx = NULL,   /* 用轮询读，简化 */
    };
    err = tinyusb_cdcacm_init(&acm_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "cdcacm init failed: %s", esp_err_to_name(err));
        tinyusb_driver_uninstall();
        return err;
    }

    /* 3. 安装日志钩子：所有 ESP_LOG 镜像到 CDC（原 UART 保留） */
    s_log_len = 0;
    s_old_vprintf = esp_log_set_vprintf(cdc_log_vprintf);
    s_log_hooked = true;

    /* 4. 启动收发任务（栈放 PSRAM 省内部 RAM）
     * 先置 active 再建任务，避免高优先级下任务一启动就因 active=false 退出 */
    s_active = true;
    s_attached = false;
    BaseType_t ok = xTaskCreateWithCaps(cdc_task, "usb_cdc", 4096, NULL, 3, &s_task, MALLOC_CAP_SPIRAM);
    if (ok != pdPASS) {
        xTaskCreate(cdc_task, "usb_cdc", 4096, NULL, 3, &s_task);
    }

    ESP_LOGI(TAG, "USB CDC 调试口已启动，ESP_LOG 已镜像到 USB，请把 USB 线接到电脑");
    return ESP_OK;
}

esp_err_t usb_cdc_stop(void)
{
    if (!s_active) return ESP_OK;
    s_active = false;

    if (s_task) {
        vTaskDelete(s_task);
        s_task = NULL;
    }
    /* 恢复原日志输出，避免日志继续往已停的 CDC 缓冲里堆 */
    if (s_log_hooked) {
        esp_log_set_vprintf(s_old_vprintf);
        s_log_hooked = false;
        s_old_vprintf = NULL;
    }
    s_log_len = 0;
    tinyusb_cdcacm_deinit(TINYUSB_CDC_ACM_0);
    tinyusb_driver_uninstall();
    s_attached = false;
    ESP_LOGI(TAG, "USB CDC 调试口已关闭");
    return ESP_OK;
}

bool usb_cdc_is_active(void) { return s_active; }
