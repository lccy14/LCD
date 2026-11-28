#include "usb_msc.h"

#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_msc.h"
#include "sd_card.h"
#include "audio_player.h"
#include "mic_rec.h"
#include "voice_tts.h"
#include "usb_uac.h"
#include "usb_cdc.h"
#include "usb_ncm.h"

static const char *TAG = "USB_MSC";

/* 自定义字符串描述符：esp_tinyusb 默认描述符数量在某些 class 组合下会超过
 * 组件内部的 8 个上限（String descriptors exceed limit），导致描述符设置失败、
 * 电脑无法识别 U 盘。这里显式提供 6 个，覆盖 LANGID/MANU/PROD/SERIAL/CDC/MSC，
 * 数量固定 6 ≤ 8，绕开默认值超限问题。索引 4/5 都填通用名，
 * 这样无论 CDC 是否编译进来，MSC 接口的 iFunction 都能指向合法字符串。 */
static const char *s_usb_strings[] = {
    (char[]){0x09, 0x04},   /* 0: LANGID (English) */
    "ESP32-S3",             /* 1: Manufacturer */
    "LCD U盘",             /* 2: Product */
    NULL,                   /* 3: Serial（运行时由 eFuse MAC 填充） */
    "Mass Storage",         /* 4: MSC/CDC 接口（占位，兼容两种 STRID 布局） */
    "Mass Storage",         /* 5: MSC 接口 */
};

/* 纯 MSC 描述符：必须显式传，否则 esp_tinyusb 会用编译期默认描述符。
 * 开了 CDC/NCM/AUDIO 后默认描述符变成复合设备（含 NCM 接口），U盘模式下
 * NCM driver 没初始化，主机枚举时 netd_open/dfifo_alloc 会 ASSERT FAILED。 */
#define MSC_CFG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_MSC_DESC_LEN)
static const tusb_desc_device_t s_msc_dev_desc = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = 0x00,   /* 纯 MSC：class 在接口层声明 */
    .bDeviceSubClass    = 0x00,
    .bDeviceProtocol    = 0x00,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = 0x303A,
    .idProduct          = 0x8001,
    .bcdDevice          = 0x0100,
    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,
    .bNumConfigurations = 0x01,
};
static const uint8_t s_msc_cfg_desc[] = {
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, MSC_CFG_TOTAL_LEN, 0x00, 100),
    TUD_MSC_DESCRIPTOR(0, 5, 0x01, 0x81, 64),
};

static usb_msc_state_t s_state = USB_MSC_IDLE;
/* TinyUSB 任务异步启动，内部只保存 desc_cfg 指针；配置对象必须是文件级 static，
 * 不能用 usb_msc_start() 的局部变量——install 返回后栈失效，任务读到垃圾的
 * string_count 就会报 "String descriptors exceed limit" 并使描述符设置失败。 */
static tinyusb_config_t s_tusb_cfg = {0};
static sdmmc_card_t   *s_card = NULL;             /* MSC 独占期间裸初始化的 card */
static tinyusb_msc_storage_handle_t s_storage = NULL;
static bool s_usb_installed = false;
static bool s_msc_driver_installed = false;
static bool s_host_attached = false;

/* USB 设备事件：只用于日志和状态提示 */
static void usb_device_event_cb(tinyusb_event_t *event, void *arg)
{
    (void)arg;
    switch (event->id) {
        case TINYUSB_EVENT_ATTACHED:
            s_host_attached = true;
            ESP_LOGI(TAG, "USB 已连接到电脑（U盘模式）");
            break;
        case TINYUSB_EVENT_DETACHED:
            s_host_attached = false;
            ESP_LOGI(TAG, "USB 已断开");
            break;
        default:
            break;
    }
}

/* MSC 存储事件：跟踪电脑挂载/卸载 */
static void msc_storage_event_cb(tinyusb_msc_storage_handle_t handle,
                                 tinyusb_msc_event_t *event, void *arg)
{
    (void)handle; (void)arg;
    switch (event->id) {
        case TINYUSB_MSC_EVENT_MOUNT_COMPLETE:
            ESP_LOGI(TAG, "MSC %s 完成",
                     event->mount_point == TINYUSB_MSC_STORAGE_MOUNT_USB ? "交给电脑" : "交回设备");
            break;
        case TINYUSB_MSC_EVENT_MOUNT_FAILED:
            ESP_LOGE(TAG, "MSC 挂载切换失败");
            break;
        default:
            break;
    }
}

usb_msc_state_t usb_msc_get_state(void) { return s_state; }
bool usb_msc_is_active(void) { return s_state == USB_MSC_ACTIVE; }

void usb_msc_init(void)
{
    /* TinyUSB driver 在进入 U 盘模式时才安装，避免平时占用 USB 引脚/内存 */
    ESP_LOGI(TAG, "ready (lazy init)");
}

