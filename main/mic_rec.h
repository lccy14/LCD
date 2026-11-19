#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* ============================================================
 * 录音引擎：I2S 数字麦克风 → 16bit PCM WAV → /sdcard
 * ------------------------------------------------------------
 * - 走独立 I2S_NUM_1，与播放用的 I2S_NUM_0 完全隔离，互不干扰。
 * - 采样 16kHz / 16bit / 单声道（32KB/s，对 SD 卡友好），录完回填 WAV 头。
 * - 沿用项目「全局状态 + version 轮询」范式：后台任务写 g_mic，
 *   UI 按 version 变化刷新，不跨任务调用 LVGL，也不需要 LVGL 锁。
 * - 与播放互斥：开始录音会先停播放；录音中播放请求会被拒绝。
 * - 第一版 PCB 未引出这三个脚（IO1/2/42 被 SD 占用），功能自动禁用。
 * ============================================================ */

typedef struct {
    volatile uint32_t version;       /* 状态变化 +1，UI 据此刷新 */
    volatile bool     recording;     /* 正在录音 */
    volatile bool     done;          /* 刚录完一个文件，UI 消费后 ack */
    volatile uint32_t elapsed_ms;    /* 已录时长 */
    volatile uint32_t bytes_written; /* 已写入的 PCM 字节数 */
    volatile uint8_t  peak;          /* 最近一块的峰值电平 0..100 */
    volatile char     hint[40];      /* 提示/错误文本，非空时优先显示 */
    volatile char     last_path[128];/* 最近保存的 WAV 路径 */
} mic_state_t;

extern volatile mic_state_t g_mic;

/* 开机调用一次：分配 PSRAM 缓冲、建命令队列与后台任务。
 * 不在此处建 I2S 通道（内部 DMA 内存紧张），首次录音时才申请。 */
void mic_rec_init(void);

/* 本板是否支持录音（第一版 PCB 返回 false） */
bool mic_rec_available(void);

/* 开始录音：会先停掉播放。异步，立即返回 */
void mic_rec_start(void);
void mic_rec_stop(void);
void mic_rec_toggle(void);
bool mic_rec_is_recording(void);

/* UI 刷新完「录完」提示后调用，清掉 done 标志 */
void mic_rec_ack_done(void);
