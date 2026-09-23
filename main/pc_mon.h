#pragma once
#include <stdbool.h>
#include <stdint.h>

/* 电脑监控：ESP32 通过 TCP 从 PC 端服务(pc_mon_server.py)读取 AIDA64 传感器。
 * 协议：服务端每条连接发送若干行 "label|value|unit\n"，发完即关闭。 */

#define PC_MON_MAX_SENSORS 40
#define PC_MON_LABEL_LEN   32
#define PC_MON_VALUE_LEN   16
#define PC_MON_UNIT_LEN    8

typedef struct {
    char label[PC_MON_LABEL_LEN];
    char value[PC_MON_VALUE_LEN];
    char unit[PC_MON_UNIT_LEN];
} pc_sensor_t;

typedef struct {
    bool     valid;       /* 是否成功获取过至少一次 */
    bool     error;       /* 上次连接/读取失败 */
    uint32_t version;     /* 每次成功或失败更新 +1，UI 据此判断是否需要刷新 */
    int      count;       /* 当前传感器条数 */
    pc_sensor_t sensors[PC_MON_MAX_SENSORS];
    int64_t  updated_ms;  /* 最后成功更新时间(ms) */
} pc_mon_data_t;

/* 全局数据（由 pc_mon 后台任务更新，UI 线程只读） */
extern pc_mon_data_t g_pcmon;

/* 启动后台 TCP 拉取任务（建一次即可） */
void pc_mon_init(void);

/* UI 调用：进入「电脑监控」界面时传 true 开始拉数据，离开时传 false 停止。
 * 停止后任务阻塞挂起，不再联网也不占 CPU；进入时会立即拉一次，不等待间隔。 */
void pc_mon_set_active(bool active);
