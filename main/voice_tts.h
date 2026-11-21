#pragma once
#include <stdbool.h>
#include <stdint.h>

/* 语音合成（智谱 glm-tts）：把 AI 回复文字转成 WAV 并用功放播放。
 * 后台任务 + version 轮询范式，与 ai_chat/voice_asr 同构。
 * 复用 AI 聊天同一个智谱 Key（从缓存读，不碰 NVS）。
 * WAV 直接流式写 SD 卡，不占内存；播放交给 audio_player（命令队列）。 */
typedef struct {
    volatile uint32_t version;    /* 状态变化 +1 */
    volatile bool     busy;       /* 合成中 */
    volatile bool     error;
    volatile char     hint[48];   /* 错误/状态提示 */
    volatile char     path[160];  /* 生成的 WAV 路径，播放用 */
} voice_tts_state_t;

extern volatile voice_tts_state_t g_tts;

void voice_tts_init(void);
/* 合成并播放一段文字，异步；从 LVGL 任务/内部 RAM 任务调用，文字会拷到 PSRAM */
void voice_tts_speak(const char *text);
bool voice_tts_is_busy(void);
void voice_tts_stop(void);        /* 停止播放并取消当前合成 */
