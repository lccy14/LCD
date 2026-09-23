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
#include "time_sync.h"
#include "esp_wifi.h"
#include "driver/ledc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_log.h"

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

/* 各屏幕对象（用于界面跳转） */
static lv_obj_t *g_home = NULL;
static lv_obj_t *g_wifi_screen = NULL;
static lv_obj_t *g_settings_screen = NULL;
static lv_obj_t *g_clock_screen = NULL;
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
static lv_obj_t *g_wx_city    = NULL;
static lv_obj_t *g_wx_temp    = NULL;
static lv_obj_t *g_wx_desc    = NULL;
static lv_obj_t *g_wx_extra   = NULL;
static lv_obj_t *g_wx_updated = NULL;

/* 主循环里用的状态：避免每帧重绘/重复请求 */
static uint32_t s_wx_shown_ver    = 0;          /* 已显示到界面上的天气版本号 */
static bool     s_wx_fetching     = false;      /* 上次界面显示的“请求中”状态 */
static uint32_t s_last_wx_req_ms  = 0;          /* 上次发天气请求的 tick(ms) */
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

/* LVGL 输入设备回调：读取 FT6336，把坐标喂给 LVGL */
static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
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
        lv_label_set_text(g_subtitle, "Scanning...");
    } else {
        lv_label_set_text(g_subtitle, "Wi-Fi Off");
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
            lv_label_set_text_fmt(g_subtitle, "Connected  %s", g_wifi_ip);
        } else if (st == WIFI_STA_CONNECTING) {
            lv_label_set_text_fmt(g_subtitle, "Connecting to %s...", g_wifi_ssid);
        } else if (st == WIFI_STA_FAILED) {
            lv_label_set_text(g_subtitle, "Connection failed");
        } else {
            lv_label_set_text_fmt(g_subtitle, "Found %d networks", g_ap_count);
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
        snprintf(status, sizeof(status), "Connected\nIP: %s", g_wifi_ip);
        btn = "Disconnect";
    } else if (st == WIFI_STA_CONNECTING && is_current) {
        snprintf(status, sizeof(status), "Connecting...");
        btn = "Cancel";
    } else if (st == WIFI_STA_FAILED && is_current) {
        snprintf(status, sizeof(status), "Failed - check password");
        btn = "Connect";
    } else {
        snprintf(status, sizeof(status), "Not connected");
        btn = "Connect";
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

/* 返回主界面 */
static void go_home_cb(lv_event_t *e)
{
    (void)e;
    lv_screen_load(g_home);
}

/* App 编号，用于懒加载各屏幕 */
typedef enum {
    APP_WIFI = 1,
    APP_SETTINGS,
    APP_CLOCK,
    APP_MUSIC,
    APP_GAME,
    APP_WEATHER
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
    make_status_bar(scr);

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
    lv_label_set_text(bkl, "< Back");
    lv_obj_add_event_cb(bk, go_home_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *t = lv_label_create(bar);
    lv_label_set_text(t, title);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_20, 0);
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
    lv_label_set_text(cap, "Brightness");
    lv_obj_set_style_text_font(cap, &lv_font_montserrat_20, 0);
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
    lv_obj_set_style_text_font(l, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(0x9AA0B5), 0);
    lv_obj_align(l, LV_ALIGN_CENTER, 0, 0);
}

/* 主界面上的一个 App 按钮（真正的 lv_button，放在最顶层，固定且可点） */
static lv_obj_t *make_app_button(lv_obj_t *parent, const char *glyph, const char *name,
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
    lv_obj_t *g = lv_label_create(icon);
    lv_label_set_text(g, glyph);
    lv_obj_set_style_text_color(g, lv_color_white(), 0);
    lv_obj_set_style_text_font(g, &lv_font_montserrat_20, 0);
    lv_obj_center(g);

    lv_obj_t *nm = lv_label_create(btn);
    lv_label_set_text(nm, name);
    lv_obj_set_style_text_color(nm, lv_color_white(), 0);
    lv_obj_set_style_text_font(nm, &lv_font_montserrat_16, 0);
    lv_obj_clear_flag(nm, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(nm, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(nm, LV_OBJ_FLAG_CLICKABLE);     // 名称本身不拦截，点击落到按钮上
    /* 按钮自身也确保不滚动、不显示滚动条 */
    lv_obj_set_scrollbar_mode(btn, LV_SCROLLBAR_MODE_OFF);

    lv_obj_add_event_cb(btn, open_screen_cb, LV_EVENT_CLICKED, (void *)(intptr_t)app_id);
    return btn;
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
    lv_obj_clear_flag(g_home, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(g_home, LV_SCROLLBAR_MODE_OFF);
    make_status_bar(g_home);

    struct { const char *glyph, *name; lv_color_t color; int id; } apps[6] = {
        { LV_SYMBOL_WIFI,    "Wi-Fi",    lv_color_hex(0x2E7DE0), APP_WIFI },
        { LV_SYMBOL_SETTINGS, "Settings", lv_color_hex(0x6B7280), APP_SETTINGS },
        { "C", "Clock",   lv_color_hex(0x9C5FFF), APP_CLOCK },
        { "M", "Music",   lv_color_hex(0xE0457B), APP_MUSIC },
        { "2", "Game",    lv_color_hex(0x2BB673), APP_GAME },
        { "W", "Weather", lv_color_hex(0xE08A2B), APP_WEATHER },
    };
    const int start_x = 20, start_y = 38;
    const int gap_x = 100, gap_y = 108;   // 3 列 × 2 行，铺满 320×240
    for (int i = 0; i < 6; i++) {
        lv_obj_t *btn = make_app_button(g_home, apps[i].glyph, apps[i].name,
                                        apps[i].color, apps[i].id);
        int col = i % 3;
        int row = i / 3;
        lv_obj_set_pos(btn, start_x + col * gap_x, start_y + row * gap_y);
    }
}

/* 构建 Wi-Fi App 屏幕（手机风格 WiFi 界面） */
static void build_wifi_screen(lv_obj_t *scr)
{
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101018), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(scr, 0, 0);
    lv_obj_set_style_border_opa(scr, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);   // 外层不滚动，滚动交给内部列表 g_net_cont
    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);
    make_status_bar(scr);

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
    lv_label_set_text(bkl, "< Back");
    lv_obj_add_event_cb(bk, go_home_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *htitle = lv_label_create(header);
    lv_label_set_text(htitle, "Wi-Fi");
    lv_obj_set_style_text_font(htitle, &lv_font_montserrat_24, 0);
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
    lv_label_set_text(g_subtitle, "Scanning...");
    lv_obj_set_style_text_font(g_subtitle, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(g_subtitle, lv_color_hex(0x9AA0B5), 0);
    lv_obj_align(g_subtitle, LV_ALIGN_TOP_LEFT, 14, 70);

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
    lv_label_set_text(back_lbl, "< Back");
    lv_obj_add_event_cb(back_btn, back_btn_cb, LV_EVENT_CLICKED, g_detail);
    lv_obj_t *dht = lv_label_create(dh);
    lv_label_set_text(dht, "Wi-Fi");
    lv_obj_set_style_text_font(dht, &lv_font_montserrat_24, 0);
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
    lv_obj_set_style_text_font(g_detail_status, &lv_font_montserrat_16, 0);
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
    lv_label_set_text(bl, "Connect");
    lv_obj_set_style_text_font(bl, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(bl, lv_color_white(), 0);
    lv_obj_center(bl);
    lv_obj_add_event_cb(g_detail_btn, connect_btn_cb, LV_EVENT_CLICKED, NULL);
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
    lv_obj_set_style_text_font(g_pwd_ssid, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(g_pwd_ssid, lv_color_hex(0x9AA0B5), 0);
    lv_obj_set_width(g_pwd_ssid, LCD_W - 32);
    lv_label_set_long_mode(g_pwd_ssid, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(g_pwd_ssid, 16, 6);

    g_pwd_ta = lv_textarea_create(g_pwd_panel);
    lv_obj_set_size(g_pwd_ta, 288, 34);
    lv_obj_set_pos(g_pwd_ta, 16, 26);
    lv_textarea_set_one_line(g_pwd_ta, true);
    lv_textarea_set_password_mode(g_pwd_ta, true);
    lv_textarea_set_placeholder_text(g_pwd_ta, "Password");
    lv_obj_set_style_text_font(g_pwd_ta, &lv_font_montserrat_20, 0);

    lv_obj_t *ok = lv_button_create(g_pwd_panel);
    lv_obj_set_size(ok, 92, 32);
    lv_obj_set_pos(ok, 16, 66);
    lv_obj_set_style_bg_color(ok, lv_color_hex(0x2E7DE0), 0);
    lv_obj_t *okl = lv_label_create(ok);
    lv_label_set_text(okl, "Join");
    lv_obj_center(okl);
    lv_obj_add_event_cb(ok, pwd_ok_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *ca = lv_button_create(g_pwd_panel);
    lv_obj_set_size(ca, 92, 32);
    lv_obj_set_pos(ca, 120, 66);
    lv_obj_t *cal = lv_label_create(ca);
    lv_label_set_text(cal, "Cancel");
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

static void build_clock_screen(lv_obj_t *parent)
{
    g_clock_time = lv_label_create(parent);
    lv_obj_set_style_text_font(g_clock_time, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(g_clock_time, lv_color_white(), 0);
    lv_obj_align(g_clock_time, LV_ALIGN_CENTER, 0, -28);
    lv_label_set_text(g_clock_time, "--:--:--");

    g_clock_date = lv_label_create(parent);
    lv_obj_set_style_text_font(g_clock_date, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(g_clock_date, lv_color_hex(0xAAAAAA), 0);
    lv_obj_align(g_clock_date, LV_ALIGN_CENTER, 0, 24);
    lv_label_set_text(g_clock_date, "Syncing time...");
}

/* ---------------- Weather 屏（英文显示，连上网自动拉取） ---------------- */

static void wx_refresh_cb(lv_event_t *e)
{
    (void)e;
    if (g_wifi_state == WIFI_STA_CONNECTED) {
        weather_request(NULL);   /* 仅唤醒后台任务，不发网络、不建对象 */
    }
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
    lv_obj_set_style_text_font(g_wx_temp, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(g_wx_temp, lv_color_white(), 0);
    lv_label_set_text(g_wx_temp, "--.- C");

    g_wx_desc = lv_label_create(cont);
    lv_obj_set_style_text_font(g_wx_desc, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(g_wx_desc, lv_color_hex(0xCCCCCC), 0);
    lv_label_set_text(g_wx_desc, "...");

    g_wx_extra = lv_label_create(cont);
    lv_obj_set_style_text_font(g_wx_extra, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(g_wx_extra, lv_color_hex(0xAAAAAA), 0);
    lv_label_set_text(g_wx_extra, "Humidity --%   Wind -- km/h");

    g_wx_updated = lv_label_create(cont);
    lv_obj_set_style_text_font(g_wx_updated, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(g_wx_updated, lv_color_hex(0x888888), 0);
    lv_label_set_text(g_wx_updated, "Not fetched yet");

    /* 底部刷新按钮（独立于内容容器，固定在屏幕底部） */
    lv_obj_t *rf = lv_button_create(parent);
    lv_obj_set_size(rf, 140, 34);
    lv_obj_align(rf, LV_ALIGN_BOTTOM_MID, 0, -14);
    lv_obj_set_style_bg_color(rf, lv_color_hex(0x2E7DE0), 0);
    lv_obj_t *rl = lv_label_create(rf);
    lv_label_set_text(rl, "Refresh");
    lv_obj_center(rl);
    lv_obj_add_event_cb(rf, wx_refresh_cb, LV_EVENT_CLICKED, NULL);
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
    if (g_game_score_label) lv_label_set_text_fmt(g_game_score_label, "Score: %u", (unsigned)g_game_score);
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
    lv_label_set_text(g_game_score_label, "Score: 0");
    lv_obj_set_style_text_font(g_game_score_label, &lv_font_montserrat_16, 0);
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
    lvgl_home_setup();                      /* 主界面 + 触摸红点 */
}

/* 连上 WiFi 后自动拉天气，并每 10 分钟周期刷新 */
static void ui_weather_periodic(void)
{
    uint32_t now = lv_tick_get();
    if (g_wifi_state == WIFI_STA_CONNECTED && s_prev_wifi_state != WIFI_STA_CONNECTED) {
        weather_request(NULL);            /* 刚连上，拉一次 */
        s_last_wx_req_ms = now;
    } else if (g_wifi_state == WIFI_STA_CONNECTED && now - s_last_wx_req_ms >= 10 * 60 * 1000) {
        weather_request(NULL);            /* 周期刷新 */
        s_last_wx_req_ms = now;
    }
    s_prev_wifi_state = g_wifi_state;
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
        char buf[32];
        strftime(buf, sizeof(buf), "%H:%M:%S", &tm);
        lv_label_set_text(g_clock_time, buf);
        strftime(buf, sizeof(buf), "%a %b %d %Y", &tm);
        lv_label_set_text(g_clock_date, buf);
    } else {
        lv_label_set_text(g_clock_time, "--:--:--");
        lv_label_set_text(g_clock_date, "Syncing time...");
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
                   (g_weather.fetching != s_wx_fetching);
    if (!changed) return;

    s_wx_shown_ver = g_weather.version;
    s_wx_fetching  = g_weather.fetching;
    char buf[48];
    if (g_weather.fetching) {
        lv_label_set_text(g_wx_updated, "Updating...");
    } else if (g_weather.valid) {
        lv_label_set_text(g_wx_city, g_weather.city);
        snprintf(buf, sizeof(buf), "%.1f C", g_weather.temp);
        lv_label_set_text(g_wx_temp, buf);
        lv_label_set_text(g_wx_desc, g_weather.desc);
        snprintf(buf, sizeof(buf), "Humidity %d%%   Wind %.1f km/h",
                 g_weather.humidity, g_weather.wind);
        lv_label_set_text(g_wx_extra, buf);
        int ago_s = (int)((esp_timer_get_time() / 1000 - g_weather.updated_ms) / 1000);
        if (ago_s < 0) ago_s = 0;
        snprintf(buf, sizeof(buf), "Updated %ds ago", ago_s);
        lv_label_set_text(g_wx_updated, buf);
    } else if (g_weather.error) {
        lv_label_set_text(g_wx_desc, "Fetch failed");
        lv_label_set_text(g_wx_updated, "Check network");
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
    lv_label_set_text_fmt(g_pwd_ssid, "Password for %s", g_pending_ssid);
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
            g_settings_screen = make_app_screen("Settings");
            build_settings_screen(g_settings_screen);
        }
        scr = g_settings_screen;
        break;
    case APP_CLOCK:
        if (!g_clock_screen) {
            g_clock_screen = make_app_screen("Clock");
            build_clock_screen(g_clock_screen);
        }
        scr = g_clock_screen;
        break;
    case APP_MUSIC:
        if (!g_music_screen) { g_music_screen = make_app_screen("Music"); add_placeholder(g_music_screen, "Music\n(not implemented)"); }
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
            g_weather_screen = make_app_screen("Weather");
            build_weather_screen(g_weather_screen);
        }
        scr = g_weather_screen;
        /* 打开天气屏就拉一次（若已联网）；重复打开最多排队一次请求 */
        if (g_wifi_state == WIFI_STA_CONNECTED) {
            weather_request(NULL);
        }
        break;
    default:
        break;
    }
    if (scr) {
        if (id == APP_WIFI && g_wifi_on) {
            wifi_scan_trigger();   // 进入 WiFi 才扫描
        }
        lv_screen_load(scr);
    }
}

/* 触摸红点：按下时显示并跟随手指坐标，松开时隐藏 */
static void ui_refresh_touch_dot(void)
{
    if (g_touch_pressed) {
        lv_obj_set_pos(g_red_dot, g_touch_x - 12, g_touch_y - 12);
        lv_obj_clear_flag(g_red_dot, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(g_red_dot, LV_OBJ_FLAG_HIDDEN);
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
            lv_label_set_text_fmt(g_subtitle, "Found %d networks", g_ap_count);
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

        /* 各屏数据周期性刷新：天气自动拉取、时钟、状态栏时钟、天气屏 */
        ui_weather_periodic();
        ui_clock_refresh();
        ui_status_clocks_refresh();
        ui_weather_screen_refresh();

        /* 延迟任务：密码面板、App 跳转、触摸红点、WiFi 列表刷新 */
        ui_handle_pending_password();
        ui_handle_pending_app();
        ui_refresh_touch_dot();
        ui_refresh_wifi_list();

        /* 处理 LVGL 任务，返回建议休眠 ms */
        uint32_t t = lv_timer_handler();
        vTaskDelay(pdMS_TO_TICKS(t > 0 ? t : 1));
    }
}

/* 启动 LVGL 任务：main 任务默认栈太小（LVGL 渲染需要较大栈），另起大栈任务 */
static void lvgl_run(void)
{
    xTaskCreate(lvgl_task, "lvgl", 8192, NULL, 5, NULL);
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
