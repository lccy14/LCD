#pragma once
#include <stdbool.h>
#include <time.h>

/* 连上 WiFi 拿到 IP 后调用一次即可（内部有幂等保护）。
 * 内部启动 SNTP 向 NTP 服务器校时，校时成功前 time() 返回的是 1970 基准。 */
void time_sync_start(void);

/* 是否已成功从 NTP 同步过时间 */
bool time_sync_is_done(void);

/* 把当前本地时间填入 tm（已按 CST-8 时区转换）。未同步则返回 false。 */
bool time_sync_localtime(struct tm *tm);
