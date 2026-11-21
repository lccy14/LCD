#include "ui.h"
#include "sd_card.h"
#include "audio_player.h"
#include "mic_rec.h"
#include "ai_chat.h"
#include "voice_asr.h"
#include "voice_tts.h"
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

    /* 音频播放引擎：此时 LVGL / 小说等还没占用内存，先申请 PSRAM 缓冲与后台任务栈。
     * 只建通道与任务，不播放；SD_MODE 保持关断，开机不会有噗声。 */
    audio_player_init();

    /* 录音引擎：只分配 PSRAM 缓冲与建后台任务，I2S1 RX 通道要等首次录音
     * 才申请（内部 DMA 内存紧张，平时不占）。同样排在 LVGL 之前。 */
    mic_rec_init();

    /* AI 聊天后台任务（栈/缓冲在 PSRAM），排在 LVGL 之前；
     * 未配置 Key 时不发请求，只占任务栈。 */
    ai_chat_init();

    /* 语音识别后台任务（栈/缓冲在 PSRAM），复用 AI 模块的智谱 Key */
    voice_asr_init();

    /* 语音合成后台任务（栈/缓冲在 PSRAM），复用智谱 Key + audio_player 播放 */
    voice_tts_init();

    ui_app_start();
}
