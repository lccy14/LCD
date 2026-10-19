#include "ui.h"
#include "sd_card.h"
#include "wifi_scan.h"   /* WiFi 初始化提前到 UI 之前：RAM 最空时连，避开进书把 DMA 吃光导致重试失败 */
#include "esp_log.h"     /* ESP_LOGE */

void app_main(void)
{
    /* 开机先挂 SD 卡：此刻 LVGL / WiFi / BLE / TLS 都还没申请内部 RAM，
     * SDMMC 需要的 DMA 内存充裕。放到打开「文件」App 时再挂会因为内部 RAM
     * 被吃光而报 ESP_ERR_NO_MEM（连 8 字节的 SCR 缓冲都申请不到）。
     * 失败不影响启动，「文件」App 里的「刷新」会重试。 */
    sd_card_mount(true);

    /* WiFi 必须在 LVGL / 小说屏占用内存之前就 init 并开始连接。
     * 实测：首次 auth 偶发失败需重试，若此时已进书把 DMA 吃到底(最低≈19)会全部
     * 重试失败而放弃；提前到这里，连接环境与「不进书」一致，重试能稳定连上。 */
    if (wifi_scan_init() != ESP_OK) {
        ESP_LOGE("WIFI", "wifi init failed");
    }

    ui_app_start();
}
