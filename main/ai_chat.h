#pragma once
#include <stdbool.h>
#include <stdint.h>

/* AI 聊天全局状态：后台任务写、UI 只读，靠 version 轮询（与天气/录音同构）。
 * reply 是最新一次助手回复（UTF-8），UI 只在 version 变化时重新渲染。 */
typedef struct {
    bool     busy;        /* 正在请求中 */
    bool     error;       /* 上次请求失败 */
    uint32_t version;     /* 每次请求结束 +1，UI 据此刷新 */
    char     reply[512];  /* 最新回复（约 170 汉字），PSRAM 任务写入；全链路统一 512 */
} ai_chat_state_t;

extern volatile ai_chat_state_t g_ai;

/* 初始化后台任务（建一次即可；栈在 PSRAM） */
void ai_chat_init(void);

/* 发起一次聊天。question 为用户输入，后台复制到 PSRAM 后异步发送，不阻塞。
 * 未配置 API Key 时会在 reply 里写提示并直接返回。 */
void ai_chat_ask(const char *question);

/* 可切换的服务商。默认 AI_PROVIDER_GLM（智谱 GLM-4-Flash 永久免费）。 */
typedef enum {
    AI_PROVIDER_GLM = 0,   /* 智谱：glm-4-flash，永久免费 */
    AI_PROVIDER_DEEPSEEK,  /* DeepSeek：deepseek-chat，低价 */
    AI_PROVIDER_SILICON,   /* 硅基流动：Qwen2.5-7B，9B 以下免费 */
    AI_PROVIDER_MAX
} ai_provider_t;

/* 把 NVS 里的平台/Key 同步到内存缓存。必须在内部 RAM 栈的任务里调用
 * （LVGL 任务 / app_main 均可）；PSRAM 栈任务禁止调 NVS，见 ai_chat.c 注释。 */
void ai_chat_cfg_sync(void);

/* 从内存缓存读当前 Key（供 PSRAM 栈任务用，绝不读 NVS）。未同步返回 false。 */
bool ai_chat_key_cached(char *out, size_t cap);

/* 当前服务商（存 NVS，掉电保留） */
ai_provider_t ai_chat_provider_get(void);
bool          ai_chat_provider_set(ai_provider_t p);
const char   *ai_chat_provider_name(ai_provider_t p);   /* 短名，给 UI 显示 */

/* API Key 存取（NVS 命名空间 "aichat"，键 "apikey"）。
 * 返回 true 表示成功。get 时若未配置，out 会被置为 ""。 */
bool ai_chat_key_get(char *out, size_t cap);
bool ai_chat_key_set(const char *key);
bool ai_chat_key_has(void);
