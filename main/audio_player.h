#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* ============================================================
 * 音频播放引擎：WAV(PCM) / MP3(minimp3) → I2S → MAX98357AETE+T
 * ------------------------------------------------------------
 * - 解码在独立后台任务里跑（栈放 PSRAM），UI 线程只发命令、只读状态。
 * - 沿用项目 pc_mon.c 的「全局状态 + version 轮询」范式：
 *   后台任务写 g_audio 并 version++，UI 的 lv_timer 检测 version 变化再刷新，
 *   因此不需要 LVGL 锁，也不会跨任务调用 LVGL API。
 * - WAV：未压缩 PCM，8/16/24/32 位、单/双声道，时长由头精确计算、seek 精确。
 * - MP3：minimp3（公共领域单头文件，Layer III），支持 ID3v2/Xing/VBRI；
 *   时长优先用 VBR 帧数、否则按 CBR 码率估算，seek 为按字节的近似定位。
 * - FLAC/OGG/AAC 暂不支持，扫描时会被 audio_player_supported() 过滤掉。
 * ============================================================ */

typedef struct {
    volatile uint32_t version;      /* 每次状态变化 +1，UI 据此判断是否需要刷新 */
    volatile bool     playing;      /* 正在输出音频 */
    volatile bool     loaded;       /* 已成功打开一个文件 */
    volatile bool     eof;          /* 一曲播完，UI 消费后调 audio_player_ack_eof() */
    volatile uint32_t pos_ms;       /* 已播放毫秒（按实际送给 I2S 的字节数算） */
    volatile uint32_t dur_ms;       /* 总时长毫秒（WAV 精确；MP3 由 Xing 帧数或 CBR 码率估算） */
    volatile uint32_t sample_rate;  /* 采样率 Hz */
    volatile uint8_t  channels;     /* 源文件声道数 */
    volatile uint8_t  bits;         /* 源文件位深 */
    volatile uint8_t  volume;       /* 0..100，软件音量 */
    volatile char     hint[40];     /* 非空时显示给用户：错误/格式提示 */
} audio_state_t;

extern volatile audio_state_t g_audio;

/* 开机调用一次：建 I2S 通道、SD_MODE 引脚、后台任务。失败会打日志但不影响其它功能 */
void audio_player_init(void);

/* 打开并从头播放；路径为 FATFS 路径（/sdcard/xxx.wav 或 xxx.mp3） */
esp_err_t audio_player_play(const char *path);
void audio_player_pause(void);
void audio_player_resume(void);
void audio_player_stop(void);

/* pct_0_1000: 与 lv_slider 的 0..1000 直接对应 */
void audio_player_seek(int pct_0_1000);
void audio_player_set_volume(uint8_t pct_0_100);

/* UI 处理完 eof 后调用，避免重复触发「自动下一首」 */
void audio_player_ack_eof(void);

/* 供文件管理器等判断某扩展名能否播放 */
bool audio_player_supported(const char *name);