esp_err_t usb_msc_start(void)
{
    if (s_state != USB_MSC_IDLE) {
        ESP_LOGW(TAG, "状态非 IDLE(%d)，忽略 start", s_state);
        return ESP_ERR_INVALID_STATE;
    }
    s_state = USB_MSC_STARTING;
    esp_err_t err;

    /* 0. 与 UAC 声卡 / CDC / 网络共享互斥：先卸载释放 USB */
    usb_uac_stop();
    usb_cdc_stop();
    usb_ncm_stop();

    /* 1. 停掉所有会访问 SD 卡的任务 */
    audio_player_stop();
    mic_rec_stop();
    voice_tts_stop();
    vTaskDelay(pdMS_TO_TICKS(120));   /* 等正在写的句柄关闭 */

    /* 2. 记录总线参数 → 卸载设备端 FATFS（会 deinit host + free card） */
    sd_card_save_params();
    if (sd_card_is_mounted()) {
        sd_card_unmount();
    }

    /* 3. 裸初始化 SDMMC（不挂 FATFS），拿 card 给 MSC */
    err = sd_card_raw_init(&s_card);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "raw init failed: %s", esp_err_to_name(err));
        goto fail_raw;
    }

    /* 4. 安装 MSC driver（auto_mount_off：禁止 USB 插拔自动切换，手动控制） */
    tinyusb_msc_driver_config_t msc_cfg = {
        .user_flags.auto_mount_off = 1,
        .callback = msc_storage_event_cb,
        .callback_arg = NULL,
    };
    err = tinyusb_msc_install_driver(&msc_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "msc install driver failed: %s", esp_err_to_name(err));
        goto fail_msc_drv;
    }
    s_msc_driver_installed = true;

    /* 5. 创建 storage：直接用 card，初始就交给 USB（MOUNT_USB 不挂 FATFS） */
    tinyusb_msc_storage_config_t storage_cfg = {
        .medium.card = s_card,
        .mount_point = TINYUSB_MSC_STORAGE_MOUNT_USB,
    };
    err = tinyusb_msc_new_storage_sdmmc(&storage_cfg, &s_storage);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "new storage failed: %s", esp_err_to_name(err));
        goto fail_storage;
    }

    /* 6. 安装 USB device driver（full-speed OTG，默认 PHY）
     * 显式传纯 MSC 描述符：开了 NCM/CDC/AUDIO 后默认描述符是复合设备，
     * 会导致未初始化的 class（如 NCM）在枚举时 open 端点失败。 */
    s_tusb_cfg = TINYUSB_DEFAULT_CONFIG(usb_device_event_cb, NULL);
    s_tusb_cfg.descriptor.device = &s_msc_dev_desc;
    s_tusb_cfg.descriptor.string = s_usb_strings;
    s_tusb_cfg.descriptor.string_count = sizeof(s_usb_strings) / sizeof(s_usb_strings[0]);
    s_tusb_cfg.descriptor.full_speed_config = s_msc_cfg_desc;
    err = tinyusb_driver_install(&s_tusb_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "tinyusb driver install failed: %s", esp_err_to_name(err));
        goto fail_usb;
    }
    s_usb_installed = true;

    s_state = USB_MSC_ACTIVE;
    ESP_LOGI(TAG, "U盘模式已启动，请把 USB 线接到电脑");
    return ESP_OK;

fail_usb:
    tinyusb_msc_delete_storage(s_storage);
    s_storage = NULL;
fail_storage:
    tinyusb_msc_uninstall_driver();
    s_msc_driver_installed = false;
fail_msc_drv:
    sd_card_raw_deinit(s_card);
    s_card = NULL;
fail_raw:
    /* 恢复设备端 FATFS */
    if (sd_card_mount(false) == ESP_OK) {
        ESP_LOGI(TAG, "已恢复设备端 FATFS");
    } else {
        ESP_LOGE(TAG, "恢复 FATFS 失败！请复位设备");
    }
    s_state = USB_MSC_IDLE;
    return err;
}

esp_err_t usb_msc_stop(void)
{
    if (s_state != USB_MSC_ACTIVE) {
        /* 四选一切换时，每个新模式 start 都会调本函数；静默返回避免日志刷屏 */
        return ESP_OK;
    }
    s_state = USB_MSC_STOPPING;

    if (s_usb_installed) {
        tinyusb_driver_uninstall();
        s_usb_installed = false;
    }
    if (s_storage) {
        tinyusb_msc_delete_storage(s_storage);
        s_storage = NULL;
    }
    if (s_msc_driver_installed) {
        tinyusb_msc_uninstall_driver();
        s_msc_driver_installed = false;
    }
    if (s_card) {
        sd_card_raw_deinit(s_card);
        s_card = NULL;
    }

    /* 恢复设备端 FATFS */
    esp_err_t err = sd_card_mount(false);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "恢复 FATFS 失败: %s", esp_err_to_name(err));
        s_state = USB_MSC_IDLE;
        return err;
    }
    s_host_attached = false;
    s_state = USB_MSC_IDLE;
    ESP_LOGI(TAG, "已退出 U盘模式，设备端 FATFS 恢复");
    return ESP_OK;
}
