#include "usb_ncm.h"

#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "lwip/esp_netif_net_stack.h"   /* ethernetif_init / ethernetif_input */
#include "esp_netif_defaults.h"    /* soft_ap 默认 IP 段 */
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_net.h"
#include "tusb.h"                  /* tud_mounted() */
#include "usb_msc.h"
#include "usb_uac.h"
#include "usb_cdc.h"
#include "wifi_scan.h"            /* g_wifi_state / WIFI_STA_CONNECTED */

static const char *TAG = "USB_NCM";

/* ---------------- 纯 NCM 描述符 ----------------
 * 接口 0(communication) / 1(data)，端点：
 *   0x81 IN notification, 0x02 OUT data, 0x82 IN data
 * 与其它模式分时切换，端点号可复用。 */
#define NCM_ITF_CNT  2
#define NCM_EP_NOTIF 0x81
#define NCM_EP_OUT   0x02
#define NCM_EP_IN    0x82
#define NCM_CFG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_CDC_NCM_DESC_LEN)
#define NCM_MTU       1500

static const tusb_desc_device_t s_ncm_dev_desc = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = TUSB_CLASS_MISC,
    .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol    = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = 0x303A,
    .idProduct          = 0x8003,
    .bcdDevice          = 0x0100,
    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,
    .bNumConfigurations = 0x01,
};

/* 字符串索引布局对齐 esp_tinyusb 内部 STRID（CFG_TUD_CDC=1/MSC=1 → STRID_MAC=7）*/
#define NCM_STRID_MAC 7
static const uint8_t s_ncm_cfg_desc[] = {
    TUD_CONFIG_DESCRIPTOR(1, NCM_ITF_CNT, 0, NCM_CFG_TOTAL_LEN, 0x00, 100),
    TUD_CDC_NCM_DESCRIPTOR(0, 4, NCM_STRID_MAC, NCM_EP_NOTIF, 8, NCM_EP_OUT, NCM_EP_IN, 64, NCM_MTU),
};
static const char *s_ncm_strings[] = {
    (char[]){0x09, 0x04},   /* 0: LANGID */
    "ESP32-S3",             /* 1: Manufacturer */
    "LCD USB Network",      /* 2: Product */
    NULL,                   /* 3: Serial（运行时填 MAC） */
    "USB Network",          /* 4: NCM interface */
    NULL,                   /* 5: 占位（对齐 STRID_MSC） */
    NULL,                   /* 6: 占位（对齐 STRID_NET_INTERFACE） */
    NULL,                   /* 7: MAC string — 由 tinyusb_net_init() 填 ASCII MAC */
};

/* ---------------- 状态 ---------------- */
static bool              s_active = false;
static bool              s_mounted = false;
static tinyusb_config_t  s_tusb_cfg = {0};   /* 异步任务只存指针，必须 static */
static uint8_t           s_sta_mac[6] = {0};
static esp_netif_t      *s_usb_netif = NULL; /* USB NCM 的独立 netif（NAPT 网关） */
static TaskHandle_t      s_monitor_task = NULL;

/* ---------------- USB NCM 数据面回调 ---------------- */

/* USB → netif：电脑发出的以太网帧交给 USB netif（lwIP 处理，经 NAPT 转 WiFi） */
static esp_err_t ncm_recv_cb(void *buffer, uint16_t len, void *ctx)
{
    (void)ctx;
    if (!s_usb_netif) return ESP_OK;
    /* esp_netif_receive 会接管 buffer；复制一份避免与 USB 内部 buffer 混淆 */
    void *copy = malloc(len);
    if (!copy) return ESP_ERR_NO_MEM;
    memcpy(copy, buffer, len);
    return esp_netif_receive(s_usb_netif, copy, len, NULL);
}

/* USB netif → lwIP：netif 要发包时（如 DHCP 响应），交给 USB。
 * buffer 是 lwIP pbuf 的 payload，归 lwIP 所有；tinyusb_net_send_sync() 是同步
 * 发送，返回前已把数据复制进 NTB，绝不能在这里 free(buffer)。 */
static esp_err_t usb_netif_transmit(void *h, void *buffer, size_t len)
{
    (void)h;
    if (len > UINT16_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = tinyusb_net_send_sync(buffer, (uint16_t)len, NULL, pdMS_TO_TICKS(100));
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "usb net send failed: %s", esp_err_to_name(err));
    }
    return err;
}

