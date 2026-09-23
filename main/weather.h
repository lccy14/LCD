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
    int      code;        /* WMO weather code */
    char     desc[28];    /* 天气英文描述 */
    int      humidity;    /* 相对湿度 % */
    float    wind;        /* 风速 km/h */
    int64_t  updated_ms;  /* 最后成功更新时间(ms) */
} weather_data_t;

/* 全局天气数据（由天气后台任务更新，UI 线程只读） */
extern weather_data_t g_weather;

/* 启动天气后台任务（建一次即可） */
void weather_init(void);

/* 请求拉取一次天气。city==NULL 使用默认城市（见 weather.c）。
 * 本函数仅置位请求并唤醒任务，不阻塞、不在调用上下文做网络。 */
void weather_request(const char *city);
