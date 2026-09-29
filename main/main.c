#include "ui.h"
#include "sd_card.h"

void app_main(void)
{
    /* 开机先挂 SD 卡：此刻 LVGL / WiFi / BLE / TLS 都还没申请内部 RAM，
     * SDMMC 需要的 DMA 内存充裕。放到打开「文件」App 时再挂会因为内部 RAM
     * 被吃光而报 ESP_ERR_NO_MEM（连 8 字节的 SCR 缓冲都申请不到）。
     * 失败不影响启动，「文件」App 里的「刷新」会重试。 */
    sd_card_mount(true);

    ui_app_start();
}