static void usb_netif_free_rx(void *h, void *buffer)
{
    (void)h;
    free(buffer);
}

/* NCM 枚举状态监控（新版 TinyUSB 不触发 on_init_callback） */
static void ncm_monitor_task(void *arg)
{
    (void)arg;
    bool was_mounted = false;
    while (s_active) {
        bool mounted = tud_mounted();
        if (mounted != was_mounted) {
            was_mounted = mounted;
            s_mounted = mounted;
            if (mounted) ESP_LOGI(TAG, "USB 网络已枚举成功，电脑应识别到网卡");
            else        ESP_LOGW(TAG, "USB 枚举断开，等待电脑重新连接");
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    s_monitor_task = NULL;
    vTaskDelete(NULL);
}

static void ncm_event_cb(tinyusb_event_t *event, void *arg)
{
    (void)arg;
    if (event->id == TINYUSB_EVENT_ATTACHED) {
        ESP_LOGI(TAG, "USB 线已插入，等待电脑枚举");
    } else if (event->id == TINYUSB_EVENT_DETACHED) {
        ESP_LOGI(TAG, "USB 线已拔出");
    }
}

/* ---------------- 启停 ---------------- */
esp_err_t usb_ncm_start(void)
{
    if (s_active) return ESP_OK;

    /* 前提：WiFi 必须已连上路由器 */
    if (g_wifi_state != WIFI_STA_CONNECTED) {
        ESP_LOGW(TAG, "WiFi 未连接，无法启动 USB 网络共享");
        return ESP_ERR_INVALID_STATE;
    }

    usb_msc_stop();
    usb_uac_stop();
    usb_cdc_stop();

    esp_read_mac(s_sta_mac, ESP_MAC_WIFI_STA);

    static char serial[13];
    snprintf(serial, sizeof(serial), "%02X%02X%02X%02X%02X%02X",
             s_sta_mac[0], s_sta_mac[1], s_sta_mac[2],
             s_sta_mac[3], s_sta_mac[4], s_sta_mac[5]);
    s_ncm_strings[3] = serial;

    /* 1. 安装 USB device driver，传纯 NCM 描述符 */
    s_tusb_cfg = TINYUSB_DEFAULT_CONFIG(ncm_event_cb, NULL);
    s_tusb_cfg.descriptor.device = &s_ncm_dev_desc;
    s_tusb_cfg.descriptor.string = s_ncm_strings;
    s_tusb_cfg.descriptor.string_count = sizeof(s_ncm_strings) / sizeof(s_ncm_strings[0]);
    s_tusb_cfg.descriptor.full_speed_config = s_ncm_cfg_desc;
    esp_err_t err = tinyusb_driver_install(&s_tusb_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "tinyusb install failed: %s", esp_err_to_name(err));
        return err;
    }

    /* 2. 初始化 NCM net：用独立 MAC（与 WiFi STA 不同，避免同 MAC 冲突） */
    uint8_t ncm_mac[6];
    memcpy(ncm_mac, s_sta_mac, 6);
    ncm_mac[0] |= 0x02;              /* 本地管理位，保证与 STA MAC 不同 */
    tinyusb_net_config_t net_cfg = {
        .mac_addr = {0},
        .on_recv_callback = ncm_recv_cb,
        .free_tx_buffer = NULL,   /* TX buffer 归 lwIP 所有，不能由 USB 回调释放 */
        .on_init_callback = NULL,
        .user_context = NULL,
    };
    memcpy(net_cfg.mac_addr, ncm_mac, 6);
    err = tinyusb_net_init(&net_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "tinyusb_net_init failed: %s", esp_err_to_name(err));
        tinyusb_driver_uninstall();
        return err;
    }

    /* 3. 创建 USB netif：独立子网 + DHCP server，电脑拿 192.168.4.x */
    esp_netif_inherent_config_t base_cfg = {
        .flags = ESP_NETIF_DHCP_SERVER | ESP_NETIF_FLAG_AUTOUP,
        .ip_info = &_g_esp_netif_soft_ap_ip,   /* 192.168.4.1/24（和 IDF softAP 同段） */
        .if_key = "USB_NCM",
        .if_desc = "USB NCM network share",
        .route_prio = 10,
    };
    esp_netif_driver_ifconfig_t driver_cfg = {
        .handle = (void *)1,               /* NCM 是单例，句柄非 NULL 即可 */
        .transmit = usb_netif_transmit,
        .driver_free_rx_buffer = usb_netif_free_rx,
    };
    esp_netif_netstack_config_t netstack_cfg = {
        .lwip = {
            .init_fn = ethernetif_init,
            .input_fn = ethernetif_input,
        },
    };
    esp_netif_config_t if_cfg = {
        .base = &base_cfg,
        .driver = &driver_cfg,
        .stack = &netstack_cfg,
    };
    s_usb_netif = esp_netif_new(&if_cfg);
    if (!s_usb_netif) {
        ESP_LOGE(TAG, "创建 USB netif 失败");
        tinyusb_net_deinit();
        tinyusb_driver_uninstall();
        return ESP_FAIL;
    }
    esp_netif_set_mac(s_usb_netif, ncm_mac);

    /* DHCP server 默认把 DNS 下发成自身 IP(192.168.4.1)，但设备没有 DNS 服务，
     * 电脑会拿不到域名解析。改为下发 WiFi STA 的上游 DNS；取不到时回退到
     * 上游网关（例如 192.168.1.1）。必须在 DHCP server 启动前配置。 */
    {
        esp_netif_t *sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        esp_netif_dns_info_t dns = {0};
        bool have_dns = false;

        if (sta_netif &&
            esp_netif_get_dns_info(sta_netif, ESP_NETIF_DNS_MAIN, &dns) == ESP_OK &&
            dns.ip.type == ESP_IPADDR_TYPE_V4 && dns.ip.u_addr.ip4.addr != 0) {
            have_dns = true;
        }
        if (!have_dns && sta_netif) {
            esp_netif_ip_info_t sta_ip = {0};
            if (esp_netif_get_ip_info(sta_netif, &sta_ip) == ESP_OK && sta_ip.gw.addr != 0) {
                dns.ip.type = ESP_IPADDR_TYPE_V4;
                dns.ip.u_addr.ip4.addr = sta_ip.gw.addr;
                have_dns = true;
            }
        }
        if (have_dns) {
            uint8_t offer_dns = 1;   /* 非 0 即打开 DHCP OFFER_DNS */
            esp_err_t dns_err = esp_netif_dhcps_option(s_usb_netif, ESP_NETIF_OP_SET,
                                                       ESP_NETIF_DOMAIN_NAME_SERVER,
                                                       &offer_dns, sizeof(offer_dns));
            if (dns_err == ESP_OK) {
                dns_err = esp_netif_set_dns_info(s_usb_netif, ESP_NETIF_DNS_MAIN, &dns);
            }
            if (dns_err == ESP_OK) {
                ESP_LOGI(TAG, "DHCP DNS 已设为上游 DNS/网关，电脑可解析域名");
            } else {
                ESP_LOGW(TAG, "设置 DHCP DNS 失败: %s", esp_err_to_name(dns_err));
            }
        } else {
            ESP_LOGW(TAG, "未取到上游 DNS/网关，电脑可能只能按 IP 访问");
        }
    }
    esp_netif_action_start(s_usb_netif, 0, 0, 0);

    /* 4. 启用 NAPT：USB 子网流量经 WiFi STA 转发出去（设备自己网络不受影响） */
    esp_netif_set_default_netif(esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"));
    err = esp_netif_napt_enable(s_usb_netif);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NAPT 启用失败: %s（检查 CONFIG_LWIP_IPV4_NAPT）", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "NAPT 已启用：USB 子网 → WiFi STA");
    }

    s_active = true;
    s_mounted = false;
    ESP_LOGI(TAG, "USB 网络共享已启动（USB NCM → NAPT → WiFi STA），等待电脑枚举");
    xTaskCreate(ncm_monitor_task, "ncm_mon", 2048, NULL, 2, &s_monitor_task);
    return ESP_OK;
}

esp_err_t usb_ncm_stop(void)
{
    if (!s_active) return ESP_OK;
    s_active = false;
    s_mounted = false;

    if (s_monitor_task) {
        vTaskDelete(s_monitor_task);
        s_monitor_task = NULL;
    }
    if (s_usb_netif) {
        esp_netif_napt_disable(s_usb_netif);
        esp_netif_destroy(s_usb_netif);
        s_usb_netif = NULL;
    }
    tinyusb_net_deinit();
    tinyusb_driver_uninstall();
    ESP_LOGI(TAG, "USB 网络共享已关闭");
    return ESP_OK;
}

bool usb_ncm_is_active(void) { return s_active; }
