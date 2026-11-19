#pragma once
#include <stdbool.h>
#include <stdint.h>

/* 语音识别（智谱 glm-asr-2512）：把 mic_rec 录好的 WAV 转成文字。
 * 后台任务 + version 轮询范式，与 ai_chat/weather 同构。
 * 复用 AI 聊天同一个智谱 Key（从 ai_chat 模块读，不重复存 NVS）。 */
typedef struct {
    volatile uint32_t version;    /* 状态变化 +1 */
    volatile bool     busy;       /* 识别中 */
    volatile bool     done;       /* 识别完成，UI 消费后 ack */
    volatile bool     error;
    volatile char     text[512];  /* 识别结果 UTF-8 */
    volatile char     hint[64];   /* 错误/状态提示 */
} voice_asr_state_t;

extern volatile voice_asr_state_t g_asr;

void voice_asr_init(void);
/* 对指定 WAV 文件发起识别，异步；path 为 /sdcard/xxx.wav */
void voice_asr_start(const char *wav_path);
bool voice_asr_is_busy(void);
void voice_asr_ack_done(void);
