#include "ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lcd.h"
#include "lcd_init.h"
#include "lcdfont.h"
#include "lvgl_port.h"
#include "ft6336.h"
#include "wifi_scan.h"
#include "weather.h"
#include "pc_mon.h"
#include "time_sync.h"
#include "alarm.h"
#include "ble_scan.h"
#include "esp_wifi.h"
#include "driver/ledc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_heap_caps.h"   /* heap_caps_malloc PSRAM for prerender_home_bg */
#include "novel_text.h"

/* LVGL 主任务句柄, 供 lvgl_port.c 的 DMA 完成中断回调 xTaskNotifyGive 唤醒主循环。
 * 替代 vTaskDelay 固定 10ms 等待: DMA 完成立刻唤醒, 滑动 FPS 46→80+。 */
TaskHandle_t g_lvgl_task = NULL;

/* 中文字库：lv_font_cn_16.c 由 lv_font_conv 生成（16px / bpp4），
 * 覆盖 ASCII 32-127 + 界面所需的全部汉字，英文字形也来自该字体。
 * 注意：LVGL 内置 montserrat 不含汉字，凡是要显示中文的标签都必须显式设置为它。 */
LV_FONT_DECLARE(lv_font_cn_16);
/* degree 字体只含「°」一个符号，并以 lv_font_cn_16 作回退；
 * 用它作界面字体即可在中文之外正常显示「°C」，无需改动中文字库。 */
LV_FONT_DECLARE(degree);

/* 主界面背景图：由 tools/img2lvgl_rgb565.py 把 PNG 转成 RGB565 未压缩数据
 * （320×240×2 = 150KB，存在 Flash 的 rodata，不占 RAM；LVGL 直接 blit，无需运行时解码）。
 * 换背景时重跑该脚本覆盖 main/bg_home.c 即可，这里不用改。 */
LV_IMAGE_DECLARE(bg_home_img);

/* App 图标：Google Material Icons (Apache 2.0) 的 A8 位图，数据见 main/app_icons.c。
 * A8 只存透明通道（40×40×1 = 1.6KB/个），显示时用 image_recolor 染成白色，
 * 继续压在原来的彩色圆角方块上，保持现有设计语言。 */
LV_IMAGE_DECLARE(ico_wifi);
LV_IMAGE_DECLARE(ico_settings);
LV_IMAGE_DECLARE(ico_clock);
LV_IMAGE_DECLARE(ico_music);
LV_IMAGE_DECLARE(ico_game);
LV_IMAGE_DECLARE(ico_weather);
LV_IMAGE_DECLARE(ico_novel);
LV_IMAGE_DECLARE(ico_pcmon);
LV_IMAGE_DECLARE(ico_bluetooth);

void lcd_test(void)
{
    LCD_Init();

    // 清屏 + 纯色渐变测试
    LCD_Fill(0, 0, LCD_W, LCD_H, BLACK);
    LCD_Flush_All();
    vTaskDelay(1000 / portTICK_PERIOD_MS);
    LCD_Fill(0, 0, LCD_W, LCD_H, RED);
    LCD_Flush_All();
    vTaskDelay(500 / portTICK_PERIOD_MS);
    LCD_Fill(0, 0, LCD_W, LCD_H, GREEN);
    LCD_Flush_All();
    vTaskDelay(500 / portTICK_PERIOD_MS);
    LCD_Fill(0, 0, LCD_W, LCD_H, BLUE);
    LCD_Flush_All();
    vTaskDelay(500 / portTICK_PERIOD_MS);
    LCD_Fill(0, 0, LCD_W, LCD_H, WHITE);
    LCD_Flush_All();
    vTaskDelay(500 / portTICK_PERIOD_MS);
    LCD_Fill(0, 0, LCD_W, LCD_H, BLACK);
    LCD_Flush_All();

    // 文字显示
    LCD_ShowString(20, 10, (const u8 *)"LCD Test", WHITE, BLACK, 24, 0);
    LCD_ShowString(20, 40, (const u8 *)"ESP32 SPI LCD", WHITE, BLACK, 16, 0);
    LCD_ShowString(20, 65, (const u8 *)"Resolution:", WHITE, BLACK, 16, 0);
    LCD_ShowIntNum(130, 65, LCD_W, 3, YELLOW, BLACK, 16);
    LCD_ShowString(160, 65, (const u8 *)"x", WHITE, BLACK, 16, 0);
    LCD_ShowIntNum(180, 65, LCD_H, 3, YELLOW, BLACK, 16);
    LCD_Flush_All();

    vTaskDelay(2000 / portTICK_PERIOD_MS);
    LCD_Fill(0, 0, LCD_W, LCD_H, BLACK);
    LCD_Flush_All();

    // 图形测试
    LCD_DrawLine(10, 10, LCD_W - 10, 10, RED);
    LCD_DrawLine(LCD_W - 10, 10, LCD_W - 10, LCD_H - 10, GREEN);
    LCD_DrawLine(LCD_W - 10, LCD_H - 10, 10, LCD_H - 10, BLUE);
    LCD_DrawLine(10, LCD_H - 10, 10, 10, CYAN);

    LCD_DrawRectangle(20, 20, LCD_W - 20, LCD_H - 20, YELLOW);
    Draw_Circle(LCD_W / 2, LCD_H / 2, 30, MAGENTA);
    LCD_Flush_All();

    vTaskDelay(2000 / portTICK_PERIOD_MS);
    LCD_Fill(0, 0, LCD_W, LCD_H, BLACK);
    LCD_Flush_All();

    // 颜色块循环 + 实时 FPS（绘制写 framebuffer，每帧全屏 flush）
    u16 fps = 0;
    while (1)
    {
        int64_t t0 = esp_timer_get_time();

        LCD_Fill(0, 0, LCD_W, LCD_H, BLACK);
        LCD_ShowString(20, 20, (const u8 *)"Color Test:", WHITE, BLACK, 24, 0);

        LCD_Fill(20, 50, 50, 80, RED);
        LCD_Fill(70, 50, 100, 80, GREEN);
        LCD_Fill(120, 50, 150, 80, BLUE);
        LCD_Fill(170, 50, 200, 80, YELLOW);
        LCD_Fill(220, 50, 240, 80, CYAN);

        LCD_Fill(20, 100, 50, 130, MAGENTA);
        LCD_Fill(70, 100, 100, 130, BROWN);
        LCD_Fill(120, 100, 150, 130, GRAY);
        LCD_Fill(170, 100, 200, 130, DARKBLUE);
        LCD_Fill(220, 100, 240, 130, LIGHTGREEN);

        // 显示上一帧测得的 fps（避免自举：本帧 fps 要等 flush 完才知道）
        LCD_ShowString(LCD_W - 90, LCD_H - 24, (const u8 *)"FPS:", WHITE, BLACK, 16, 0);
        LCD_ShowIntNum(LCD_W - 40, LCD_H - 24, fps, 3, YELLOW, BLACK, 16);

        // 真正耗时的 SPI 发送放进计时区间
        LCD_Flush_All();

        fps = (u16)(1000000 / (esp_timer_get_time() - t0));
    }
}

#if USE_LVGL
/* ----------------------------- LVGL 示例 UI ----------------------------- */

/* 详情页对象（回调里需访问，故文件级） */
static lv_obj_t *g_detail = NULL;
static lv_obj_t *g_detail_label = NULL;

/* 网络列表容器 + 副标题 */
static lv_obj_t *g_net_cont = NULL;
static lv_obj_t *g_subtitle = NULL;
static bool      g_wifi_on = true;

/* 状态栏上的 WiFi 连接状态指示。
 * 每个界面都有自己的状态栏（make_status_bar 会被复用），所以这里保存所有实例统一刷新。 */
#define WIFI_ICON_MAX 8
static lv_obj_t      *g_wifi_icons[WIFI_ICON_MAX];
static int            g_wifi_icon_cnt = 0;
static bool           g_wifi_icons_dirty = true;

/* 状态栏时钟标签：每个界面都复用 make_status_bar，故会有多个实例，统一刷新 */
#define STATUS_CLK_MAX 8
static lv_obj_t      *g_status_clocks[STATUS_CLK_MAX];
static int            g_status_clock_cnt = 0;
static wifi_sta_state_t g_last_wifi_state = WIFI_STA_IDLE;

/* WiFi 详情页：当前显示的 AP、状态文字、连接按钮 */
static int       g_detail_ap_idx = -1;
static lv_obj_t *g_detail_status = NULL;
static lv_obj_t *g_detail_btn    = NULL;

/* 密码输入面板：首次用到时才创建（键盘有几十个按钮，建屏时一起建会拖慢首次进入） */
static lv_obj_t *g_pwd_panel = NULL;
static lv_obj_t *g_pwd_ta    = NULL;
static lv_obj_t *g_pwd_kb    = NULL;
static lv_obj_t *g_pwd_ssid  = NULL;
static bool      g_pending_pwd = false;
static char      g_pending_ssid[33];

/* ---------------- BLE (蓝牙) 界面状态 ---------------- */
#define BLE_LIST_ROWS 24
typedef struct {
    lv_obj_t *row;
    lv_obj_t *sig;
    lv_obj_t *name;
    int       dev_idx;
} ble_row_t;
static ble_row_t g_ble_rows[BLE_LIST_ROWS];
static lv_obj_t *g_ble_detail = NULL;
static lv_obj_t *g_ble_detail_label = NULL;
static lv_obj_t *g_ble_detail_status = NULL;
static lv_obj_t *g_ble_detail_btn = NULL;
static int       g_ble_detail_idx = -1;

/* 各屏幕对象（用于界面跳转） */
static lv_obj_t *g_home = NULL;
static lv_obj_t *g_wifi_screen = NULL;
static lv_obj_t *g_ble_screen = NULL;
static lv_obj_t *g_settings_screen = NULL;
static lv_obj_t *g_clock_screen = NULL;
static lv_obj_t *g_alarm_screen = NULL;
static lv_obj_t *g_music_screen = NULL;
/* 2048 小游戏 */
static lv_obj_t *g_game_screen = NULL;
static lv_obj_t *g_board_bg = NULL;          /* 棋盘底框（tile 的父容器） */
static lv_obj_t *g_tile_obj[16];             /* 每个棋盘格对应的数字方块对象（NULL=空） */
static lv_obj_t *g_tile_lbl[16];             /* 方块上的数字标签 */
static lv_obj_t *g_cell_bg[16];              /* 每个格子固定的背景框（空格也显示） */
static int       g_gap = 6;                  /* 格子间隙（像素） */
static int       g_cell = 34;                /* 格子边长（像素） */
static lv_obj_t *g_game_score_label = NULL;
static uint16_t  g_board[16];      /* 0=空，其余为 2 的幂次（2,4,8...） */
static uint32_t  g_game_score = 0;
static lv_obj_t *g_weather_screen = NULL;

/* Clock 屏与 Weather 屏里的动态标签 */
static lv_obj_t *g_clock_time = NULL;
static lv_obj_t *g_clock_date = NULL;

/* 闹钟：时钟屏上的设置行，以及到点时弹出的顶层浮层（任何界面都能盖在最前面） */
static lv_obj_t *g_alarm_label     = NULL;
static lv_obj_t *g_alarm_popup     = NULL;
static lv_obj_t *g_alarm_popup_box = NULL;
static lv_obj_t *g_alarm_popup_msg = NULL;
static lv_obj_t *g_wx_city    = NULL;
static lv_obj_t *g_wx_temp    = NULL;
static lv_obj_t *g_wx_desc    = NULL;
static lv_obj_t *g_wx_extra   = NULL;
static lv_obj_t *g_wx_updated = NULL;

/* 电脑监控屏：传感器行标签池（最多 PC_MON_MAX_SENSORS 行，懒加载创建一次） */
static lv_obj_t *g_pcmon_screen = NULL;
static lv_obj_t *g_pcmon_labels[PC_MON_MAX_SENSORS];
static lv_obj_t *g_pcmon_status = NULL;
static uint32_t  s_pcmon_shown_ver = 0;

/* 主循环里用的状态：避免每帧重绘/重复请求 */
static uint32_t s_wx_shown_ver    = 0;          /* 已显示到界面上的天气版本号 */
static bool     s_wx_fetching     = false;      /* 上次界面显示的“请求中”状态 */
static uint32_t s_last_wx_req_ms  = 0;          /* 上次发天气请求的 tick(ms) */
static bool     s_wx_need_redraw  = false;      /* 进入天气屏时置位，强制重绘一次 */
static uint8_t  s_prev_wifi_state = WIFI_STA_IDLE;

/* WiFi 列表固定行池：只在初始化时创建一次，扫描结果出来后原地更新，
 * 不在主循环里反复 lv_obj_create，避免对象爆炸导致看门狗复位 */
#define WIFI_LIST_ROWS 24
typedef struct {
    lv_obj_t *row;
    lv_obj_t *sig;     // 信号强度单条指示（高度表示档位）
    lv_obj_t *lock;    // 加密指示（单个圆角小块）
    lv_obj_t *name;
    int       ap_idx;
} wifi_row_t;
static wifi_row_t g_rows[WIFI_LIST_ROWS];

/* 待跳转的 App（点击后不在事件回调里同步建屏，避免重入 + 一次性建大量对象卡死看门狗） */
static int g_pending_app = -1;

/* 触摸相关：FT6336 实时坐标 + 屏幕上的红色指示点 */
static lv_obj_t      *g_red_dot = NULL;
static int32_t       g_touch_x = 0;
static int32_t       g_touch_y = 0;
static bool          g_touch_pressed = false;

/* LVGL 输入设备回调：读取 FT6336，把坐标喂给 LVGL。
 * 优化: 先查 INT 引脚电平, 高=无触摸立即返回, 避免每帧阻塞读 I2C。
 * 滑动响应延迟从 ~1ms 降到 ~1µs, 解决滑动卡顿。 */
