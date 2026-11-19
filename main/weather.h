#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool     valid;       /* 是否成功获取过至少一次 */
    bool     fetching;    /* 正在请求中 */
    bool     error;       /* 上次请求失败 */
    uint32_t version;     /* 每次成功更新 +1，UI 据此判断是否需要刷新 */
    char     city[40];    /* 城市名（英文） */
    float    temp;        /* 温度 °C */
    int      code;        /* 兼容字段（sojson 不用） */
    char     desc[28];    /* 天气中文描述（阴/晴/小雨） */
    int      humidity;    /* 相对湿度 % */
    float    wind;        /* 兼容字段（sojson 无精确风速，置0） */
    /* sojson 扩展字段：让天气页信息完整 */
    char     high[12];    /* 今日高温原文（如 "高温 21℃"） */
    char     low[12];     /* 今日低温原文 */
    char     wind_dir[12];/* 风向（如 "东北风"） */
    char     wind_level[8];/* 风力（如 "2级"） */
    char     air_quality[8];/* 空气质量（优/良） */
    int      pm25;        /* PM2.5 */
    char     sunrise[8];  /* 日出 "06:38" */
    char     sunset[8];   /* 日落 "17:39" */
    char     notice[48];  /* 生活提示 */
    int64_t  updated_ms;  /* 最后成功更新时间(ms) */
} weather_data_t;

/* 全局天气数据（由天气后台任务更新，UI 线程只读） */
extern weather_data_t g_weather;

/* 启动天气后台任务（建一次即可） */
void weather_init(void);

/* 请求拉取一次天气。city==NULL 使用默认城市（见 weather.c）。
 * 本函数仅置位请求并唤醒任务，不阻塞、不在调用上下文做网络。 */
void weather_request(const char *city);