static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;

    /* INT 高 = 无触摸, 立即返回 released, 不读 I2C */
    if (!ft6336_touched()) {
        g_touch_pressed = false;
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }

    touch_point_t tp;
    if (ft6336_read(&tp) == ESP_OK && tp.pressed) {
        /* 坐标裁剪到屏幕范围（FT6336 原生范围可能比 320x240 大，需按屏裁剪） */
        /* 触摸坐标定义：tp.y = 屏幕横向(0~320,屏宽), tp.x = 屏幕纵向(0~240,屏高)
         * 直接互换轴、不取反 */
        int32_t sx = (int32_t)tp.y;   // 屏 X = 触摸 Y
        int32_t sy = (int32_t)tp.x;   // 屏 Y = 触摸 X
        if (sx < 0) { sx = 0; }
        if (sx >= LCD_W) { sx = LCD_W - 1; }
        if (sy < 0) { sy = 0; }
        if (sy >= LCD_H) { sy = LCD_H - 1; }
        g_touch_x = sx;
        g_touch_y = sy;
        g_touch_pressed = true;
        data->point.x = sx;
        data->point.y = sy;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        g_touch_pressed = false;
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

/* Wi-Fi 开关：开 -> 触发扫描；关 -> 清空列表 */
static void switch_cb(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    g_wifi_on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    if (g_wifi_on) {
        wifi_scan_trigger();
        lv_label_set_text(g_subtitle, "扫描中...");
    } else {
        lv_label_set_text(g_subtitle, "无线已关闭");
        for (int i = 0; i < WIFI_LIST_ROWS; i++) {
            lv_obj_add_flag(g_rows[i].row, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

/* 刷新所有状态栏上的 WiFi 图标 + WiFi 界面副标题。
 * 只在状态变化时改样式，避免每帧都触发重绘。 */
static void wifi_status_update(void)
{
    wifi_sta_state_t st = g_wifi_state;
    if (!g_wifi_icons_dirty && st == g_last_wifi_state) return;
    g_wifi_icons_dirty = false;
    g_last_wifi_state = st;

    uint32_t c;
    switch (st) {
    case WIFI_STA_CONNECTED:  c = 0x35D04A; break;   /* 绿：已连接 */
    case WIFI_STA_CONNECTING: c = 0xE0B02B; break;   /* 黄：连接中 */
    case WIFI_STA_FAILED:     c = 0xE0453B; break;   /* 红：失败 */
    default:                  c = 0x55556A; break;   /* 灰：未连接 */
    }
    /* 已连接时把 SSID 直接显示在图标旁边（截断到 12 字符，避免挤到电池图标） */
    char txt[40];
    if (st == WIFI_STA_CONNECTED && g_wifi_ssid[0] != '\0') {
        snprintf(txt, sizeof(txt), LV_SYMBOL_WIFI " %.12s", g_wifi_ssid);
    } else {
        snprintf(txt, sizeof(txt), "%s", LV_SYMBOL_WIFI);
    }
    for (int i = 0; i < g_wifi_icon_cnt; i++) {
        lv_obj_set_style_text_color(g_wifi_icons[i], lv_color_hex(c), 0);
        lv_label_set_text(g_wifi_icons[i], txt);
    }

    /* 副标题优先显示连接状态，空闲时才显示扫描到的网络数。
     * 仅在“主界面”或“Wi-Fi 界面”才改写副标题，避免覆盖其它 App 屏
     * （如 Weather/Clock）自己的标题。 */
    lv_obj_t *active = lv_screen_active();
    bool on_wifi_ui = (active == g_home) ||
                      (g_wifi_on && g_wifi_screen && active == g_wifi_screen);
    if (g_subtitle && on_wifi_ui) {
        if (st == WIFI_STA_CONNECTED) {
            lv_label_set_text_fmt(g_subtitle, "已连接  %s", g_wifi_ip);
        } else if (st == WIFI_STA_CONNECTING) {
            lv_label_set_text_fmt(g_subtitle, "正在连接 %s...", g_wifi_ssid);
        } else if (st == WIFI_STA_FAILED) {
            lv_label_set_text(g_subtitle, "连接失败");
        } else {
            lv_label_set_text_fmt(g_subtitle, "找到 %d 个网络", g_ap_count);
        }
    }
}

/* 刷新详情页的状态文字与按钮（连接中/已连接/未连接） */
static void update_detail_ui(void)
{
    if (!g_detail || !g_detail_status || !g_detail_btn) return;
    if (lv_obj_has_flag(g_detail, LV_OBJ_FLAG_HIDDEN)) return;
    if (g_detail_ap_idx < 0 || g_detail_ap_idx >= g_ap_count) return;

    const char *ssid = g_ap_list[g_detail_ap_idx].ssid;
    wifi_sta_state_t st = g_wifi_state;
    bool is_current = (g_wifi_ssid[0] != '\0') && (strncmp(g_wifi_ssid, ssid, 32) == 0);

    char status[64];
    const char *btn;

    if (st == WIFI_STA_CONNECTED && is_current) {
        snprintf(status, sizeof(status), "已连接\nIP: %s", g_wifi_ip);
        btn = "断开";
    } else if (st == WIFI_STA_CONNECTING && is_current) {
        snprintf(status, sizeof(status), "正在连接...");
        btn = "取消";
    } else if (st == WIFI_STA_FAILED && is_current) {
        snprintf(status, sizeof(status), "失败 - 请检查密码");
        btn = "连接";
    } else {
        snprintf(status, sizeof(status), "未连接");
        btn = "连接";
    }

    /* 内容没变就别写：主循环每圈都会调用，无条件 set_text 会导致每帧重绘 */
    static char s_last_status[64] = "";
    static char s_last_btn[16]    = "";
    if (strcmp(status, s_last_status) != 0) {
        lv_label_set_text(g_detail_status, status);
        strncpy(s_last_status, status, sizeof(s_last_status) - 1);
        s_last_status[sizeof(s_last_status) - 1] = '\0';
    }
    if (strcmp(btn, s_last_btn) != 0) {
        lv_label_set_text(lv_obj_get_child(g_detail_btn, 0), btn);
        strncpy(s_last_btn, btn, sizeof(s_last_btn) - 1);
        s_last_btn[sizeof(s_last_btn) - 1] = '\0';
    }
}

/* 详情页连接按钮：开放网络直接连，加密网络弹出密码面板 */
static void connect_btn_cb(lv_event_t *e)
{
    (void)e;
    if (g_detail_ap_idx < 0 || g_detail_ap_idx >= g_ap_count) return;

    const char *ssid = g_ap_list[g_detail_ap_idx].ssid;
    bool is_current = (g_wifi_ssid[0] != '\0') && (strncmp(g_wifi_ssid, ssid, 32) == 0);
    wifi_sta_state_t st = g_wifi_state;

    /* 已连上 / 正在连这个网络 -> 按钮当断开用 */
    if (is_current && (st == WIFI_STA_CONNECTED || st == WIFI_STA_CONNECTING)) {
        wifi_app_disconnect();
        return;
    }

    if (g_ap_list[g_detail_ap_idx].authmode == WIFI_AUTH_OPEN) {
        wifi_app_connect(ssid, "");
    } else {
        /* 只置标志，面板的创建放到主循环里做（键盘按钮多，避免在输入回调里建对象卡看门狗） */
        strncpy(g_pending_ssid, ssid, sizeof(g_pending_ssid) - 1);
        g_pending_ssid[sizeof(g_pending_ssid) - 1] = '\0';
        g_pending_pwd = true;
    }
}

/* 点击某个 AP 项：打开详情页 */
static void ap_item_cb(lv_event_t *e)
{
    int row_i = (int)(intptr_t)lv_event_get_user_data(e);
    if (row_i < 0 || row_i >= WIFI_LIST_ROWS) return;
    int idx = g_rows[row_i].ap_idx;
    if (idx < 0 || idx >= g_ap_count) return;

    ap_info_t *ap = &g_ap_list[idx];
    g_detail_ap_idx = idx;
    char mac[18];
    snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X",
             ap->bssid[0], ap->bssid[1], ap->bssid[2],
             ap->bssid[3], ap->bssid[4], ap->bssid[5]);

    const char *sec = (ap->authmode == WIFI_AUTH_OPEN) ? "Open"
                    : (ap->authmode == WIFI_AUTH_WEP)  ? "WEP"
                    : (ap->authmode == WIFI_AUTH_WPA_PSK)    ? "WPA"
                    : (ap->authmode == WIFI_AUTH_WPA2_PSK)   ? "WPA2"
                    : (ap->authmode == WIFI_AUTH_WPA_WPA2_PSK) ? "WPA/WPA2"
                    : (ap->authmode == WIFI_AUTH_WPA3_PSK)   ? "WPA3"
                    : (ap->authmode == WIFI_AUTH_WPA2_WPA3_PSK) ? "WPA2/WPA3"
                    : "?";

    char buf[256];
    snprintf(buf, sizeof(buf),
             "SSID : %s\n"
             "RSSI : %ddBm\n"
             "Ch   : %u\n"
             "Sec  : %s\n"
             "MAC  : %s",
             ap->ssid[0] ? ap->ssid : "<hidden>", ap->rssi,
             ap->channel, sec, mac);

    lv_label_set_text(g_detail_label, buf);
    lv_obj_clear_flag(g_detail, LV_OBJ_FLAG_HIDDEN);
    update_detail_ui();   /* 必须在取消隐藏之后调用 */
}

/* 返回按钮：关闭详情页 */
static void back_btn_cb(lv_event_t *e)
{
    lv_obj_t *d = (lv_obj_t *)lv_event_get_user_data(e);
    lv_obj_add_flag(d, LV_OBJ_FLAG_HIDDEN);
}

/* 状态栏电池图标 */
static lv_obj_t *make_battery(lv_obj_t *parent)
{
    lv_obj_t *cont = lv_obj_create(parent);
    lv_obj_set_size(cont, 30, 14);
    lv_obj_set_style_bg_opa(cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(cont, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_width(cont, 1, 0);
    lv_obj_set_style_radius(cont, 2, 0);
    lv_obj_t *nub = lv_obj_create(cont);
    lv_obj_set_size(nub, 2, 4);
    lv_obj_align(nub, LV_ALIGN_RIGHT_MID, 2, 0);
    lv_obj_set_style_bg_color(nub, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_opa(nub, LV_OPA_TRANSP, 0);
    lv_obj_t *fill = lv_obj_create(cont);
    lv_obj_set_size(fill, 22, 8);
    lv_obj_align(fill, LV_ALIGN_LEFT_MID, 2, 0);
    lv_obj_set_style_bg_color(fill, lv_color_hex(0x35D04A), 0);
    lv_obj_set_style_border_opa(fill, LV_OPA_TRANSP, 0);
    return cont;
}

/* 前向声明：这几个函数在文件后面定义，open_screen_cb 会用到 */
static void build_wifi_screen(lv_obj_t *scr);
static lv_obj_t *make_app_screen(const char *title);
static void add_placeholder(lv_obj_t *scr, const char *msg);
static void build_pcmon_screen(lv_obj_t *scr);
static void build_alarm_screen(lv_obj_t *scr);
static void build_ble_screen(lv_obj_t *scr);

/* 返回主界面 */
static void go_home_cb(lv_event_t *e)
{
    (void)e;
    pc_mon_set_active(false);   /* 离开 App 界面，停止监控数据拉取 */
    lv_screen_load(g_home);
}

/* App 编号，用于懒加载各屏幕 */
typedef enum {
    APP_WIFI = 1,
    APP_SETTINGS,
    APP_CLOCK,
    APP_MUSIC,
    APP_GAME,
    APP_WEATHER,
    APP_NOVEL,
    APP_PCMON,
    APP_ALARM,
    APP_BLE
} app_id_t;

/* 打开某个 App 屏幕：仅记录待跳转 id，真正的建屏/加载放到主循环里执行。
 * 不能在这里同步建屏——此回调运行在 lv_timer_handler 的输入事件里，
 * 重入时一次性创建几百个 LVGL 对象会卡 >5s 触发看门狗复位。 */
static void open_screen_cb(lv_event_t *e)
{
    int id = (int)(intptr_t)lv_event_get_user_data(e);
    g_pending_app = id;
}

/* 顶部状态栏（时间 + 电池），可复用 */
static void make_status_bar(lv_obj_t *parent)
{
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_set_size(bar, LCD_W, 22);
    lv_obj_align(bar, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x05050A), 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_border_opa(bar, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(bar, 8, 0);
    lv_obj_t *clk = lv_label_create(bar);
    lv_label_set_text(clk, "--:--");   /* 占位，连上网校时后由主循环刷新为真实本地时间 */
    lv_obj_set_style_text_font(clk, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(clk, lv_color_white(), 0);
    if (g_status_clock_cnt < STATUS_CLK_MAX) {
        g_status_clocks[g_status_clock_cnt++] = clk;
    }

    /* WiFi 连接状态指示（颜色由 wifi_status_update 按连接状态改：
     * 灰=未连接 黄=连接中 绿=已连接 红=失败） */
    lv_obj_t *w = lv_label_create(bar);
    lv_label_set_text(w, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_font(w, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(w, lv_color_hex(0x55556A), 0);
    if (g_wifi_icon_cnt < WIFI_ICON_MAX) {
        g_wifi_icons[g_wifi_icon_cnt++] = w;
        g_wifi_icons_dirty = true;   /* 让新图标在下一次刷新时拿到正确颜色 */
    }

    make_battery(bar);
}

/* 通用 App 屏幕框架：状态栏 + 返回栏（标题） */
static lv_obj_t *make_app_screen(const char *title)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101018), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(scr, 0, 0);
    lv_obj_set_style_border_opa(scr, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);   // 固定界面，不滚动
    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);
    /* 状态栏由 lvgl_ui_bootstrap 在 lv_layer_top() 上统一创建，不随 screen 切换/滚动 */

    lv_obj_t *bar = lv_obj_create(scr);
    lv_obj_set_size(bar, LCD_W, 40);
    lv_obj_align(bar, LV_ALIGN_TOP_LEFT, 0, 22);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x16161F), 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_border_opa(bar, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(bar, 8, 0);
    lv_obj_set_style_pad_column(bar, 8, 0);
    lv_obj_t *bk = lv_button_create(bar);
    lv_obj_set_size(bk, 80, 32);
    lv_obj_t *bkl = lv_label_create(bk);
    lv_label_set_text(bkl, "< 返回");
    lv_obj_set_style_text_font(bkl, &lv_font_cn_16, 0);
    lv_obj_center(bkl);
    lv_obj_add_event_cb(bk, go_home_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *t = lv_label_create(bar);
    lv_label_set_text(t, title);
    lv_obj_set_style_text_font(t, &lv_font_cn_16, 0);
    lv_obj_set_style_text_color(t, lv_color_white(), 0);
    lv_obj_set_style_pad_left(t, 10, 0);
    return scr;
}

/* ---------------- 背光亮度：LEDC PWM 驱动 LCD_BLK 引脚 ---------------- */
/* BLK 原来是普通 GPIO 常亮，这里改成 PWM 输出，占空比即亮度 */
#define BLK_PWM_FREQ_HZ   1000
#define BLK_PWM_RES       LEDC_TIMER_10_BIT                 /* 分辨率 10bit -> 0..1023 */
#define BLK_MAX_DUTY      ((1u << LEDC_TIMER_10_BIT) - 1u)
#define BLK_MIN_PERCENT   10        /* 最低亮度，避免调到 0 后屏幕全黑 */
#define BLK_DEFAULT_PERCENT 100

static uint8_t   g_brightness = BLK_DEFAULT_PERCENT;
static lv_obj_t *g_br_label   = NULL;   /* 百分比数字标签 */

/* 设置背光亮度（percent: BLK_MIN_PERCENT ~ 100） */
static void backlight_set(uint8_t percent)
{
    if (percent < BLK_MIN_PERCENT) percent = BLK_MIN_PERCENT;
    if (percent > 100) percent = 100;
    g_brightness = percent;
    uint32_t duty = (uint32_t)BLK_MAX_DUTY * percent / 100;
    /* 注意：不能用 ledc_set_duty_and_update()！
     * 它属于 fade 系列的线程安全 API，内部会先检查 fade 服务是否已安装，
     * 未安装时会直接返回 ESP_FAIL 并打印 "Fade service not installed"，
     * 占空比根本不会写入（readback 一直是初始值）。
     * ledc_set_duty + ledc_update_duty 走的是非 fade 通路，不需要 fade 服务。 */
    esp_err_t e1 = ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    esp_err_t e2 = ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    /* 注：不要在这里用 ledc_get_duty() 做校验。它直接读硬件 duty 寄存器，
     * 而新占空比要等下一个 PWM 周期才生效，紧跟着读必然读到「上一次」的值，
     * 看起来像没写进去，其实是正常现象。用示波器看波形最准。 */
    if (e1 != ESP_OK || e2 != ESP_OK) {
        ESP_LOGE("BLK", "duty %lu failed (set=%d upd=%d)", (unsigned long)duty, (int)e1, (int)e2);
    }
}

/* 初始化 LEDC，把 LCD_BLK 接到 PWM 通道上 */
static void backlight_init(void)
{
    ledc_timer_config_t tcfg = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .duty_resolution = BLK_PWM_RES,
        .timer_num       = LEDC_TIMER_0,
        .freq_hz         = BLK_PWM_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&tcfg));

    /* 注意：intr_type 在新版 IDF 已废弃，不要赋值，否则会有编译告警 */
    ledc_channel_config_t ccfg = {
        .gpio_num   = LCD_BLK_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = LEDC_CHANNEL_0,
        .timer_sel  = LEDC_TIMER_0,
        .duty       = BLK_MAX_DUTY,      /* 默认最亮 */
        .hpoint     = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ccfg));
    ESP_LOGI("BLK", "LEDC ready: GPIO%d, %luHz, %dbit",
             (int)LCD_BLK_GPIO,
             (unsigned long)ledc_get_freq(LEDC_LOW_SPEED_MODE, LEDC_TIMER_0),
             (int)LEDC_TIMER_10_BIT);
}

/* 亮度滑块回调 */
static void brightness_cb(lv_event_t *e)
{
    lv_obj_t *sl = lv_event_get_target(e);
    int v = lv_slider_get_value(sl);
    backlight_set((uint8_t)v);
    if (g_br_label) {
        lv_label_set_text_fmt(g_br_label, "%d%%", v);
    }
}

/* 设置界面：状态栏 + 返回栏（复用 make_app_screen）+ 亮度滑动条 */
static void build_settings_screen(lv_obj_t *scr)
{
    lv_obj_t *cap = lv_label_create(scr);
    lv_label_set_text(cap, "亮度");
    lv_obj_set_style_text_font(cap, &lv_font_cn_16, 0);
    lv_obj_set_style_text_color(cap, lv_color_white(), 0);
    lv_obj_set_pos(cap, 16, 76);

    g_br_label = lv_label_create(scr);
    lv_label_set_text_fmt(g_br_label, "%d%%", g_brightness);
    lv_obj_set_style_text_font(g_br_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(g_br_label, lv_color_hex(0x9AA0B5), 0);
    lv_obj_set_width(g_br_label, 60);
    lv_obj_set_style_text_align(g_br_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(g_br_label, 244, 76);

    lv_obj_t *sl = lv_slider_create(scr);
    lv_obj_set_size(sl, 288, 20);
    lv_obj_set_pos(sl, 16, 116);
    lv_slider_set_range(sl, BLK_MIN_PERCENT, 100);
    lv_slider_set_value(sl, g_brightness, LV_ANIM_OFF);
    lv_obj_set_scrollbar_mode(sl, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_event_cb(sl, brightness_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* 两端提示文字 */
    lv_obj_t *mn = lv_label_create(scr);
    lv_label_set_text(mn, "10%");
    lv_obj_set_style_text_font(mn, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(mn, lv_color_hex(0x9AA0B5), 0);
    lv_obj_set_pos(mn, 16, 142);

    lv_obj_t *mx = lv_label_create(scr);
    lv_label_set_text(mx, "100%");
    lv_obj_set_style_text_font(mx, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(mx, lv_color_hex(0x9AA0B5), 0);
    lv_obj_set_pos(mx, 268, 142);
}

/* 在 App 屏幕中央放一行占位文字 */
static void add_placeholder(lv_obj_t *scr, const char *msg)
{
    lv_obj_t *l = lv_label_create(scr);
    lv_label_set_text(l, msg);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(l, LCD_W - 40);
    lv_obj_set_style_text_font(l, &lv_font_cn_16, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(0x9AA0B5), 0);
    lv_obj_align(l, LV_ALIGN_CENTER, 0, 0);
}

/* 主界面上的一个 App 按钮（真正的 lv_button，放在最顶层，固定且可点） */
static lv_obj_t *make_app_button(lv_obj_t *parent, const lv_image_dsc_t *app_icon, const char *name,
                                 lv_color_t color, int app_id)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, 88, 88);
    lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);     // 按钮本身透明，里面用色块当图标
    lv_obj_set_style_shadow_opa(btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_opa(btn, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(btn, 8, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x1A1A24), LV_STATE_PRESSED);  // 按下时给点反馈

    lv_obj_t *icon = lv_obj_create(btn);
    lv_obj_set_size(icon, 52, 52);
    lv_obj_set_style_bg_color(icon, color, 0);
    lv_obj_set_style_radius(icon, 14, 0);
    lv_obj_set_style_border_opa(icon, LV_OPA_TRANSP, 0);
    /* 图标本身不滚动、不显示滚动条（否则按下/拖动时会出现滑动条痕迹） */
    lv_obj_clear_flag(icon, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(icon, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(icon, LV_OBJ_FLAG_CLICKABLE);   // 图标本身不拦截，点击落到按钮上
    lv_obj_t *g = lv_image_create(icon);
    lv_image_set_src(g, app_icon);
    /* A8 只有 alpha 通道，必须指定着色颜色才会显示（recolor_opa 默认为 0，纯 A8 不会被绘制） */
    lv_obj_set_style_image_recolor(g, lv_color_white(), 0);
    lv_obj_set_style_image_recolor_opa(g, LV_OPA_COVER, 0);
    lv_obj_clear_flag(g, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(g, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(g, LV_OBJ_FLAG_CLICKABLE);   // 图标本身不拦截，点击落到按钮上
    lv_obj_center(g);

    lv_obj_t *nm = lv_label_create(btn);
    lv_label_set_text(nm, name);
    lv_obj_set_style_text_color(nm, lv_color_white(), 0);
    lv_obj_set_style_text_font(nm, &lv_font_cn_16, 0);
    lv_obj_clear_flag(nm, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(nm, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(nm, LV_OBJ_FLAG_CLICKABLE);     // 名称本身不拦截，点击落到按钮上
    /* 按钮自身也确保不滚动、不显示滚动条 */
    lv_obj_set_scrollbar_mode(btn, LV_SCROLLBAR_MODE_OFF);

    lv_obj_add_event_cb(btn, open_screen_cb, LV_EVENT_CLICKED, (void *)(intptr_t)app_id);
    return btn;
}

/* 预渲染后的主界面壁纸: 把"壁纸 × 50% + 底色 0x0B0B12"预先 CPU 混合,
 * 运行时 LVGL 只 blit 这个 buffer(opa=COVER, 无 alpha 混合), 滑动 FPS 40→60+。
 * 一次性 CPU 混合 153KB, 初始化 ~30ms, 可接受。 */
static lv_image_dsc_t s_home_bg_dsc = {
    .header = {
        .magic = LV_IMAGE_HEADER_MAGIC,
        .cf = LV_COLOR_FORMAT_RGB565,
        .flags = 0,
        .w = 320,
        .h = 240,
        .stride = 640,
        .reserved_2 = 0,
    },
    .data_size = 320 * 240 * 2,
    .data = NULL,
};

static void prerender_home_bg(void)
{
    /* 分配 PSRAM buffer, 320×240×2 = 153600 字节 */
    void *buf = heap_caps_malloc(s_home_bg_dsc.data_size, MALLOC_CAP_SPIRAM);
    if (!buf) {
        ESP_LOGE("UI", "home bg prerender alloc failed (need %u)", s_home_bg_dsc.data_size);
        return;
    }

    /* 底色 0x0B0B12 转 RGB565 (5-6-5 通道) */
    uint16_t dst_r = (0x0B >> 3) & 0x1F;
    uint16_t dst_g = (0x0B >> 2) & 0x3F;
    uint16_t dst_b = (0x12 >> 3) & 0x1F;

    /* 壁纸 RGB565 数据(小端, LV_COLOR_16_SWAP=0) */
    const uint8_t *src = bg_home_img.data;

    /* 50% alpha 混合: out = (src + dst) / 2, 用整数加法+移位避免除法 */
    for (uint32_t i = 0; i < 320 * 240; i++) {
        uint16_t px = src[i * 2] | (src[i * 2 + 1] << 8);
        uint16_t sr = (px >> 11) & 0x1F;
        uint16_t sg = (px >> 5) & 0x3F;
        uint16_t sb = px & 0x1F;

        uint16_t out_r = (sr + dst_r) >> 1;
        uint16_t out_g = (sg + dst_g) >> 1;
        uint16_t out_b = (sb + dst_b) >> 1;

        uint16_t out_px = (out_r << 11) | (out_g << 5) | out_b;
        ((uint8_t *)buf)[i * 2]     = (uint8_t)(out_px & 0xFF);
        ((uint8_t *)buf)[i * 2 + 1] = (uint8_t)((out_px >> 8) & 0xFF);
    }

    s_home_bg_dsc.data = (const uint8_t *)buf;
    ESP_LOGI("UI", "home bg prerendered: %u bytes at %p", s_home_bg_dsc.data_size, buf);
}

/* 构建主界面：状态栏 + 固定位置的 6 个 lv_button（直接挂在主界面上，
 * 主界面已设为不可滚动，所以按钮固定不动且能被正常命中点击） */
static void build_home(void)
{
    g_home = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(g_home, lv_color_hex(0x0B0B12), 0);
    lv_obj_set_style_bg_opa(g_home, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(g_home, 0, 0);
    lv_obj_set_style_border_opa(g_home, LV_OPA_TRANSP, 0);
    /* 预渲染壁纸: 把壁纸 + 底色 50% 混合预先算好, 运行时只 blit 不混合。
     * 关键: opa=COVER 让 LVGL 跳过 alpha 混合, 只 memcpy draw buf, 滑动快一倍。
     * 预渲染失败则回退到原 alpha 混合方案(慢但能显示)。 */
    prerender_home_bg();
    if (s_home_bg_dsc.data) {
        lv_obj_set_style_bg_image_src(g_home, &s_home_bg_dsc, 0);
        lv_obj_set_style_bg_image_opa(g_home, LV_OPA_COVER, 0);
    } else {
        lv_obj_set_style_bg_image_src(g_home, &bg_home_img, 0);
        lv_obj_set_style_bg_image_opa(g_home, LV_OPA_50, 0);
    }
    /* 7 个 App 排成 3 列 × 3 行，第 3 行超出 240px 屏幕，允许竖直滚动翻到 */
    lv_obj_add_flag(g_home, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(g_home, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(g_home, LV_SCROLLBAR_MODE_AUTO);
    /* 状态栏由 lvgl_ui_bootstrap 在 lv_layer_top() 上统一创建，不随 g_home 滚动 */

    struct { const lv_image_dsc_t *icon; const char *name; lv_color_t color; int id; } apps[9] = {
        { &ico_wifi,     "无线", lv_color_hex(0x2E7DE0), APP_WIFI },
        { &ico_bluetooth, "蓝牙", lv_color_hex(0x0082FC), APP_BLE },
        { &ico_settings, "设置", lv_color_hex(0x6B7280), APP_SETTINGS },
        { &ico_clock,    "时钟", lv_color_hex(0x9C5FFF), APP_CLOCK },
        { &ico_music,    "音乐", lv_color_hex(0xE0457B), APP_MUSIC },
        { &ico_game,     "游戏", lv_color_hex(0x2BB673), APP_GAME },
        { &ico_weather,  "天气", lv_color_hex(0xE08A2B), APP_WEATHER },
        { &ico_novel,    "小说", lv_color_hex(0xD4A24C), APP_NOVEL },
        { &ico_pcmon,    "监控", lv_color_hex(0x2EC4B6), APP_PCMON },
    };
    /* 横屏 320×240：3 列 × 3 行排 7 个 App，第 3 行超出屏幕，靠竖直滚动翻到。
     *   列 x = 20 / 120 / 220（按钮宽 88 → 最右边缘 308 < 320）
     *   行 y = 38 / 146 / 254（按钮高 88 → 第 3 行 254..342，需下滑） */
    const int start_x = 20, start_y = 38;
    const int gap_x = 100, gap_y = 108;   // 3 列 × 3 行
    for (int i = 0; i < (int)(sizeof(apps) / sizeof(apps[0])); i++) {
        lv_obj_t *btn = make_app_button(g_home, apps[i].icon, apps[i].name,
                                        apps[i].color, apps[i].id);
        int col = i % 3;
        int row = i / 3;
        lv_obj_set_pos(btn, start_x + col * gap_x, start_y + row * gap_y);
    }
}

/* 壁纸已重构为独立 obj(在 g_home_parent 里, 不随 g_home 滚动),
 * 滑动时只 invalidate 按钮覆盖的小区域, 壁纸大部分不重绘, 不需要压暗了。
 * 旧 ui_home_scroll_check 已删除。 */

/* 构建 Wi-Fi App 屏幕（手机风格 WiFi 界面） */
static void build_wifi_screen(lv_obj_t *scr)
{
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101018), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(scr, 0, 0);
    lv_obj_set_style_border_opa(scr, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);   // 外层不滚动，滚动交给内部列表 g_net_cont
    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);

    /* 顶部栏：返回 + Wi-Fi + 开关 */
    lv_obj_t *header = lv_obj_create(scr);
    lv_obj_set_size(header, LCD_W, 44);
    lv_obj_align(header, LV_ALIGN_TOP_LEFT, 0, 22);
    lv_obj_set_style_bg_color(header, lv_color_hex(0x16161F), 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(header, 0, 0);
    lv_obj_set_style_border_opa(header, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(header, 10, 0);
    lv_obj_set_style_pad_column(header, 8, 0);
    lv_obj_t *bk = lv_button_create(header);
    lv_obj_set_size(bk, 80, 34);
    lv_obj_t *bkl = lv_label_create(bk);
    lv_label_set_text(bkl, "< 返回");
    lv_obj_set_style_text_font(bkl, &lv_font_cn_16, 0);
    lv_obj_center(bkl);
    lv_obj_add_event_cb(bk, go_home_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *htitle = lv_label_create(header);
    lv_label_set_text(htitle, "无线");
    lv_obj_set_style_text_font(htitle, &lv_font_cn_16, 0);
    lv_obj_set_style_text_color(htitle, lv_color_white(), 0);
    lv_obj_t *spacer = lv_obj_create(header);
    lv_obj_set_size(spacer, 0, 0);
    lv_obj_set_style_bg_opa(spacer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_opa(spacer, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_grow(spacer, 1);
    lv_obj_t *sw = lv_switch_create(header);
    lv_obj_add_state(sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw, switch_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* 副标题（网络数量 / 状态） */
    g_subtitle = lv_label_create(scr);
    lv_label_set_text(g_subtitle, "扫描中...");
    lv_obj_set_style_text_font(g_subtitle, &lv_font_cn_16, 0);
    lv_obj_set_style_text_color(g_subtitle, lv_color_hex(0x9AA0B5), 0);
    lv_obj_align(g_subtitle, LV_ALIGN_TOP_LEFT, 14, 66);

    /* 网络列表容器（可滚动） */
    g_net_cont = lv_obj_create(scr);
    lv_obj_set_size(g_net_cont, LCD_W - 8, LCD_H - 92);
    lv_obj_align(g_net_cont, LV_ALIGN_BOTTOM_MID, 0, -6);
    lv_obj_set_style_bg_opa(g_net_cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_opa(g_net_cont, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(g_net_cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(g_net_cont, 6, 0);
    lv_obj_set_style_pad_top(g_net_cont, 2, 0);
    lv_obj_set_scroll_dir(g_net_cont, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(g_net_cont, LV_SCROLLBAR_MODE_OFF);
    /* 注意：不要把 clip_corner 设 true，否则 LVGL 会为该滚动容器分配整块内容大小的
     * 离屏 layer（约 1.5MB），分配器反复申请直到看门狗触发。滚动容器默认就会裁剪。 */

    /* 一次性创建固定行池（24 行），扫描结果出来后只更新内容，不在循环里反复建对象。
     * 每行仅 5 个对象（row/信号/名称/锁/箭头），避免对象过多导致建屏过慢卡死看门狗。 */
    for (int i = 0; i < WIFI_LIST_ROWS; i++) {
        wifi_row_t *r = &g_rows[i];
        lv_obj_t *row = lv_obj_create(g_net_cont);
        lv_obj_set_size(row, lv_pct(100), 50);
        lv_obj_set_style_bg_color(row, lv_color_hex(0x23232F), 0);
        lv_obj_set_style_radius(row, 10, 0);
        lv_obj_set_style_border_opa(row, LV_OPA_TRANSP, 0);
        /* 关键：行本身不可滚动。
         * 之前只关了滚动条（只是不画），LV_OBJ_FLAG_SCROLLABLE 还在，
         * 拖动时 LVGL 会优先滚动最内层的可滚动对象，也就是这一行，
         * 于是每一行都能被单独上下拖动；关掉后拖动才会冒泡到外层列表统一滚动。 */
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scrollbar_mode(row, LV_SCROLLBAR_MODE_OFF);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_hor(row, 12, 0);
        lv_obj_set_style_pad_column(row, 10, 0);

        /* 信号强度：单个圆角小块，高度表示档位（绿=强/灰=弱），刷新时改色 */
        lv_obj_t *sig = lv_obj_create(row);
        lv_obj_set_size(sig, 10, 18);
        lv_obj_set_style_bg_color(sig, lv_color_hex(0x35D04A), 0);
        lv_obj_set_style_bg_opa(sig, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(sig, 2, 0);
        lv_obj_set_style_border_opa(sig, LV_OPA_TRANSP, 0);
        lv_obj_clear_flag(sig, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scrollbar_mode(sig, LV_SCROLLBAR_MODE_OFF);
        lv_obj_clear_flag(sig, LV_OBJ_FLAG_CLICKABLE);
        r->sig = sig;

        /* SSID 名称 */
        r->name = lv_label_create(row);
        lv_label_set_text(r->name, "");
        lv_obj_set_style_text_font(r->name, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(r->name, lv_color_white(), 0);
        lv_obj_set_flex_grow(r->name, 1);
        lv_label_set_long_mode(r->name, LV_LABEL_LONG_DOT);
        lv_obj_clear_flag(r->name, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scrollbar_mode(r->name, LV_SCROLLBAR_MODE_OFF);

        /* 加密指示：单个圆角小块（加密网络才显示） */
        lv_obj_t *lock = lv_obj_create(row);
        lv_obj_set_size(lock, 14, 16);
        lv_obj_set_style_bg_opa(lock, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_color(lock, lv_color_hex(0xCFCFCF), 0);
        lv_obj_set_style_border_width(lock, 2, 0);
        lv_obj_set_style_radius(lock, 3, 0);
        lv_obj_clear_flag(lock, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scrollbar_mode(lock, LV_SCROLLBAR_MODE_OFF);
        lv_obj_clear_flag(lock, LV_OBJ_FLAG_CLICKABLE);
        r->lock = lock;

        /* 右箭头 */
        lv_obj_t *chev = lv_label_create(row);
        lv_label_set_text(chev, LV_SYMBOL_RIGHT);
        lv_obj_set_style_text_color(chev, lv_color_hex(0x9AA0B5), 0);
        lv_obj_clear_flag(chev, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scrollbar_mode(chev, LV_SCROLLBAR_MODE_OFF);

        /* 整行可点，user_data 为行号 */
        r->row = row;
        r->ap_idx = -1;
        lv_obj_add_event_cb(row, ap_item_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);   // 初始隐藏，扫描后显示
    }

    /* 详情页（全屏覆盖层，默认隐藏） */
    g_detail = lv_obj_create(scr);
    lv_obj_set_size(g_detail, LCD_W, LCD_H);
    lv_obj_align(g_detail, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(g_detail, lv_color_hex(0x101018), 0);
    lv_obj_set_style_bg_opa(g_detail, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(g_detail, 0, 0);
    lv_obj_set_style_border_opa(g_detail, LV_OPA_TRANSP, 0);
    lv_obj_set_scrollbar_mode(g_detail, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(g_detail, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(g_detail, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *dh = lv_obj_create(g_detail);
    lv_obj_set_size(dh, LCD_W, 44);
    lv_obj_align(dh, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(dh, lv_color_hex(0x16161F), 0);
    lv_obj_set_style_radius(dh, 0, 0);
    lv_obj_set_style_border_opa(dh, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(dh, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(dh, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(dh, 10, 0);
    lv_obj_t *back_btn = lv_button_create(dh);
    lv_obj_set_size(back_btn, 80, 32);
    lv_obj_t *back_lbl = lv_label_create(back_btn);
    lv_label_set_text(back_lbl, "< 返回");
    lv_obj_set_style_text_font(back_lbl, &lv_font_cn_16, 0);
    lv_obj_center(back_lbl);
    lv_obj_add_event_cb(back_btn, back_btn_cb, LV_EVENT_CLICKED, g_detail);
    lv_obj_t *dht = lv_label_create(dh);
    lv_label_set_text(dht, "无线");
    lv_obj_set_style_text_font(dht, &lv_font_cn_16, 0);
    lv_obj_set_style_text_color(dht, lv_color_white(), 0);
    g_detail_label = lv_label_create(g_detail);
    lv_label_set_long_mode(g_detail_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(g_detail_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(g_detail_label, lv_color_white(), 0);
    lv_obj_set_width(g_detail_label, LCD_W - 24);
    lv_obj_align(g_detail_label, LV_ALIGN_TOP_LEFT, 12, 50);
    lv_label_set_text(g_detail_label, "");

    /* 连接状态文字 */
    g_detail_status = lv_label_create(g_detail);
    lv_label_set_long_mode(g_detail_status, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(g_detail_status, &lv_font_cn_16, 0);
    lv_obj_set_style_text_color(g_detail_status, lv_color_hex(0x9AA0B5), 0);
    lv_obj_set_width(g_detail_status, LCD_W - 32);
    lv_obj_align(g_detail_status, LV_ALIGN_TOP_LEFT, 16, 172);
    lv_label_set_text(g_detail_status, "");

    /* 连接 / 断开按钮（标签按需切换） */
    g_detail_btn = lv_button_create(g_detail);
    lv_obj_set_size(g_detail_btn, 140, 34);
    lv_obj_align(g_detail_btn, LV_ALIGN_TOP_LEFT, 16, 198);
    lv_obj_set_style_bg_color(g_detail_btn, lv_color_hex(0x2E7DE0), 0);
    lv_obj_t *bl = lv_label_create(g_detail_btn);
    lv_label_set_text(bl, "连接");
    lv_obj_set_style_text_font(bl, &lv_font_cn_16, 0);
    lv_obj_set_style_text_color(bl, lv_color_white(), 0);
    lv_obj_center(bl);
    lv_obj_add_event_cb(g_detail_btn, connect_btn_cb, LV_EVENT_CLICKED, NULL);
}

/* ---------------- BLE (蓝牙) 界面 ---------------- */

static bool g_ble_on = true;
static lv_obj_t *g_ble_subtitle = NULL;
static lv_obj_t *g_ble_list_cont = NULL;

/* BLE 开关：开 -> 触发扫描；关 -> 清空列表 */
static void ble_switch_cb(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    g_ble_on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    if (g_ble_on) {
        ble_scan_trigger();
        if (g_ble_subtitle) lv_label_set_text(g_ble_subtitle, "扫描中...");
    } else {
        if (g_ble_subtitle) lv_label_set_text(g_ble_subtitle, "蓝牙已关闭");
        for (int i = 0; i < BLE_LIST_ROWS; i++) {
            lv_obj_add_flag(g_ble_rows[i].row, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

/* 点击某个 BLE 设备：打开详情页 */
static void ble_item_cb(lv_event_t *e)
{
    int row_i = (int)(intptr_t)lv_event_get_user_data(e);
    if (row_i < 0 || row_i >= BLE_LIST_ROWS) return;
    int idx = g_ble_rows[row_i].dev_idx;
    if (idx < 0 || idx >= g_ble_dev_count) return;

    g_ble_detail_idx = idx;
    ble_device_t *d = &g_ble_dev_list[idx];

    char mac[18];
    snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X",
             d->addr[0], d->addr[1], d->addr[2],
             d->addr[3], d->addr[4], d->addr[5]);

    const char *nm = d->name[0] ? d->name : "<unknown>";
    char info[128];
    snprintf(info, sizeof(info), "%s\nMAC: %s\nRSSI: %d dBm", nm, mac, d->rssi);
    if (g_ble_detail_label) lv_label_set_text(g_ble_detail_label, info);

    if (g_ble_detail) lv_obj_clear_flag(g_ble_detail, LV_OBJ_FLAG_HIDDEN);
}

/* BLE 详情页返回按钮 */
static void ble_back_btn_cb(lv_event_t *e)
{
    lv_obj_t *detail = (lv_obj_t *)lv_event_get_user_data(e);
    if (detail) lv_obj_add_flag(detail, LV_OBJ_FLAG_HIDDEN);
    g_ble_detail_idx = -1;
}

/* BLE 详情页连接/断开按钮 */
static void ble_connect_btn_cb(lv_event_t *e)
{
    (void)e;
    if (g_ble_detail_idx < 0 || g_ble_detail_idx >= g_ble_dev_count) return;

    ble_device_t *d = &g_ble_dev_list[g_ble_detail_idx];
    bool is_current = (g_ble_conn_name[0] != '\0') &&
                      (strncmp(g_ble_conn_name, d->name[0] ? d->name : "", 31) == 0);

    if (is_current && (g_ble_state == BLE_STATE_CONNECTED || g_ble_state == BLE_STATE_CONNECTING)) {
        ble_app_disconnect();
        return;
    }

    if (d->is_connectable) {
        ble_app_connect(d->addr, d->addr_type);
    }
}

/* 构建蓝牙 App 屏幕 */
static void build_ble_screen(lv_obj_t *scr)
{
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101018), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(scr, 0, 0);
    lv_obj_set_style_border_opa(scr, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);

    /* 顶部栏：返回 + 蓝牙 + 开关 */
    lv_obj_t *header = lv_obj_create(scr);
    lv_obj_set_size(header, LCD_W, 44);
    lv_obj_align(header, LV_ALIGN_TOP_LEFT, 0, 22);
    lv_obj_set_style_bg_color(header, lv_color_hex(0x16161F), 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(header, 0, 0);
    lv_obj_set_style_border_opa(header, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(header, 10, 0);
    lv_obj_set_style_pad_column(header, 8, 0);
    lv_obj_t *bk = lv_button_create(header);
    lv_obj_set_size(bk, 80, 34);
    lv_obj_t *bkl = lv_label_create(bk);
    lv_label_set_text(bkl, "< 返回");
    lv_obj_set_style_text_font(bkl, &lv_font_cn_16, 0);
    lv_obj_center(bkl);
    lv_obj_add_event_cb(bk, go_home_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *htitle = lv_label_create(header);
    lv_label_set_text(htitle, "蓝牙");
    lv_obj_set_style_text_font(htitle, &lv_font_cn_16, 0);
    lv_obj_set_style_text_color(htitle, lv_color_white(), 0);
    lv_obj_t *spacer = lv_obj_create(header);
    lv_obj_set_size(spacer, 0, 0);
    lv_obj_set_style_bg_opa(spacer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_opa(spacer, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_grow(spacer, 1);
    lv_obj_t *sw = lv_switch_create(header);
    lv_obj_add_state(sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw, ble_switch_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* 副标题 */
    g_ble_subtitle = lv_label_create(scr);
    lv_label_set_text(g_ble_subtitle, "扫描中...");
    lv_obj_set_style_text_font(g_ble_subtitle, &lv_font_cn_16, 0);
    lv_obj_set_style_text_color(g_ble_subtitle, lv_color_hex(0x9AA0B5), 0);
    lv_obj_align(g_ble_subtitle, LV_ALIGN_TOP_LEFT, 14, 70);

    /* 设备列表容器 */
    g_ble_list_cont = lv_obj_create(scr);
    lv_obj_set_size(g_ble_list_cont, LCD_W - 8, LCD_H - 92);
    lv_obj_align(g_ble_list_cont, LV_ALIGN_BOTTOM_MID, 0, -6);
    lv_obj_set_style_bg_opa(g_ble_list_cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_opa(g_ble_list_cont, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(g_ble_list_cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(g_ble_list_cont, 6, 0);
    lv_obj_set_style_pad_top(g_ble_list_cont, 2, 0);
    lv_obj_set_scroll_dir(g_ble_list_cont, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(g_ble_list_cont, LV_SCROLLBAR_MODE_OFF);

    /* 固定行池 */
    for (int i = 0; i < BLE_LIST_ROWS; i++) {
        ble_row_t *r = &g_ble_rows[i];
        lv_obj_t *row = lv_obj_create(g_ble_list_cont);
        lv_obj_set_size(row, lv_pct(100), 50);
        lv_obj_set_style_bg_color(row, lv_color_hex(0x23232F), 0);
        lv_obj_set_style_radius(row, 10, 0);
        lv_obj_set_style_border_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scrollbar_mode(row, LV_SCROLLBAR_MODE_OFF);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_hor(row, 12, 0);
        lv_obj_set_style_pad_column(row, 10, 0);

        /* 信号强度 */
        lv_obj_t *sig = lv_obj_create(row);
        lv_obj_set_size(sig, 10, 18);
        lv_obj_set_style_bg_color(sig, lv_color_hex(0x35D04A), 0);
        lv_obj_set_style_bg_opa(sig, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(sig, 2, 0);
        lv_obj_set_style_border_opa(sig, LV_OPA_TRANSP, 0);
        lv_obj_clear_flag(sig, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scrollbar_mode(sig, LV_SCROLLBAR_MODE_OFF);
        lv_obj_clear_flag(sig, LV_OBJ_FLAG_CLICKABLE);
        r->sig = sig;

        /* 设备名称 */
        r->name = lv_label_create(row);
        lv_label_set_text(r->name, "");
        lv_obj_set_style_text_font(r->name, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(r->name, lv_color_white(), 0);
        lv_obj_set_flex_grow(r->name, 1);
        lv_label_set_long_mode(r->name, LV_LABEL_LONG_DOT);
        lv_obj_clear_flag(r->name, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scrollbar_mode(r->name, LV_SCROLLBAR_MODE_OFF);

        /* 右箭头 */
        lv_obj_t *chev = lv_label_create(row);
        lv_label_set_text(chev, LV_SYMBOL_RIGHT);
        lv_obj_set_style_text_color(chev, lv_color_hex(0x9AA0B5), 0);
        lv_obj_clear_flag(chev, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scrollbar_mode(chev, LV_SCROLLBAR_MODE_OFF);

        r->row = row;
        r->dev_idx = -1;
        lv_obj_add_event_cb(row, ble_item_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
    }

    /* 详情页（全屏覆盖层，默认隐藏） */
    g_ble_detail = lv_obj_create(scr);
    lv_obj_set_size(g_ble_detail, LCD_W, LCD_H);
    lv_obj_align(g_ble_detail, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(g_ble_detail, lv_color_hex(0x101018), 0);
    lv_obj_set_style_bg_opa(g_ble_detail, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(g_ble_detail, 0, 0);
    lv_obj_set_style_border_opa(g_ble_detail, LV_OPA_TRANSP, 0);
    lv_obj_set_scrollbar_mode(g_ble_detail, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(g_ble_detail, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(g_ble_detail, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *dh = lv_obj_create(g_ble_detail);
    lv_obj_set_size(dh, LCD_W, 44);
    lv_obj_align(dh, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(dh, lv_color_hex(0x16161F), 0);
    lv_obj_set_style_radius(dh, 0, 0);
    lv_obj_set_style_border_opa(dh, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(dh, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(dh, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(dh, 10, 0);
    lv_obj_t *back_btn = lv_button_create(dh);
    lv_obj_set_size(back_btn, 80, 32);
    lv_obj_t *back_lbl = lv_label_create(back_btn);
    lv_label_set_text(back_lbl, "< 返回");
    lv_obj_set_style_text_font(back_lbl, &lv_font_cn_16, 0);
    lv_obj_center(back_lbl);
    lv_obj_add_event_cb(back_btn, ble_back_btn_cb, LV_EVENT_CLICKED, g_ble_detail);
    lv_obj_t *dht = lv_label_create(dh);
    lv_label_set_text(dht, "蓝牙");
    lv_obj_set_style_text_font(dht, &lv_font_cn_16, 0);
    lv_obj_set_style_text_color(dht, lv_color_white(), 0);

    g_ble_detail_label = lv_label_create(g_ble_detail);
    lv_label_set_long_mode(g_ble_detail_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(g_ble_detail_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(g_ble_detail_label, lv_color_white(), 0);
    lv_obj_set_width(g_ble_detail_label, LCD_W - 24);
    lv_obj_align(g_ble_detail_label, LV_ALIGN_TOP_LEFT, 12, 50);
    lv_label_set_text(g_ble_detail_label, "");

    g_ble_detail_status = lv_label_create(g_ble_detail);
    lv_label_set_long_mode(g_ble_detail_status, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(g_ble_detail_status, &lv_font_cn_16, 0);
    lv_obj_set_style_text_color(g_ble_detail_status, lv_color_hex(0x9AA0B5), 0);
    lv_obj_set_width(g_ble_detail_status, LCD_W - 32);
    lv_obj_align(g_ble_detail_status, LV_ALIGN_TOP_LEFT, 16, 172);
    lv_label_set_text(g_ble_detail_status, "");

    g_ble_detail_btn = lv_button_create(g_ble_detail);
    lv_obj_set_size(g_ble_detail_btn, 140, 34);
    lv_obj_align(g_ble_detail_btn, LV_ALIGN_TOP_LEFT, 16, 198);
    lv_obj_set_style_bg_color(g_ble_detail_btn, lv_color_hex(0x0082FC), 0);
    lv_obj_t *bl = lv_label_create(g_ble_detail_btn);
    lv_label_set_text(bl, "连接");
    lv_obj_set_style_text_font(bl, &lv_font_cn_16, 0);
    lv_obj_set_style_text_color(bl, lv_color_white(), 0);
    lv_obj_center(bl);
    lv_obj_add_event_cb(g_ble_detail_btn, ble_connect_btn_cb, LV_EVENT_CLICKED, NULL);
}

/* ---------------- 密码输入面板（首次使用时才创建） ---------------- */
static void pwd_ok_cb(lv_event_t *e)
{
    (void)e;
    const char *pw = lv_textarea_get_text(g_pwd_ta);
    lv_obj_add_flag(g_pwd_panel, LV_OBJ_FLAG_HIDDEN);
    lv_keyboard_set_textarea(g_pwd_kb, NULL);
    wifi_app_connect(g_pending_ssid, pw);
}

static void pwd_cancel_cb(lv_event_t *e)
{
    (void)e;
    lv_obj_add_flag(g_pwd_panel, LV_OBJ_FLAG_HIDDEN);
    lv_keyboard_set_textarea(g_pwd_kb, NULL);
}

/* 创建密码面板：SSID 提示 + 密码框 + OK/Cancel + 屏幕键盘。
 * 最后创建，所以会盖在详情页之上。 */
static void build_pwd_panel(lv_obj_t *parent)
{
    g_pwd_panel = lv_obj_create(parent);
    lv_obj_set_size(g_pwd_panel, LCD_W, LCD_H);
    lv_obj_align(g_pwd_panel, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(g_pwd_panel, lv_color_hex(0x101018), 0);
    lv_obj_set_style_bg_opa(g_pwd_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(g_pwd_panel, 0, 0);
    lv_obj_set_style_border_opa(g_pwd_panel, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(g_pwd_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(g_pwd_panel, LV_SCROLLBAR_MODE_OFF);

    g_pwd_ssid = lv_label_create(g_pwd_panel);
    lv_label_set_text(g_pwd_ssid, "");
    lv_obj_set_style_text_font(g_pwd_ssid, &lv_font_cn_16, 0);
    lv_obj_set_style_text_color(g_pwd_ssid, lv_color_hex(0x9AA0B5), 0);
    lv_obj_set_width(g_pwd_ssid, LCD_W - 32);
    lv_label_set_long_mode(g_pwd_ssid, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(g_pwd_ssid, 16, 6);

    g_pwd_ta = lv_textarea_create(g_pwd_panel);
    lv_obj_set_size(g_pwd_ta, 288, 34);
    lv_obj_set_pos(g_pwd_ta, 16, 26);
    lv_textarea_set_one_line(g_pwd_ta, true);
    lv_textarea_set_password_mode(g_pwd_ta, true);
    lv_textarea_set_placeholder_text(g_pwd_ta, "密码");
    lv_obj_set_style_text_font(g_pwd_ta, &lv_font_montserrat_20, 0);

    lv_obj_t *ok = lv_button_create(g_pwd_panel);
    lv_obj_set_size(ok, 92, 32);
    lv_obj_set_pos(ok, 16, 66);
    lv_obj_set_style_bg_color(ok, lv_color_hex(0x2E7DE0), 0);
    lv_obj_t *okl = lv_label_create(ok);
    lv_label_set_text(okl, "加入");
    lv_obj_set_style_text_font(okl, &lv_font_cn_16, 0);
    lv_obj_center(okl);
    lv_obj_add_event_cb(ok, pwd_ok_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *ca = lv_button_create(g_pwd_panel);
    lv_obj_set_size(ca, 92, 32);
    lv_obj_set_pos(ca, 120, 66);
    lv_obj_t *cal = lv_label_create(ca);
    lv_label_set_text(cal, "取消");
    lv_obj_set_style_text_font(cal, &lv_font_cn_16, 0);
    lv_obj_center(cal);
    lv_obj_add_event_cb(ca, pwd_cancel_cb, LV_EVENT_CLICKED, NULL);

    g_pwd_kb = lv_keyboard_create(g_pwd_panel);
    lv_obj_set_size(g_pwd_kb, LCD_W, 140);
    lv_obj_align(g_pwd_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(g_pwd_kb, g_pwd_ta);

    /* 键盘自带的“OK”键触发 textarea 的 LV_EVENT_READY、“关闭”键触发
     * LV_EVENT_CANCEL，并不会走 Join/Cancel 按钮的 LV_EVENT_CLICKED，
     * 这里补上，让点键盘的确定键也能提交密码/取消。 */
    lv_obj_add_event_cb(g_pwd_ta, pwd_ok_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(g_pwd_ta, pwd_cancel_cb, LV_EVENT_CANCEL, NULL);

    lv_obj_add_flag(g_pwd_panel, LV_OBJ_FLAG_HIDDEN);
}

/* ---------------- Clock 屏（连上网自动校时后显示本地时间） ---------------- */

/* 刷新时钟屏上的闹钟概要行：显示 3 个闹钟的开关状态 */
static void ui_alarm_update_label(void)
{
    if (!g_alarm_label) return;
    char buf[48];
    char *p = buf;
    int n = sizeof(buf);
    int used = 0;
    used += snprintf(p + used, n - used, "闹钟:");
    for (int i = 0; i < ALARM_MAX; i++) {
        const alarm_conf_t *a = alarm_get(i);
        used += snprintf(p + used, n - used, " %02d:%02d%s%s",
                         a->hour, a->minute,
                         a->enabled ? "*" : "",
                         i == ALARM_MAX - 1 ? "" : " /");
    }
    lv_label_set_text(g_alarm_label, buf);
}

/* 闹钟屏按钮编号（user_data 编码：idx*10 + op） */
enum {
    ALARM_OP_HOUR_DEC = 0,
    ALARM_OP_HOUR_INC,
    ALARM_OP_MIN_DEC,
    ALARM_OP_MIN_INC,
    ALARM_OP_TOGGLE,
};

static void alarm_btn_cb(lv_event_t *e)
{
    int code = (int)(intptr_t)lv_event_get_user_data(e);
    int idx = code / 10;
    int op  = code % 10;
    if (idx < 0 || idx >= ALARM_MAX) return;
    const alarm_conf_t *a = alarm_get(idx);
    int  hh = a->hour, mm = a->minute;
    bool en = a->enabled;

    switch (op) {
        case ALARM_OP_HOUR_DEC: hh = (hh + 23) % 24; break;
        case ALARM_OP_HOUR_INC: hh = (hh + 1) % 24; break;
        case ALARM_OP_MIN_DEC:  mm = (mm + 59) % 60; break;
        case ALARM_OP_MIN_INC:  mm = (mm + 1) % 60; break;
        case ALARM_OP_TOGGLE:   en = !en; break;
        default: break;
    }
    alarm_set(idx, hh, mm, en);     /* 内部写 NVS，重启后保留 */
    ui_alarm_update_label();

    /* 同步刷新闹钟屏里对应行的显示 */
    if (g_alarm_screen) {
        lv_obj_t *row = lv_obj_get_child(g_alarm_screen, 2 + idx);   /* 跳过状态栏+标题栏 */
        if (row) {
            lv_obj_t *tm = lv_obj_get_child(row, 1);     /* 时间 label */
            lv_obj_t *sw = lv_obj_get_child(row, 6);     /* 开关按钮 */
            if (tm) {
                char tbuf[8];
                snprintf(tbuf, sizeof(tbuf), "%02d:%02d", hh, mm);
                lv_label_set_text(tm, tbuf);
            }
            if (sw) {
                lv_obj_t *sl = lv_obj_get_child(sw, 0);
                if (sl) lv_label_set_text(sl, en ? "开" : "关");
                lv_obj_set_style_bg_color(sw,
                    en ? lv_color_hex(0x2BB673) : lv_color_hex(0x2A2A38), 0);
            }
        }
    }
}

/* 点击时钟屏闹钟概要行：跳转到闹钟设置屏 */
static void alarm_label_click_cb(lv_event_t *e)
{
    (void)e;
    g_pending_app = APP_ALARM;
}

static void build_clock_screen(lv_obj_t *parent)
{
    g_clock_time = lv_label_create(parent);
    lv_obj_set_style_text_font(g_clock_time, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(g_clock_time, lv_color_white(), 0);
    lv_obj_align(g_clock_time, LV_ALIGN_CENTER, 0, -28);
    lv_label_set_text(g_clock_time, "--:--:--");

    g_clock_date = lv_label_create(parent);
    lv_obj_set_style_text_font(g_clock_date, &lv_font_cn_16, 0);
    lv_obj_set_style_text_color(g_clock_date, lv_color_hex(0xAAAAAA), 0);
    lv_obj_align(g_clock_date, LV_ALIGN_CENTER, 0, 24);
    lv_label_set_text(g_clock_date, "正在同步时间...");

    /* ---- 闹钟概要行：点击进入闹钟设置屏 ----
     * g_alarm_label 是按钮里的子 label（用于 set_text 显示文字）；
     * 按钮自己接收点击跳转。 */
    lv_obj_t *abtn = lv_button_create(parent);
    lv_obj_set_size(abtn, LCD_W - 32, 28);
    lv_obj_set_pos(abtn, 16, 168);
    lv_obj_set_style_bg_color(abtn, lv_color_hex(0x1A1A24), 0);
    lv_obj_set_style_radius(abtn, 8, 0);
    lv_obj_set_style_pad_left(abtn, 8, 0);
    lv_obj_set_style_border_opa(abtn, LV_OPA_TRANSP, 0);
    lv_obj_t *alab = lv_label_create(abtn);
    lv_obj_set_style_text_font(alab, &lv_font_cn_16, 0);
    lv_obj_set_style_text_color(alab, lv_color_hex(0xE0E0E0), 0);
    lv_obj_align(alab, LV_ALIGN_LEFT_MID, 0, 0);
    g_alarm_label = alab;
    lv_obj_add_event_cb(abtn, alarm_label_click_cb, LV_EVENT_CLICKED, NULL);
    ui_alarm_update_label();
}

/* 闹钟设置屏：3 行，每行 闹钟N + 时间 + 时-/时+/分-/分+ + 开关 */
static void build_alarm_screen(lv_obj_t *scr)
{
    /* 行容器：3 行闹钟设置 */
    static const int row_h = 50;
    int y0 = 70;     /* 状态栏 22 + 标题栏 40 + 间距 8 = 70 */
    for (int i = 0; i < ALARM_MAX; i++) {
        const alarm_conf_t *a = alarm_get(i);
        lv_obj_t *row = lv_obj_create(scr);
        lv_obj_set_size(row, LCD_W - 16, row_h);
        lv_obj_align(row, LV_ALIGN_TOP_MID, 0, y0 + i * (row_h + 6));
        lv_obj_set_style_bg_color(row, lv_color_hex(0x16161F), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(row, 8, 0);
        lv_obj_set_style_border_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_pad_all(row, 4, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row, 4, 0);

        /* 闹钟号 */
        lv_obj_t *nlbl = lv_label_create(row);
        char nbuf[8];
        snprintf(nbuf, sizeof(nbuf), "闹%d", i + 1);
        lv_label_set_text(nlbl, nbuf);
        lv_obj_set_style_text_font(nlbl, &lv_font_cn_16, 0);
        lv_obj_set_style_text_color(nlbl, lv_color_hex(0xE0E0E0), 0);
        lv_obj_set_width(nlbl, 36);

        /* 时间显示 */
        lv_obj_t *tlbl = lv_label_create(row);
        char tbuf[8];
        snprintf(tbuf, sizeof(tbuf), "%02d:%02d", a->hour, a->minute);
        lv_label_set_text(tlbl, tbuf);
        lv_obj_set_style_text_font(tlbl, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(tlbl, lv_color_white(), 0);
        lv_obj_set_width(tlbl, 70);

        /* 4 个调节按钮 */
        static const char *op_txt[4] = {"时-", "时+", "分-", "分+"};
        for (int k = 0; k < 4; k++) {
            lv_obj_t *b = lv_button_create(row);
            lv_obj_set_size(b, 34, 30);
            lv_obj_set_style_bg_color(b, lv_color_hex(0x2A2A38), 0);
            lv_obj_set_style_radius(b, 6, 0);
            lv_obj_t *bl = lv_label_create(b);
            lv_label_set_text(bl, op_txt[k]);
            lv_obj_set_style_text_font(bl, &lv_font_cn_16, 0);
            lv_obj_set_style_text_color(bl, lv_color_white(), 0);
            lv_obj_center(bl);
            int code = i * 10 + k;
            lv_obj_add_event_cb(b, alarm_btn_cb, LV_EVENT_CLICKED,
                                (void *)(intptr_t)code);
        }

        /* 开关按钮 */
        lv_obj_t *sw = lv_button_create(row);
        lv_obj_set_size(sw, 38, 30);
        lv_obj_set_style_bg_color(sw,
            a->enabled ? lv_color_hex(0x2BB673) : lv_color_hex(0x2A2A38), 0);
        lv_obj_set_style_radius(sw, 6, 0);
        lv_obj_t *sl = lv_label_create(sw);
        lv_label_set_text(sl, a->enabled ? "开" : "关");
        lv_obj_set_style_text_font(sl, &lv_font_cn_16, 0);
        lv_obj_set_style_text_color(sl, lv_color_white(), 0);
        lv_obj_center(sl);
        int code = i * 10 + ALARM_OP_TOGGLE;
        lv_obj_add_event_cb(sw, alarm_btn_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)code);
    }
}

/* ---------------- Weather 屏（英文显示，连上网自动拉取） ---------------- */

/* 天气刷新间隔：数据超过这个时长即视为过期，过期才重新请求 */
#define WX_REFRESH_MS (10 * 60 * 1000)

/* 只在「需要」时才请求天气，避免不必要的联网：
 *   - 不在天气界面时，周期刷新整个跳过（见 ui_weather_periodic）；
 *   - 进入界面时若数据仍然新鲜就不重复请求，来回切界面不会反复拉；
 *   - force=true 用于手动点「刷新」，无视新鲜度强制拉一次。
 * 返回是否真的发出了请求（weather_request 只是唤醒后台任务，不阻塞）。 */
static bool ui_weather_refresh_if_needed(bool force)
{
    if (g_wifi_state != WIFI_STA_CONNECTED) return false;

    /* 上一次请求还在进行中，不再重复发，避免请求排队堆积 */
    if (g_weather.fetching) return false;

    uint32_t now = lv_tick_get();
    bool stale = (now - s_last_wx_req_ms) >= WX_REFRESH_MS;

    /* 非强制且数据仍新鲜，就不打扰网络（用于停留在界面时的周期刷新） */
    if (!force && !stale && g_weather.valid) return false;

    weather_request(NULL);
    s_last_wx_req_ms = now;
    return true;
}

static void wx_refresh_cb(lv_event_t *e)
{
    (void)e;
    ui_weather_refresh_if_needed(true);   /* 手动刷新：强制拉一次 */
}

static void build_weather_screen(lv_obj_t *parent)
{
    /* 内容容器：放在标题栏(22~62)之下、底部按钮之上，用纵向 flex 自动堆叠 5 个标签。
     * 用 flex 而非固定坐标，可按各标签真实行高排布并保留固定间距，避免重叠。 */
    lv_obj_t *cont = lv_obj_create(parent);
    lv_obj_set_size(cont, LCD_W - 24, LV_SIZE_CONTENT);  /* 高度按内容自适应，避免末行溢出压到按钮 */
    lv_obj_align(cont, LV_ALIGN_TOP_MID, 0, 62);
    lv_obj_set_style_bg_opa(cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_opa(cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_shadow_opa(cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(cont, 0, 0);
    lv_obj_set_style_pad_all(cont, 0, 0);
    lv_obj_clear_flag(cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(cont, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(cont, 4, 0);   /* 标签之间的竖直间距 */

    g_wx_city = lv_label_create(cont);
    lv_obj_set_style_text_font(g_wx_city, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(g_wx_city, lv_color_white(), 0);
    lv_label_set_text(g_wx_city, "--");

    g_wx_temp = lv_label_create(cont);
    lv_obj_set_style_text_font(g_wx_temp, &lv_font_cn_16, 0);
    lv_obj_set_style_text_color(g_wx_temp, lv_color_white(), 0);
    lv_label_set_text(g_wx_temp, "--.- C");

    g_wx_desc = lv_label_create(cont);
    lv_obj_set_style_text_font(g_wx_desc, &lv_font_cn_16, 0);
    lv_obj_set_style_text_color(g_wx_desc, lv_color_hex(0xCCCCCC), 0);
    lv_label_set_text(g_wx_desc, "...");

    g_wx_extra = lv_label_create(cont);
    lv_obj_set_style_text_font(g_wx_extra, &lv_font_cn_16, 0);
    lv_obj_set_style_text_color(g_wx_extra, lv_color_hex(0xAAAAAA), 0);
    lv_label_set_text(g_wx_extra, "湿度 --%   风速 -- km/h");

    g_wx_updated = lv_label_create(cont);
    lv_obj_set_style_text_font(g_wx_updated, &lv_font_cn_16, 0);
    lv_obj_set_style_text_color(g_wx_updated, lv_color_hex(0x888888), 0);
    lv_label_set_text(g_wx_updated, "尚未获取");

    /* 底部刷新按钮（独立于内容容器，固定在屏幕底部） */
    lv_obj_t *rf = lv_button_create(parent);
    lv_obj_set_size(rf, 140, 34);
    lv_obj_align(rf, LV_ALIGN_BOTTOM_MID, 0, -14);
    lv_obj_set_style_bg_color(rf, lv_color_hex(0x2E7DE0), 0);
    lv_obj_t *rl = lv_label_create(rf);
    lv_label_set_text(rl, "刷新");
    lv_obj_set_style_text_font(rl, &lv_font_cn_16, 0);
    lv_obj_center(rl);
    lv_obj_add_event_cb(rf, wx_refresh_cb, LV_EVENT_CLICKED, NULL);
}

/* ---------------- 电脑监控屏（TCP 从 PC 端 AIDA64 读取，每秒刷新） ---------------- */

static void build_pcmon_screen(lv_obj_t *parent)
{
    /* 可滚动的传感器列表容器（标题栏 22~62 之下，底部状态条之上） */
    lv_obj_t *cont = lv_obj_create(parent);
    lv_obj_set_size(cont, LCD_W - 10, LCD_H - 86);
    lv_obj_align(cont, LV_ALIGN_TOP_MID, 0, 62);
    lv_obj_set_style_bg_opa(cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_opa(cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_shadow_opa(cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(cont, 0, 0);
    lv_obj_set_style_pad_all(cont, 4, 0);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(cont, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(cont, 2, 0);
    lv_obj_add_flag(cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(cont, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(cont, LV_SCROLLBAR_MODE_AUTO);

    for (int i = 0; i < PC_MON_MAX_SENSORS; i++) {
        lv_obj_t *l = lv_label_create(cont);
        lv_obj_set_width(l, LCD_W - 22);
        lv_obj_set_style_text_font(l, &lv_font_cn_16, 0);
        lv_obj_set_style_text_color(l, lv_color_white(), 0);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_LEFT, 0);
        lv_label_set_text(l, "");
        g_pcmon_labels[i] = l;
    }

    /* 底部状态条（固定，不随列表滚动） */
    g_pcmon_status = lv_label_create(parent);
    lv_obj_set_width(g_pcmon_status, LCD_W - 12);
    lv_obj_align(g_pcmon_status, LV_ALIGN_BOTTOM_LEFT, 6, -4);
    lv_obj_set_style_text_font(g_pcmon_status, &lv_font_cn_16, 0);
    lv_obj_set_style_text_color(g_pcmon_status, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_align(g_pcmon_status, LV_TEXT_ALIGN_LEFT, 0);
    lv_label_set_text(g_pcmon_status, "未连接");
}

/* ---------------- 2048 小游戏 ---------------- */

/* 在标准 4x4 棋盘上玩 2048：滑动控制方向，棋盘格存 2 的幂次。
 * 显示层用独立 tile 对象（每个数字方块一个），移动时做滑动+合并弹入动画，更流畅。 */

/* 第 idx 个格子（0..15）相对 g_board_bg 的像素坐标 */
static void cell_xy(int idx, int *x, int *y)
{
    int r = idx / 4, c = idx % 4;
    *x = g_gap + c * (g_cell + g_gap);
    *y = g_gap + r * (g_cell + g_gap);
}

/* 根据数值返回方块背景色/文字色（经典 2048 配色） */
static void tile_colors(uint16_t v, uint32_t *bg, uint32_t *fg)
{
    *fg = 0x000000;
    switch (v) {
    case 2:    *bg = 0xEEE4DA; break;
    case 4:    *bg = 0xEDE0C8; break;
    case 8:    *bg = 0xF2B179; *fg = 0xFFFFFF; break;
    case 16:   *bg = 0xF59563; *fg = 0xFFFFFF; break;
    case 32:   *bg = 0xF67C5F; *fg = 0xFFFFFF; break;
    case 64:   *bg = 0xF65E3B; *fg = 0xFFFFFF; break;
    case 128:  *bg = 0xEDCF72; *fg = 0xFFFFFF; break;
    case 256:  *bg = 0xEDCC61; *fg = 0xFFFFFF; break;
    case 512:  *bg = 0xEDC850; *fg = 0xFFFFFF; break;
    case 1024: *bg = 0xEDC53F; *fg = 0xFFFFFF; break;
    case 2048: *bg = 0xEDC22E; *fg = 0xFFFFFF; break;
    default:   *bg = 0x3C3A32; *fg = 0xFFFFFF; break;   /* 4096+ */
    }
}

/* 把数值写入方块标签，并按位数自适应字号避免溢出 */
static void tile_set_text(lv_obj_t *lbl, uint16_t v)
{
    char buf[8];
    int len = snprintf(buf, sizeof(buf), "%u", (unsigned)v);
    lv_label_set_text(lbl, buf);
    const lv_font_t *f = &lv_font_montserrat_20;
    if (len >= 4)      f = &lv_font_montserrat_14;
    else if (len == 3) f = &lv_font_montserrat_16;
    lv_obj_set_style_text_font(lbl, f, 0);
}

/* 创建一个数字方块对象并放到 idx 位置（调用方负责初始缩放/动画） */
static void tile_create(int idx, uint16_t v)
{
    int x, y; cell_xy(idx, &x, &y);
    uint32_t bg, fg; tile_colors(v, &bg, &fg);
    lv_obj_t *o = lv_obj_create(g_board_bg);
    lv_obj_set_size(o, g_cell, g_cell);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_style_bg_color(o, lv_color_hex(bg), 0);
    lv_obj_set_style_radius(o, 6, 0);
    lv_obj_set_style_border_opa(o, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_set_style_shadow_opa(o, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(o, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *l = lv_label_create(o);
    tile_set_text(l, v);
    lv_obj_set_style_text_color(l, lv_color_hex(fg), 0);
    lv_obj_center(l);
    g_tile_obj[idx] = o;
    g_tile_lbl[idx] = l;
}

/* 在空格随机生成一个新方块，返回其索引（满则返回 -1） */
static int game_spawn(void)
{
    int empty[16], n = 0;
    for (int i = 0; i < 16; i++) if (g_board[i] == 0) empty[n++] = i;
    if (n == 0) return -1;
    int idx = empty[rand() % n];
    g_board[idx] = (rand() % 10 == 0) ? 4 : 2;
    return idx;
}

static void game_update_score(void)
{
    if (g_game_score_label) lv_label_set_text_fmt(g_game_score_label, "分数: %u", (unsigned)g_game_score);
}

/* 全量重绘：删除所有方块对象，按当前 g_board 重建 */
static void game_render(void)
{
    for (int i = 0; i < 16; i++) {
        if (g_tile_obj[i]) { lv_obj_del(g_tile_obj[i]); g_tile_obj[i] = NULL; g_tile_lbl[i] = NULL; }
        if (g_board[i] != 0) tile_create(i, g_board[i]);
    }
}

/* 开新局：清空棋盘，放两个起始方块并重绘 */
static void game_new(void)
{
    for (int i = 0; i < 16; i++) g_board[i] = 0;
    g_game_score = 0;
    game_spawn();
    game_spawn();
    game_render();
    game_update_score();
}

/* 棋盘移动的一步描述：目标值、保留源索引、被合并源索引（-1=无） */
typedef struct { uint16_t val; int keep; int merge; } nl_t;

/* 按方向滑动：先算逻辑并写回 g_board，再全量重绘 */
static void game_move(lv_dir_t dir)
{
    /* 每条线（行/列）按“方向近端->远端”排好的 4 个格子索引 */
    int lines[4][4];
    if (dir == LV_DIR_LEFT || dir == LV_DIR_RIGHT) {
        for (int r = 0; r < 4; r++)
            for (int k = 0; k < 4; k++)
                lines[r][k] = r * 4 + ((dir == LV_DIR_LEFT) ? k : 3 - k);
    } else {
        for (int c = 0; c < 4; c++)
            for (int k = 0; k < 4; k++)
                lines[c][k] = ((dir == LV_DIR_TOP) ? k : 3 - k) * 4 + c;
    }

    nl_t newlist[4][4];
    int m[4] = {0};
    bool changed = false;

    /* 1) 逻辑计算并写回 g_board */
    for (int L = 0; L < 4; L++) {
        int srcs[4], sn = 0;
        for (int k = 0; k < 4; k++) if (g_board[lines[L][k]] != 0) srcs[sn++] = lines[L][k];
        int i = 0;
        while (i < sn) {
            if (i + 1 < sn && g_board[srcs[i]] == g_board[srcs[i + 1]]) {
                uint16_t nv = (uint16_t)(g_board[srcs[i]] * 2);
                g_game_score += nv;
                newlist[L][m[L]].val   = nv;
                newlist[L][m[L]].keep  = srcs[i];
                newlist[L][m[L]].merge = srcs[i + 1];
                changed = true;
                i += 2;
            } else {
                newlist[L][m[L]].val   = g_board[srcs[i]];
                newlist[L][m[L]].keep  = srcs[i];
                newlist[L][m[L]].merge = -1;
                i += 1;
            }
            if (newlist[L][m[L]].keep != lines[L][m[L]]) changed = true;
            m[L]++;
        }
        for (int k = 0; k < 4; k++) g_board[lines[L][k]] = (k < m[L]) ? newlist[L][k].val : 0;
    }
    if (!changed) return;

    /* 2) 移动后生成一个新方块并全量重绘 */
    game_spawn();
    game_render();
    game_update_score();
}

/* 手势回调：屏幕对象上检测到滑动即移动棋盘 */
static void game_gesture_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_GESTURE) return;
    lv_dir_t dir = lv_indev_get_gesture_dir(lv_event_get_indev(e));
    game_move(dir);
}

/* 构建 2048 游戏屏：标题栏右侧分数 + 4x4 棋盘底框（按实际尺寸预算格子大小居中） */
static void build_game_screen(lv_obj_t *scr)
{
    static bool seeded = false;
    if (!seeded) { srand((unsigned)(esp_timer_get_time() & 0xFFFFFFFF)); seeded = true; }

    /* 标题栏右侧显示分数（make_app_screen 已建好标题栏） */
    g_game_score_label = lv_label_create(scr);
    lv_label_set_text(g_game_score_label, "分数: 0");
    lv_obj_set_style_text_font(g_game_score_label, &lv_font_cn_16, 0);
    lv_obj_set_style_text_color(g_game_score_label, lv_color_hex(0x9AA0B5), 0);
    lv_obj_align(g_game_score_label, LV_ALIGN_TOP_RIGHT, -12, 30);

    /* 4x4 棋盘：格子边长取“宽、高可用空间”的较小者并居中，适配任意分辨率 */
    const int W = lv_obj_get_width(scr);
    const int H = lv_obj_get_height(scr);
    const int top = 66;
    const int bottom = 8;
    const int avail_w = W - 24;
    const int avail_h = H - top - bottom;
    g_gap = 6;
    g_cell = ((avail_w < avail_h ? avail_w : avail_h) - 5 * g_gap) / 4;
    const int board_total = 4 * g_cell + 5 * g_gap;
    const int ox = (W - board_total) / 2;
    const int oy = top + (avail_h - board_total) / 2;

    g_board_bg = lv_obj_create(scr);
    lv_obj_set_size(g_board_bg, board_total, board_total);
    lv_obj_set_pos(g_board_bg, ox - g_gap, oy - g_gap);
    lv_obj_set_style_bg_color(g_board_bg, lv_color_hex(0xBBADA0), 0);
    lv_obj_set_style_radius(g_board_bg, 8, 0);
    lv_obj_set_style_border_opa(g_board_bg, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(g_board_bg, 0, 0);
    lv_obj_set_style_pad_all(g_board_bg, 0, 0);
    lv_obj_set_style_shadow_opa(g_board_bg, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(g_board_bg, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(g_board_bg, LV_SCROLLBAR_MODE_OFF);

    /* 16 个固定单元格背景框（空格也显示，作为数字块的底色） */
    for (int i = 0; i < 16; i++) {
        int x, y; cell_xy(i, &x, &y);
        lv_obj_t *c = lv_obj_create(g_board_bg);
        lv_obj_set_size(c, g_cell, g_cell);
        lv_obj_set_pos(c, x, y);
        lv_obj_set_style_bg_color(c, lv_color_hex(0xCDC1B4), 0);
        lv_obj_set_style_radius(c, 6, 0);
        lv_obj_set_style_border_opa(c, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(c, 0, 0);
        lv_obj_set_style_pad_all(c, 0, 0);
        lv_obj_set_style_shadow_opa(c, LV_OPA_TRANSP, 0);
        lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scrollbar_mode(c, LV_SCROLLBAR_MODE_OFF);
        lv_obj_clear_flag(c, LV_OBJ_FLAG_CLICKABLE);
        g_cell_bg[i] = c;
    }

    /* 在屏幕对象上检测滑动手势控制方向 */
    lv_obj_add_event_cb(scr, game_gesture_cb, LV_EVENT_GESTURE, NULL);

    game_new();
}

/* ===================================================================
 * 主循环周期性刷新逻辑（抽出独立函数，保持 lvgl_task 主循环简洁易读）
 * =================================================================== */

/* 触摸初始化：FT6336 注册为 LVGL 指针输入设备 */
/* ---------------- 小说阅读器 ----------------
 * 支持多本书：g_novel_books[] 由 novel_text.h 提供（书名 + 正文指针）。
 * 文本是 const 数组，常驻 Flash，不进 RAM；翻页时只把当前页那一小段
 * 拷进栈上缓冲再交给 LVGL（LVGL 会自行复制一份），因此内存开销极小。
 * 小说屏有两层：书单（列表）-> 点书名进入阅读器（正文 + 翻页）。
 * -------------------------------------------- */
#define NOVEL_PAGE_CHARS   84      /* 每页字符数（按"字"算，不是字节） */

static lv_obj_t *g_novel_screen  = NULL;
static lv_obj_t *g_novel_list    = NULL;   /* 书单容器 */
static lv_obj_t *g_novel_reader  = NULL;   /* 阅读器容器（初始隐藏） */
static lv_obj_t *g_novel_label   = NULL;
static lv_obj_t *g_novel_page_lb = NULL;
static lv_obj_t *g_novel_title   = NULL;   /* 顶栏标题（书单="小说"，阅读=书名） */
static bool      g_novel_in_reader = false;
static int       g_cur_book  = 0;
static int       g_cur_page  = 0;
static int       g_cur_pages = 0;

/* UTF-8：把 pos 移到下一个字符起始，避免按字节截断汉字 */
static size_t novel_utf8_next(const char *s, size_t pos)
{
    while (s[pos] != '\0' && ((unsigned char)s[pos] & 0xC0u) == 0x80u) pos++;
    return pos;
}

/* 从第 pos 个字节起，前进 n 个字符 */
static size_t novel_skip_chars(const char *s, size_t pos, int n)
{
    for (int i = 0; i < n && s[pos] != '\0'; i++) {
        pos = novel_utf8_next(s, pos + 1);
    }
    return pos;
}

static int novel_page_count(const char *text)
{
    int chars = 0;
    for (size_t i = 0; text[i] != '\0'; i = novel_utf8_next(text, i + 1)) {
        chars++;
    }
    return (chars + NOVEL_PAGE_CHARS - 1) / NOVEL_PAGE_CHARS;
}

static void novel_show_page(int page)
{
    if (!g_novel_label) return;
    const char *text = g_novel_books[g_cur_book].text;
    if (g_cur_pages <= 0) g_cur_pages = novel_page_count(text);
    if (page < 0) page = 0;
    if (page > g_cur_pages - 1) page = g_cur_pages - 1;
    g_cur_page = page;

    size_t start = novel_skip_chars(text, 0, page * NOVEL_PAGE_CHARS);
    size_t end   = novel_skip_chars(text, start, NOVEL_PAGE_CHARS);

    size_t n = end - start;
    char buf[NOVEL_PAGE_CHARS * 4];
    if (n >= sizeof(buf)) n = sizeof(buf) - 1;
    memcpy(buf, text + start, n);
    buf[n] = '\0';

    lv_label_set_text(g_novel_label, buf);
    if (g_novel_page_lb) {
        lv_label_set_text_fmt(g_novel_page_lb, "%d / %d", g_cur_page + 1, g_cur_pages);
    }
}

/* 手动滑动翻页（不依赖 LVGL 手势识别，更稳）：
 * 按下时记起点，松开时按水平位移判断：左滑下一页，右滑上一页 */
static int32_t g_swipe_x0 = 0;
static int32_t g_swipe_y0 = 0;

static void novel_press_cb(lv_event_t *e)
{
    lv_indev_t *indev = lv_event_get_indev(e);
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    g_swipe_x0 = p.x;
    g_swipe_y0 = p.y;
}

static void novel_release_cb(lv_event_t *e)
{
    lv_indev_t *indev = lv_event_get_indev(e);
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    int32_t dx  = p.x - g_swipe_x0;
    int32_t dy  = p.y - g_swipe_y0;
    int32_t adx = dx < 0 ? -dx : dx;
    int32_t ady = dy < 0 ? -dy : dy;
    if (adx > 30 && adx > ady * 2) {          // 明显水平滑动才算翻页
        novel_show_page(g_cur_page + (dx < 0 ? 1 : -1));
    }
}

/* 点书名 -> 进入阅读器 */
static void novel_book_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    g_cur_book  = idx;
    g_cur_page  = 0;
    g_cur_pages = novel_page_count(g_novel_books[idx].text);
    g_novel_in_reader = true;
    lv_label_set_text(g_novel_title, g_novel_books[idx].title);
    lv_obj_add_flag(g_novel_list, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(g_novel_reader, LV_OBJ_FLAG_HIDDEN);
    novel_show_page(0);
}

/* 顶栏返回：阅读中 -> 回书单；书单中 -> 回主界面 */
static void novel_top_back_cb(lv_event_t *e)
{
    if (g_novel_in_reader) {
        g_novel_in_reader = false;
        lv_label_set_text(g_novel_title, "小说");
        lv_obj_clear_flag(g_novel_list, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(g_novel_reader, LV_OBJ_FLAG_HIDDEN);
    } else {
        go_home_cb(e);
    }
}

static void build_novel_screen(lv_obj_t *scr)
{
    /* 状态栏由 lvgl_ui_bootstrap 在 lv_layer_top() 上统一创建 */

    /* 顶栏：返回 + 标题 */
    lv_obj_t *bar = lv_obj_create(scr);
    lv_obj_set_size(bar, LCD_W, 40);
    lv_obj_align(bar, LV_ALIGN_TOP_LEFT, 0, 22);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x16161F), 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_border_opa(bar, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(bar, 8, 0);
    lv_obj_set_style_pad_column(bar, 8, 0);
    lv_obj_t *bk = lv_button_create(bar);
    lv_obj_set_size(bk, 80, 32);
    lv_obj_t *bkl = lv_label_create(bk);
    lv_label_set_text(bkl, "< 返回");
    lv_obj_set_style_text_font(bkl, &lv_font_cn_16, 0);
    lv_obj_center(bkl);
    lv_obj_add_event_cb(bk, novel_top_back_cb, LV_EVENT_CLICKED, NULL);
    g_novel_title = lv_label_create(bar);
    lv_label_set_text(g_novel_title, "小说");
    lv_obj_set_style_text_font(g_novel_title, &lv_font_cn_16, 0);
    lv_obj_set_style_text_color(g_novel_title, lv_color_white(), 0);
    lv_obj_set_style_pad_left(g_novel_title, 10, 0);

    /* 书单（可滚动列表） */
    g_novel_list = lv_obj_create(scr);
    lv_obj_set_size(g_novel_list, LCD_W, LCD_H - 62);
    lv_obj_align(g_novel_list, LV_ALIGN_TOP_LEFT, 0, 62);
    lv_obj_set_style_bg_color(g_novel_list, lv_color_hex(0x101018), 0);
    lv_obj_set_style_radius(g_novel_list, 0, 0);
    lv_obj_set_style_border_opa(g_novel_list, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(g_novel_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(g_novel_list, 6, 0);
    lv_obj_set_style_pad_row(g_novel_list, 8, 0);
    lv_obj_set_scroll_dir(g_novel_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(g_novel_list, LV_SCROLLBAR_MODE_AUTO);
    for (int i = 0; i < NOVEL_BOOK_COUNT; i++) {
        lv_obj_t *row = lv_button_create(g_novel_list);
        lv_obj_set_width(row, lv_pct(100));
        lv_obj_set_height(row, 46);
        lv_obj_set_style_bg_color(row, lv_color_hex(0x23232F), 0);
        lv_obj_set_style_radius(row, 10, 0);
        lv_obj_set_style_border_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);   // 行不可单独滚动
        lv_obj_set_scrollbar_mode(row, LV_SCROLLBAR_MODE_OFF);
        lv_obj_t *nm = lv_label_create(row);
        lv_label_set_text(nm, g_novel_books[i].title);
        lv_obj_set_style_text_font(nm, &lv_font_cn_16, 0);
        lv_obj_set_style_text_color(nm, lv_color_white(), 0);
        lv_obj_center(nm);
        lv_obj_clear_flag(nm, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, novel_book_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }

    /* 阅读器（初始隐藏） */
    g_novel_reader = lv_obj_create(scr);
    lv_obj_set_size(g_novel_reader, LCD_W, LCD_H - 62);
    lv_obj_align(g_novel_reader, LV_ALIGN_TOP_LEFT, 0, 62);
    lv_obj_set_style_bg_color(g_novel_reader, lv_color_hex(0x101018), 0);
    lv_obj_set_style_radius(g_novel_reader, 0, 0);
    lv_obj_set_style_border_opa(g_novel_reader, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(g_novel_reader, LV_OBJ_FLAG_SCROLLABLE);   // 不可滚动，拖动才算翻页手势
    lv_obj_set_scrollbar_mode(g_novel_reader, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(g_novel_reader, LV_OBJ_FLAG_CLICKABLE);       // 可点击，手势事件才会派发到阅读器
    lv_obj_add_flag(g_novel_reader, LV_OBJ_FLAG_HIDDEN);

    /* 正文：取消翻页按钮后占满阅读区，不再被遮挡 */
    g_novel_label = lv_label_create(g_novel_reader);
    lv_obj_clear_flag(g_novel_label, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(g_novel_label, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_text_font(g_novel_label, &lv_font_cn_16, 0);
    lv_obj_set_style_text_color(g_novel_label, lv_color_hex(0xDDDDDD), 0);
    lv_obj_set_style_text_line_space(g_novel_label, 2, 0);
    lv_label_set_long_mode(g_novel_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(g_novel_label, LCD_W - 24);          // 宽度固定，高度自适应内容
    lv_obj_set_height(g_novel_label, LV_SIZE_CONTENT);    // 避免末行被固定高度裁掉
    lv_obj_align(g_novel_label, LV_ALIGN_TOP_LEFT, 12, 8);

    /* 页码（底部居中；翻页靠滑动：左滑下一页 / 右滑上一页） */
    g_novel_page_lb = lv_label_create(g_novel_reader);
    lv_obj_set_style_text_font(g_novel_page_lb, &lv_font_cn_16, 0);
    lv_obj_set_style_text_color(g_novel_page_lb, lv_color_hex(0x888888), 0);
    lv_obj_align(g_novel_page_lb, LV_ALIGN_BOTTOM_MID, 0, -6);

    /* 滑动翻页（手动计算，左滑下一页 / 右滑上一页；只在阅读器上，书单不受影响） */
    lv_obj_add_event_cb(g_novel_reader, novel_press_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(g_novel_reader, novel_release_cb, LV_EVENT_RELEASED, NULL);
}

static void lvgl_touch_setup(void)
{
    if (ft6336_init() == ESP_OK) {
        lv_indev_t *indev = lv_indev_create();
        lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(indev, touch_read_cb);
        ESP_LOGI("TOUCH", "FT6336 indev registered");
    } else {
        ESP_LOGE("TOUCH", "FT6336 init failed");
    }
}

/* 主界面 + 触摸红点 + 进入主界面 */
static void lvgl_home_setup(void)
{
    /* 仅构建主界面；其它 App 屏幕首次点击时再懒加载，避免启动时一次性创建
     * 大量对象导致 lvgl 任务卡 >5s 触发看门狗 */
    build_home();

    /* 触摸红点放在最顶层 layer 上，任何界面都可见且始终在最前 */
    lv_obj_t *top_layer = lv_layer_top();
    /* 关键：让顶层 layer 本身不拦截触摸，否则它会盖在所有界面之上吞掉点击 */
    lv_obj_clear_flag(top_layer, LV_OBJ_FLAG_CLICKABLE);
    g_red_dot = lv_obj_create(top_layer);
    lv_obj_remove_style_all(g_red_dot);
    lv_obj_set_size(g_red_dot, 18, 18);
    lv_obj_set_style_bg_color(g_red_dot, lv_color_hex(0xFF0000), 0);
    lv_obj_set_style_bg_opa(g_red_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(g_red_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_add_flag(g_red_dot, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(g_red_dot, LV_OBJ_FLAG_CLICKABLE);   // 关键：不拦截触摸事件

    /* 启动后进入主界面 */
    lv_screen_load(g_home);
}

/* ---------------- 诊断用：FPS 显示 ----------------
 * 在顶层 layer 放一个 FPS 标签，统计每秒 lv_timer_handler() 的执行次数。
 * 与触摸红点同样处理：不拦截触摸，任何界面上都可见。确认问题后可整段删除。
 * -------------------------------------------------- */
static lv_obj_t *g_fps_label   = NULL;
static uint32_t  g_fps_frames  = 0;
static uint32_t  g_fps_last_ms = 0;

static void lvgl_fps_setup(void)
{
    g_fps_label = lv_label_create(lv_layer_top());
    lv_obj_set_style_text_font(g_fps_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(g_fps_label, lv_color_hex(0x00FF00), 0);
    lv_obj_set_style_bg_color(g_fps_label, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(g_fps_label, LV_OPA_60, 0);
    lv_obj_set_style_pad_all(g_fps_label, 2, 0);
    lv_label_set_text(g_fps_label, "-- fps");
    /* 放右下角，避开状态栏与各屏标题栏；不拦截触摸，避免吞掉点击 */
    lv_obj_align(g_fps_label, LV_ALIGN_BOTTOM_RIGHT, -2, -2);
    lv_obj_clear_flag(g_fps_label, LV_OBJ_FLAG_CLICKABLE);
}

/* 每秒读 g_flush_cnt 差值, 显示真实刷屏 FPS(不是主循环圈数)。
 * g_flush_cnt 由 lvgl_port.c 的 disp_flush_cb 每次 DMA 完成时 ++,
 * 静止时几乎不增加(没脏区), 滑动时持续增加 → 真实反映屏幕刷新率。 */
extern uint32_t g_flush_cnt;   /* lvgl_port.c 全局 */

static void ui_fps_update(void)
{
    if (!g_fps_label) return;

    uint32_t now = lv_tick_get();
    uint32_t dt  = now - g_fps_last_ms;
    if (dt < 1000) return;

    /* 读 g_flush_cnt 差值 = 这一秒内的真实刷屏次数 */
    uint32_t cur  = g_flush_cnt;
    uint32_t diff = cur - g_fps_frames;   /* g_fps_frames 复用作"上次计数" */
    uint32_t fps  = (uint32_t)((uint64_t)diff * 1000U / dt);
    lv_label_set_text_fmt(g_fps_label, "%u fps", (unsigned)fps);
    g_fps_frames  = cur;   /* 保存当前计数, 下次差值用 */
    g_fps_last_ms = now;
}

static void ui_alarm_popup_create(void);  /* 前置声明：定义在文件后部 */
/* LVGL 启动初始化：端口、背光、触摸、WiFi、天气后台任务、主界面与触摸红点 */
static void lvgl_ui_bootstrap(void)
{
    lvgl_port_init();                       /* LVGL 显示端口 */
    backlight_init();                       /* 背光 PWM */
    backlight_set(g_brightness);            /* 默认全亮 */
    lvgl_touch_setup();                     /* 触摸输入 */
    if (wifi_scan_init() != ESP_OK) {       /* WiFi 扫描后台任务 */
        ESP_LOGE("WIFI", "wifi scan init failed");
    }
    weather_init();                         /* 天气后台任务 */
    pc_mon_init();                          /* 电脑监控后台任务 */
    /* 闹钟：从 NVS 载入上次设置。必须排在 wifi_scan_init() 之后，
     * 因为 nvs_flash_init() 是由它内部完成的 */
    alarm_init();
    if (ble_scan_init() != ESP_OK) {          /* BLE 扫描后台任务 */
        ESP_LOGE("BLE", "ble scan init failed");
    }
    lvgl_home_setup();                      /* 主界面 + 触摸红点 */
    make_status_bar(lv_layer_top());        /* 状态栏统一挂在顶层 layer，不随任何 screen 滚动/切换 */
    lvgl_fps_setup();                       /* 诊断：FPS 显示 */
    ui_alarm_popup_create();                /* 闹钟弹窗（挂在顶层 layer） */
}

/* 周期刷新天气：只有当前正停留在天气界面才发请求，
 * 在其它界面（主界面、监控、小说…）一个请求都不发，避免白白联网。 */
static void ui_weather_periodic(void)
{
    if (g_wifi_state == WIFI_STA_CONNECTED && s_prev_wifi_state != WIFI_STA_CONNECTED) {
        /* 刚连上：只重置时间戳，真正的请求交给「进入天气界面」触发，
         * 避免用户其实不在看天气时白拉一次 */
        s_last_wx_req_ms = lv_tick_get();
    }
    s_prev_wifi_state = g_wifi_state;

    /* 不在天气界面：直接跳过，不做任何网络请求 */
    if (!g_weather_screen || lv_screen_active() != g_weather_screen) return;

    ui_weather_refresh_if_needed(false);
}

/* Clock 屏：连上网自动校时后显示本地时间（每秒刷新一次） */
static void ui_clock_refresh(void)
{
    static uint64_t s_clock_ms = 0;
    uint64_t now_ms = esp_timer_get_time() / 1000;
    if (!g_clock_screen || !g_clock_time) return;
    if (now_ms - s_clock_ms < 1000) return;
    s_clock_ms = now_ms;

    struct tm tm;
    if (time_sync_localtime(&tm)) {
        char buf[48];
        strftime(buf, sizeof(buf), "%H:%M:%S", &tm);
        lv_label_set_text(g_clock_time, buf);

        /* 日期中文化：strftime 的 %a/%b 只输出英文缩写（如 "Sat Sep 12 2026"），
         * newlib 也没有可用的中文 locale，所以这里手动拼装成
         * "2026年9月12日 星期六" 这种格式。 */
        static const char *wd_cn[] = {"星期日", "星期一", "星期二", "星期三",
                                      "星期四", "星期五", "星期六"};
        int wd = tm.tm_wday;                 /* tm_wday: 0=周日 .. 6=周六 */
        if (wd < 0) wd = 0;
        if (wd > 6) wd = 6;
        snprintf(buf, sizeof(buf), "%d年%d月%d日 %s",
                 tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, wd_cn[wd]);
        lv_label_set_text(g_clock_date, buf);
    } else {
        lv_label_set_text(g_clock_time, "--:--:--");
        lv_label_set_text(g_clock_date, "正在同步时间...");
    }
}

/* 点「停止」：关掉弹窗，并通知 alarm 模块本次已处理，本分钟不再重复弹 */
static void alarm_stop_cb(lv_event_t *e)
{
    (void)e;
    alarm_dismiss();
    if (g_alarm_popup) lv_obj_add_flag(g_alarm_popup, LV_OBJ_FLAG_HIDDEN);
}

/* 闹钟弹窗：挂在 lv_layer_top() 上，所以无论当前停在哪个界面都能盖在最前面。
 * 顶层 layer 本身已在 lvgl_home_setup() 里清掉 CLICKABLE（否则会吞掉所有点击），
 * 但弹窗自己是可点击的，显示时会拦住背后界面的操作——这正是模态想要的效果。
 *
 * 布局（box 240×150）：
 *   ┌────────────────────┐
 *   │      闹钟 N         ← 标题 16pt（上方 ~12..30）
 *   │      07:30          ← 时间 24pt（中段 ~53..77）
 *   │                      ← 留 ~27px 间距，确保按钮不挡时间
 *   │      [ 停止 ]        ← 按钮 100×34（底部 ~104..138）
 *   └────────────────────┘
 */
static lv_obj_t *g_alarm_popup_title = NULL;   /* 标题 label：触发时设为「闹钟 N」 */

static void ui_alarm_popup_create(void)
{
    g_alarm_popup = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(g_alarm_popup);
    lv_obj_set_size(g_alarm_popup, LCD_W, LCD_H);
    lv_obj_align(g_alarm_popup, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(g_alarm_popup, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(g_alarm_popup, LV_OPA_70, 0);
    lv_obj_add_flag(g_alarm_popup, LV_OBJ_FLAG_HIDDEN);

    g_alarm_popup_box = lv_obj_create(g_alarm_popup);
    lv_obj_set_size(g_alarm_popup_box, 240, 150);
    lv_obj_align(g_alarm_popup_box, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(g_alarm_popup_box, lv_color_hex(0x1A1A24), 0);
    lv_obj_set_style_bg_opa(g_alarm_popup_box, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(g_alarm_popup_box, 12, 0);
    lv_obj_set_style_border_width(g_alarm_popup_box, 3, 0);
    lv_obj_set_style_border_color(g_alarm_popup_box, lv_color_hex(0xE08A2B), 0);
    lv_obj_clear_flag(g_alarm_popup_box, LV_OBJ_FLAG_SCROLLABLE);

    g_alarm_popup_title = lv_label_create(g_alarm_popup_box);
    lv_label_set_text(g_alarm_popup_title, "闹钟");
    lv_obj_set_style_text_font(g_alarm_popup_title, &lv_font_cn_16, 0);
    lv_obj_set_style_text_color(g_alarm_popup_title, lv_color_hex(0xE08A2B), 0);
    lv_obj_align(g_alarm_popup_title, LV_ALIGN_TOP_MID, 0, 10);

    g_alarm_popup_msg = lv_label_create(g_alarm_popup_box);
    lv_label_set_text(g_alarm_popup_msg, "--:--");
    lv_obj_set_style_text_font(g_alarm_popup_msg, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(g_alarm_popup_msg, lv_color_white(), 0);
    /* 时间 label 居中略偏上（-10），下方留出空间放按钮，不再被按钮挡住 */
    lv_obj_align(g_alarm_popup_msg, LV_ALIGN_CENTER, 0, -10);

    lv_obj_t *btn = lv_button_create(g_alarm_popup_box);
    lv_obj_set_size(btn, 100, 34);
    lv_obj_align(btn, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x2E7DE0), 0);
    lv_obj_t *bl = lv_label_create(btn);
    lv_label_set_text(bl, "停止");
    lv_obj_set_style_text_font(bl, &lv_font_cn_16, 0);
    lv_obj_center(bl);
    lv_obj_add_event_cb(btn, alarm_stop_cb, LV_EVENT_CLICKED, NULL);
}

/* 闹钟检查：每秒一次，到点弹窗；弹窗可见时闪烁边框吸引注意
 * （板上没有蜂鸣器，只能做视觉提醒） */
static void ui_alarm_refresh(void)
{
    static uint64_t s_ms = 0;
    static bool     s_blink = false;
    uint64_t now_ms = esp_timer_get_time() / 1000;
    if (now_ms - s_ms < 1000) return;
    s_ms = now_ms;

    struct tm tm;
    if (!time_sync_localtime(&tm)) return;   /* 还没校时，无法判断是否到点 */

    int idx = -1;
    if (alarm_poll(&tm, &idx)) {
        const alarm_conf_t *a = alarm_get(idx);
        char tbuf[8];   /* "07:30" + NUL */
        snprintf(tbuf, sizeof(tbuf), "%02d:%02d", a->hour, a->minute);
        if (g_alarm_popup_msg) lv_label_set_text(g_alarm_popup_msg, tbuf);
        if (g_alarm_popup_title) {
            char nbuf[24];   /* gcc 按 int 最坏情况算：%d 11B + 前缀 7B + NUL = 19B */
            snprintf(nbuf, sizeof(nbuf), "闹钟 %d", idx + 1);
            lv_label_set_text(g_alarm_popup_title, nbuf);
        }
        if (g_alarm_popup) lv_obj_clear_flag(g_alarm_popup, LV_OBJ_FLAG_HIDDEN);
    }

    /* 弹窗可见时每秒切换一次边框颜色，形成闪烁 */
    if (g_alarm_popup && !lv_obj_has_flag(g_alarm_popup, LV_OBJ_FLAG_HIDDEN)) {
        s_blink = !s_blink;
        if (g_alarm_popup_box) {
            lv_obj_set_style_border_color(g_alarm_popup_box,
                s_blink ? lv_color_hex(0xE0453B) : lv_color_hex(0x2A2A38), 0);
        }
    }
}

/* 状态栏时钟：所有界面复用的状态栏时间，统一每秒刷新 */
static void ui_status_clocks_refresh(void)
{
    static uint64_t s_status_ms = 0;
    uint64_t now_ms = esp_timer_get_time() / 1000;
    if (now_ms - s_status_ms < 1000) return;
    s_status_ms = now_ms;

    struct tm tm;
    if (time_sync_localtime(&tm)) {
        char buf[16];
        strftime(buf, sizeof(buf), "%H:%M", &tm);
        for (int i = 0; i < g_status_clock_cnt; i++) {
            lv_label_set_text(g_status_clocks[i], buf);
        }
    }
}

/* Weather 屏：数据有变化才刷新界面 */
static void ui_weather_screen_refresh(void)
{
    lv_obj_t *active = lv_screen_active();
    if (!g_weather_screen || active != g_weather_screen || !g_wx_temp) return;

    bool changed = (g_weather.version != s_wx_shown_ver) ||
                   (g_weather.fetching != s_wx_fetching) ||
                   s_wx_need_redraw;
    if (!changed) return;

    s_wx_need_redraw = false;
    s_wx_shown_ver = g_weather.version;
    s_wx_fetching  = g_weather.fetching;
    char buf[48];
    if (g_weather.fetching) {
        lv_label_set_text(g_wx_updated, "更新中...");
    } else if (g_weather.valid) {
        lv_label_set_text(g_wx_city, g_weather.city);
        snprintf(buf, sizeof(buf), "%.1f °C", g_weather.temp);
        lv_label_set_text(g_wx_temp, buf);
        lv_label_set_text(g_wx_desc, g_weather.desc);
        snprintf(buf, sizeof(buf), "湿度 %d%%   风速 %.1f km/h",
                 g_weather.humidity, g_weather.wind);
        lv_label_set_text(g_wx_extra, buf);
        int ago_s = (int)((esp_timer_get_time() / 1000 - g_weather.updated_ms) / 1000);
        if (ago_s < 0) ago_s = 0;
        snprintf(buf, sizeof(buf), "%d 秒前更新", ago_s);
        lv_label_set_text(g_wx_updated, buf);
    } else if (g_weather.error) {
        lv_label_set_text(g_wx_desc, "获取失败");
        lv_label_set_text(g_wx_updated, "请检查网络");
    }
}

/* 电脑监控屏：数据有变化才刷新界面（每秒一次） */
static void ui_pcmon_screen_refresh(void)
{
    lv_obj_t *active = lv_screen_active();
    if (!g_pcmon_screen || active != g_pcmon_screen) return;

    if (g_pcmon.version == s_pcmon_shown_ver && !g_pcmon.error) return;
    s_pcmon_shown_ver = g_pcmon.version;

    char buf[64];
    for (int i = 0; i < PC_MON_MAX_SENSORS; i++) {
        if (i < g_pcmon.count) {
            const pc_sensor_t *s = &g_pcmon.sensors[i];
            if (s->unit[0])
                snprintf(buf, sizeof(buf), "%s: %s %s", s->label, s->value, s->unit);
            else
                snprintf(buf, sizeof(buf), "%s: %s", s->label, s->value);
            lv_label_set_text(g_pcmon_labels[i], buf);
        } else {
            lv_label_set_text(g_pcmon_labels[i], "");
        }
    }

    if (g_pcmon.error) {
        lv_label_set_text(g_pcmon_status, "连接失败，请检查 PC 服务端");
    } else if (g_pcmon.valid) {
        int ago_s = (int)((esp_timer_get_time() / 1000 - g_pcmon.updated_ms) / 1000);
        if (ago_s < 0) ago_s = 0;
        snprintf(buf, sizeof(buf), "已连接 · %d 秒前更新", ago_s);
        lv_label_set_text(g_pcmon_status, buf);
    } else {
        lv_label_set_text(g_pcmon_status, "等待连接...");
    }
}

/* 需要输密码：键盘按钮很多，在这里（而非输入回调里）创建并弹出面板 */
static void ui_handle_pending_password(void)
{
    if (!g_pending_pwd) return;
    g_pending_pwd = false;
    if (!g_pwd_panel) {
        build_pwd_panel(g_wifi_screen);
    }
    lv_label_set_text_fmt(g_pwd_ssid, "%s 的密码", g_pending_ssid);
    lv_textarea_set_text(g_pwd_ta, "");
    lv_keyboard_set_textarea(g_pwd_kb, g_pwd_ta);
    lv_obj_clear_flag(g_pwd_panel, LV_OBJ_FLAG_HIDDEN);
}

/* 延迟跳转：点击 App 时只置位 g_pending_app，真正建屏/加载在这里执行，
 * 不在输入事件回调里重入 lv_timer_handler 同步建大量对象（否则卡死看门狗） */
static void ui_handle_pending_app(void)
{
    if (g_pending_app < 0) return;
    int id = g_pending_app;
    g_pending_app = -1;
    lv_obj_t *scr = NULL;
    switch (id) {
    case APP_WIFI:
        if (!g_wifi_screen) {
            g_wifi_screen = lv_obj_create(NULL);
            build_wifi_screen(g_wifi_screen);
        }
        scr = g_wifi_screen;
        break;
    case APP_SETTINGS:
        if (!g_settings_screen) {
            g_settings_screen = make_app_screen("设置");
            build_settings_screen(g_settings_screen);
        }
        scr = g_settings_screen;
        break;
    case APP_CLOCK:
        if (!g_clock_screen) {
            g_clock_screen = make_app_screen("时钟");
            build_clock_screen(g_clock_screen);
        }
        scr = g_clock_screen;
        break;
    case APP_MUSIC:
        if (!g_music_screen) { g_music_screen = make_app_screen("音乐"); add_placeholder(g_music_screen, "音乐\n(暂未实现)"); }
        scr = g_music_screen;
        break;
    case APP_GAME:
        if (!g_game_screen) {
            g_game_screen = make_app_screen("2048");
            build_game_screen(g_game_screen);
        }
        scr = g_game_screen;
        break;
    case APP_WEATHER:
        if (!g_weather_screen) {
            g_weather_screen = make_app_screen("天气");
            build_weather_screen(g_weather_screen);
        }
        scr = g_weather_screen;
        /* 进入天气屏必定拉一次（force=true），保证看到的是最新数据——
         * 之前用「数据新鲜就不请求」会让界面一直显示上次的缓存，观感像没刷新。
         * 若上一次请求还在进行中，函数内部会跳过，不会重复排队。 */
        ui_weather_refresh_if_needed(true);
        s_wx_need_redraw = true;   /* 进界面强制重绘一次，确保数据/缓存都能渲染出来 */
        break;
    case APP_NOVEL:
        if (!g_novel_screen) {
            g_novel_screen = lv_obj_create(NULL);
            lv_obj_set_style_bg_color(g_novel_screen, lv_color_hex(0x101018), 0);
            lv_obj_set_style_bg_opa(g_novel_screen, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(g_novel_screen, 0, 0);
            lv_obj_set_style_border_opa(g_novel_screen, LV_OPA_TRANSP, 0);
            lv_obj_clear_flag(g_novel_screen, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_scrollbar_mode(g_novel_screen, LV_SCROLLBAR_MODE_OFF);
            build_novel_screen(g_novel_screen);
        }
        scr = g_novel_screen;
        break;
    case APP_PCMON:
        if (!g_pcmon_screen) {
            g_pcmon_screen = make_app_screen("电脑监控");
            build_pcmon_screen(g_pcmon_screen);
        }
        scr = g_pcmon_screen;
        break;
    case APP_ALARM:
        if (!g_alarm_screen) {
            g_alarm_screen = make_app_screen("闹钟");
            build_alarm_screen(g_alarm_screen);
        }
        scr = g_alarm_screen;
        break;
    case APP_BLE:
        if (!g_ble_screen) {
            g_ble_screen = lv_obj_create(NULL);
            build_ble_screen(g_ble_screen);
        }
        scr = g_ble_screen;
        break;
    default:
        break;
    }
    if (scr) {
        if (id == APP_WIFI && g_wifi_on) {
            wifi_scan_trigger();
        }
        if (id == APP_BLE && g_ble_on) {
            ble_scan_trigger();
        }
        lv_screen_load(scr);
        /* 只有停留在监控界面时才让 pc_mon 联网拉数据，其余界面一律停止 */
        pc_mon_set_active(scr == g_pcmon_screen);
    }
}

/* 触摸红点：按下时显示并跟随手指坐标，松开时隐藏。
 * 只在「按下状态发生变化」时才动 HIDDEN 标志——主循环每圈都执行
 * lv_obj_add_flag/clear_flag 会反复把显示标脏，导致 LVGL 认为需要立刻重绘，
 * lv_timer_handler() 便一直返回 0，主循环空转。 */
static void ui_refresh_touch_dot(void)
{
    static bool s_dot_shown = false;
    if (g_touch_pressed) {
        lv_obj_set_pos(g_red_dot, g_touch_x - 12, g_touch_y - 12);
        if (!s_dot_shown) {
            lv_obj_clear_flag(g_red_dot, LV_OBJ_FLAG_HIDDEN);
            s_dot_shown = true;
        }
    } else {
        if (s_dot_shown) {
            lv_obj_add_flag(g_red_dot, LV_OBJ_FLAG_HIDDEN);
            s_dot_shown = false;
        }
    }
}

/* 有新的扫描结果则刷新网络列表（原地更新固定行池，不新建对象） */
static void ui_refresh_wifi_list(void)
{
    if (!g_ap_updated) return;
    g_ap_updated = false;
    if (g_wifi_on) {
        /* 连接中/已连接时副标题由 wifi_status_update 占用，这里别覆盖 */
        if (g_wifi_state == WIFI_STA_IDLE || g_wifi_state == WIFI_STA_FAILED) {
            lv_label_set_text_fmt(g_subtitle, "找到 %d 个网络", g_ap_count);
        }
        int n = g_ap_count < WIFI_LIST_ROWS ? g_ap_count : WIFI_LIST_ROWS;
        for (int i = 0; i < WIFI_LIST_ROWS; i++) {
            wifi_row_t *r = &g_rows[i];
            if (i < n) {
                const char *s = g_ap_list[i].ssid[0] ? g_ap_list[i].ssid : "<hidden>";
                lv_label_set_text(r->name, s);
                int lvl = g_ap_list[i].rssi >= -50 ? 4
                        : g_ap_list[i].rssi >= -60 ? 3
                        : g_ap_list[i].rssi >= -70 ? 2
                        : g_ap_list[i].rssi >= -80 ? 1 : 0;
                /* 信号单块：按档位调色（强=绿，弱=灰） */
                lv_obj_set_style_bg_color(r->sig,
                    lvl >= 3 ? lv_color_hex(0x35D04A)
                             : lvl == 2 ? lv_color_hex(0xB7C23A)
                             : lv_color_hex(0x55556A), 0);
                if (g_ap_list[i].authmode != WIFI_AUTH_OPEN) {
                    lv_obj_clear_flag(r->lock, LV_OBJ_FLAG_HIDDEN);
                } else {
                    lv_obj_add_flag(r->lock, LV_OBJ_FLAG_HIDDEN);
                }
                r->ap_idx = i;
                lv_obj_clear_flag(r->row, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(r->row, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
}

/* 有新的 BLE 扫描结果则刷新设备列表（原地更新固定行池） */
static void ui_refresh_ble_list(void)
{
    if (!g_ble_dev_updated) return;
    g_ble_dev_updated = false;
    if (!g_ble_on) return;

    if (g_ble_state == BLE_STATE_IDLE || g_ble_state == BLE_STATE_FAILED) {
        if (g_ble_subtitle) lv_label_set_text_fmt(g_ble_subtitle, "找到 %d 个设备", g_ble_dev_count);
    }
    int n = g_ble_dev_count < BLE_LIST_ROWS ? g_ble_dev_count : BLE_LIST_ROWS;
    for (int i = 0; i < BLE_LIST_ROWS; i++) {
        ble_row_t *r = &g_ble_rows[i];
        if (i < n) {
            char label[40];
            if (g_ble_dev_list[i].name[0]) {
                lv_label_set_text(r->name, g_ble_dev_list[i].name);
            } else {
                const uint8_t *a = g_ble_dev_list[i].addr;
                snprintf(label, sizeof(label), "%02X:%02X:%02X:%02X:%02X:%02X",
                         a[5], a[4], a[3], a[2], a[1], a[0]);
                lv_label_set_text(r->name, label);
            }
            int lvl = g_ble_dev_list[i].rssi >= -50 ? 4
                    : g_ble_dev_list[i].rssi >= -60 ? 3
                    : g_ble_dev_list[i].rssi >= -70 ? 2
                    : g_ble_dev_list[i].rssi >= -80 ? 1 : 0;
            lv_obj_set_style_bg_color(r->sig,
                lvl >= 3 ? lv_color_hex(0x35D04A)
                         : lvl == 2 ? lv_color_hex(0xB7C23A)
                         : lv_color_hex(0x55556A), 0);
            r->dev_idx = i;
            lv_obj_clear_flag(r->row, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(r->row, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

/* 刷新 BLE 详情页的状态文字与按钮 */
static void ui_ble_detail_refresh(void)
{
    if (!g_ble_detail || !g_ble_detail_status || !g_ble_detail_btn) return;
    if (lv_obj_has_flag(g_ble_detail, LV_OBJ_FLAG_HIDDEN)) return;
    if (g_ble_detail_idx < 0 || g_ble_detail_idx >= g_ble_dev_count) return;

    ble_device_t *d = &g_ble_dev_list[g_ble_detail_idx];
    bool is_current = (g_ble_conn_name[0] != '\0') &&
                      (d->name[0] && strncmp(g_ble_conn_name, d->name, 31) == 0);

    char status[64];
    const char *btn;

    if (g_ble_state == BLE_STATE_CONNECTED && is_current) {
        snprintf(status, sizeof(status), "已连接");
        btn = "断开";
    } else if (g_ble_state == BLE_STATE_CONNECTING && is_current) {
        snprintf(status, sizeof(status), "正在连接...");
        btn = "取消";
    } else if (g_ble_state == BLE_STATE_FAILED && is_current) {
        snprintf(status, sizeof(status), "连接失败");
        btn = "连接";
    } else if (!d->is_connectable) {
        snprintf(status, sizeof(status), "不可连接（仅广播）");
        btn = "连接";
    } else {
        snprintf(status, sizeof(status), "未连接");
        btn = "连接";
    }

    static char s_last_ble_status[64] = "";
    static char s_last_ble_btn[16] = "";
    if (strcmp(status, s_last_ble_status) != 0) {
        lv_label_set_text(g_ble_detail_status, status);
        strncpy(s_last_ble_status, status, sizeof(s_last_ble_status) - 1);
        s_last_ble_status[sizeof(s_last_ble_status) - 1] = '\0';
    }
    if (strcmp(btn, s_last_ble_btn) != 0) {
        lv_label_set_text(lv_obj_get_child(g_ble_detail_btn, 0), btn);
        strncpy(s_last_ble_btn, btn, sizeof(s_last_ble_btn) - 1);
        s_last_ble_btn[sizeof(s_last_ble_btn) - 1] = '\0';
    }
}

/* ---------------- 诊断用：LVGL 内存与刷新耗时 ----------------
 * 目的：定位 task_wdt 复位。打印 LV_MEM 使用率 / 碎片率 / 最大连续空闲块，
 * 以及单次 lv_timer_handler() 的耗时。确认问题后整段删除即可。
 * ---------------------------------------------------------- */
static void ui_mem_dump(const char *tag)
{
    lv_mem_monitor_t mon;
    lv_mem_monitor(&mon);
    ESP_LOGW("LVMEM", "[%s] total=%u  used=%u%%  frag=%u%%  maxfree=%u",
             tag,
             (unsigned)mon.total_size,
             (unsigned)mon.used_pct,
             (unsigned)mon.frag_pct,
             (unsigned)mon.free_biggest_size);
}

/* 每 5 秒打一次，观察内存占用是否随使用时间持续上涨 */
static void ui_mem_dump_periodic(void)
{
    static uint32_t s_last_ms = 0;
    uint32_t now = lv_tick_get();
    if (now - s_last_ms < 5000) return;
    s_last_ms = now;
    ui_mem_dump("5s");
}

/* LVGL 主循环放在独立任务里运行，给足栈空间避免栈溢出 */
static void lvgl_task(void *arg)
{
    (void)arg;
    lvgl_ui_bootstrap();

    /* 主循环：LVGL 自带 tick（1ms 由 esp_timer 提供），无需手动计时 */
    while (1) {
        /* 状态栏 / 详情页状态刷新 */
        wifi_status_update();
        update_detail_ui();
        ui_ble_detail_refresh();

        /* 各屏数据周期性刷新：天气自动拉取、时钟、状态栏时钟、天气屏 */
        ui_weather_periodic();
        ui_clock_refresh();
        ui_alarm_refresh();
        ui_status_clocks_refresh();   /* 状态栏时钟（恢复） */
        ui_weather_screen_refresh();
        ui_pcmon_screen_refresh();

        /* 延迟任务：密码面板、App 跳转、触摸红点、WiFi 列表刷新 */
        ui_handle_pending_password();
        int opened_app = g_pending_app;           /* 诊断：本圈是否打开了 App */
        ui_handle_pending_app();
        if (opened_app >= 0) ui_mem_dump("app");  /* 建屏后立刻打印，看单个 App 吃掉多少 */
        // ui_refresh_touch_dot();           /* 触摸红点不再使用（INT 中断后无需可视化调试） */
        ui_refresh_wifi_list();
        ui_refresh_ble_list();

        /* 诊断：每 5 秒打印一次 LV_MEM 占用 */
        ui_mem_dump_periodic();

        /* 处理 LVGL 任务，返回建议休眠 ms */
        uint32_t t0 = lv_tick_get();
        uint32_t t = lv_timer_handler();
        uint32_t dt = lv_tick_get() - t0;
        if (dt > 200) ESP_LOGW("LVMEM", "lv_timer_handler took %u ms", (unsigned)dt);
        ui_fps_update();                    /* 诊断：帧率显示 */

        /* FREERTOS_HZ=100 时 pdMS_TO_TICKS() 会把 <10ms 的值取整成 0,
         * 而 vTaskDelay(0) 只是 taskYIELD() 并不阻塞，会让主循环全速空转
         * （实测静止时 5000fps，lvgl 任务 100% 占 CPU，空闲任务被饿死）。
         * 这里先把 t 夹到 [1,50]ms，再换算并确保至少阻塞 1 个 tick。
         *
         * 任务通知改造: 用 ulTaskNotifyTake 替代 vTaskDelay。
         * DMA 完成中断会 xTaskNotifyGive 唤醒主循环, 不用傻等 10ms。
         * 静止时没 DMA 完成, 超时(delay_ticks)后自然醒来处理时钟/定时器。 */
        if (t == 0 || t > 50) t = 1;
        TickType_t delay_ticks = pdMS_TO_TICKS(t);
        if (delay_ticks < 1) delay_ticks = 1;
        ulTaskNotifyTake(pdTRUE, delay_ticks);  /* 等 DMA 完成信号或超时 */
    }
}

/* 启动 LVGL 任务：main 任务默认栈太小（LVGL 渲染需要较大栈），另起大栈任务 */
static void lvgl_run(void)
{
    xTaskCreate(lvgl_task, "lvgl", 8192, NULL, 5, &g_lvgl_task);
}

#endif /* USE_LVGL */

/* 统一入口：根据编译开关选择 LVGL UI 或原始帧缓冲测试 */
void ui_app_start(void)
{
#if USE_LVGL
    lvgl_run();
#else
    lcd_test();
#endif
}
