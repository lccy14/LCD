#include "snake.h"
#include "snake_sprites.h"   /* CC0 Snake.png by eugeneloza, 自动转换 */
#include "ui.h"              /* ui_set_status_bar_visible 进入游戏隐藏状态栏 */
#include "lcd.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>
#include <stdint.h>          /* uintptr_t: 方向枚举塞进事件的 user_data */

/* ===================== 贪吃蛇 =====================
 * 左侧游戏区 240×208 (网格 15×13, 每格 16px), 右侧 80px 放方向键
 * 顶部 32px 给 Back + 分数
 * Sprite 素材: Snake.png (CC0, eugeneloza github.com/eugeneloza/SnakeGame)
 *   - 蛇头 4 方向: idx 0=UP 1=RIGHT 2=DOWN 3=LEFT
 *   - 蛇身水平: idx 4
 *   - 食物: idx 14
 * 方向键控制: 按下即刻转向并马上走一步(不等下一个定时 tick), 比滑动灵敏
 * 吃食物 +10 分, 撞墙/撞自己 = 死亡
 */

#define TAG "SNAKE"

#define SNK_CELL_W        16
#define SNK_CELL_H        16
#define SNK_GRID_X0       0
#define SNK_GRID_Y0       32       /* 顶部 UI 32px */
#define SNK_COLS          15        /* 240/16: 右边留 80px 给方向键 */
#define SNK_ROWS          13        /* (240-32)/16 = 13 */
#define SNK_MAX_LEN       (SNK_COLS * SNK_ROWS)

/* 右侧方向键区: 紧凑十字布局（上下键宽、左右键窄，x: 240~319） */
#define SNK_PAD_X0        (SNK_COLS * SNK_CELL_W)   /* 240 = 游戏区右边界 */
#define SNK_BTN_H         48
#define SNK_BTN_GAP       4
#define SNK_BTN_W         72        /* 上下键宽 */
#define SNK_BTN_SW        34        /* 左右键宽 */
#define SNK_PAD_X         (SNK_PAD_X0 + 4)                    /* 244 */
#define SNK_PAD_TOP       (SNK_GRID_Y0 + 28)                  /* 60: 竖直居中 */

#define SPRITE_W          SNAKE_SPRITE_PX   /* 16 */
#define SPRITE_H          SNAKE_SPRITE_PX

/* Sprite 索引 (对应 snake_sprites.h 中的 snake_sprite_data[N]) */
#define SP_HEAD_UP        0
#define SP_HEAD_RIGHT     1
#define SP_HEAD_DOWN      2
#define SP_HEAD_LEFT      3
#define SP_BODY_HZ        4
#define SP_FOOD           14

/* 4 个方向的蛇头 sprite (引用 snake_sprite_data 中的常量数据) */
#define HEAD_DSC(N) { \
    .header = { .magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_ARGB8888, \
                .flags = 0, .w = SPRITE_W, .h = SPRITE_H, .stride = SPRITE_W*4, .reserved_2 = 0 }, \
    .data_size = SPRITE_W * SPRITE_H * 4, \
    .data = (const uint8_t *)snake_sprite_data[N], \
}

static lv_image_dsc_t head_sprites[4] = {
    HEAD_DSC(SP_HEAD_UP), HEAD_DSC(SP_HEAD_RIGHT),
    HEAD_DSC(SP_HEAD_DOWN), HEAD_DSC(SP_HEAD_LEFT),
};
static lv_image_dsc_t body_sprite = HEAD_DSC(SP_BODY_HZ);
static lv_image_dsc_t food_sprite = HEAD_DSC(SP_FOOD);

/* 游戏状态 ------------------------------------------------------------- */
typedef enum { DIR_UP, DIR_DOWN, DIR_LEFT, DIR_RIGHT } snake_dir_t;

typedef struct {
    int x, y;       /* 网格坐标 */
} snake_pt_t;

static lv_obj_t *g_snake_screen = NULL;
static lv_obj_t *g_score_label  = NULL;
static lv_obj_t *g_tip_label   = NULL;
static lv_obj_t *g_body_objs[SNK_MAX_LEN];  /* 蛇身 obj 数组 */
static int       g_snake_len = 0;
static snake_pt_t g_snake[SNK_MAX_LEN];
static snake_pt_t g_food;
static snake_dir_t g_dir = DIR_RIGHT;
static snake_dir_t g_pending_dir = DIR_RIGHT;
static int g_score = 0;
static bool g_active = false;
static bool g_game_over = false;
static uint32_t g_last_step_ms = 0;   /* 上一次真正走一步的时刻 */

#define STEP_INTERVAL_MS  300    /* 没按键时, 每 300ms 自动走一步 */
#define BTN_MIN_STEP_MS   110    /* 连点保护: 两次「按键立即走一步」的最小间隔。
                                  * 防手抖/连点让蛇瞬间窜出去; 仍远快于自动步进 */

/* 工具 */
static int cell_x(int col) { return SNK_GRID_X0 + col * SNK_CELL_W; }
static int cell_y(int row) { return SNK_GRID_Y0 + row * SNK_CELL_H; }

static void spawn_food(void) {
    bool ok = false;
    while (!ok) {
        g_food.x = rand() % SNK_COLS;
        g_food.y = rand() % SNK_ROWS;
        ok = true;
        for (int i = 0; i < g_snake_len; i++) {
            if (g_snake[i].x == g_food.x && g_snake[i].y == g_food.y) {
                ok = false; break;
            }
        }
    }
}

/* snake_dir_t 到 head_sprites[] 索引映射 (枚举顺序与 sprite 顺序不同) */
static int head_sprite_idx(snake_dir_t d) {
    switch (d) {
        case DIR_UP:    return SP_HEAD_UP;
        case DIR_DOWN:  return SP_HEAD_DOWN;
        case DIR_LEFT:  return SP_HEAD_LEFT;
        case DIR_RIGHT: return SP_HEAD_RIGHT;
    }
    return SP_HEAD_RIGHT;
}

static void draw_snake(void) {
    /* 重建所有蛇身 obj (简单粗暴, 蛇不长, 性能可接受) */
    const lv_image_dsc_t *head = &head_sprites[head_sprite_idx(g_dir)];
    for (int i = 0; i < g_snake_len; i++) {
        if (!g_body_objs[i]) {
            g_body_objs[i] = lv_image_create(g_snake_screen);
            lv_obj_clear_flag(g_body_objs[i], LV_OBJ_FLAG_SCROLLABLE);
        }
        lv_image_set_src(g_body_objs[i], (i == 0) ? head : &body_sprite);
        lv_obj_set_pos(g_body_objs[i], cell_x(g_snake[i].x), cell_y(g_snake[i].y));
    }
    /* 多余的 obj (蛇变短时) 删除 */
    for (int i = g_snake_len; i < SNK_MAX_LEN; i++) {
        if (g_body_objs[i]) {
            lv_obj_del(g_body_objs[i]);
            g_body_objs[i] = NULL;
        }
    }
}

static lv_obj_t *g_food_obj = NULL;
static void draw_food(void) {
    if (!g_food_obj) {
        g_food_obj = lv_image_create(g_snake_screen);
        lv_obj_clear_flag(g_food_obj, LV_OBJ_FLAG_SCROLLABLE);
    }
    lv_image_set_src(g_food_obj, &food_sprite);
    lv_obj_set_pos(g_food_obj, cell_x(g_food.x), cell_y(g_food.y));
}

static void show_game_over(void) {
    g_game_over = true;
    lv_obj_t *bg = lv_obj_create(g_snake_screen);
    lv_obj_set_size(bg, 200, 100);
    /* 居中于左侧游戏区 (240×208, y 从 32 起), 而不是整个屏幕, 免得压住方向键 */
    lv_obj_align(bg, LV_ALIGN_TOP_LEFT, 20, SNK_GRID_Y0 + 54);
    lv_obj_set_style_bg_color(bg, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(bg, LV_OPA_90, 0);
    lv_obj_set_style_radius(bg, 10, 0);
    lv_obj_clear_flag(bg, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *lbl = lv_label_create(bg);
    lv_label_set_text_fmt(lbl, "GAME OVER\nScore: %d", g_score);
    lv_obj_set_style_text_color(bg, lv_color_hex(0xF44336), 0);
    lv_obj_center(lbl);
}

/* ---------------- 方向键控制 ----------------
 * 原来的滑动方案有两个毛病: ① 要抬起手指才判定方向, 有延迟;
 * ② 改完方向还得等下一个 300ms tick 才动, 按下去像"没反应"。
 * 现在改成: 手指一按下就转向, 并且立刻走一步。 */

static void snake_step(void);   /* 定义在下面, 这里先声明给 try_step 用 */

/* 走一步的统一入口: 按键和定时 tick 共用同一个时间戳,
 * 避免「按键刚走一步、定时器紧接着又走一步」忽快忽慢。
 * min_gap = 距上一步至少要间隔多少毫秒, 不满足就只记方向不动 */
static void try_step(uint32_t min_gap)
{
    if (!g_active || g_game_over) return;
    uint32_t now = lv_tick_get();
    if (now - g_last_step_ms < min_gap) return;
    g_last_step_ms = now;
    snake_step();
}

/* 方向键回调。用 PRESSED 而不是 CLICKED —— 手指一碰就响应, 不必等抬起。
 * user_data 里直接塞方向枚举本身(值 0~3, 不是指针) */
static void dpad_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_PRESSED) return;
    if (!g_active || g_game_over) return;

    snake_dir_t d = (snake_dir_t)(uintptr_t)lv_event_get_user_data(e);

    /* 禁止 180° 掉头: 脖子就在身后那格, 转过去必死 */
    if ((d == DIR_UP    && g_dir == DIR_DOWN)  ||
        (d == DIR_DOWN  && g_dir == DIR_UP)    ||
        (d == DIR_LEFT  && g_dir == DIR_RIGHT) ||
        (d == DIR_RIGHT && g_dir == DIR_LEFT)) {
        return;
    }

    /* 方向立即生效, 而不是塞进 g_pending_dir 等下一个 tick:
     * 连按时掉头判断要用蛇的真实朝向, 也才有"跟手"的感觉 */
    g_dir = d;
    g_pending_dir = d;
    try_step(BTN_MIN_STEP_MS);
}

/* 画一个方向键 */
static void dpad_key(int x, int y, int w, int h, const char *sym, snake_dir_t d)
{
    lv_obj_t *b = lv_button_create(g_snake_screen);
    lv_obj_set_size(b, w, h);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x2A2A44), 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x4CAF50), LV_STATE_PRESSED);  /* 按下高亮 */
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(b, 8, 0);
    lv_obj_set_style_border_width(b, 0, 0);
    lv_obj_set_style_pad_all(b, 0, 0);
    lv_obj_set_style_shadow_opa(b, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, sym);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_24, 0);   /* 箭头放大, 按着有准头 */
    lv_obj_set_style_text_color(l, lv_color_hex(0xE8DEF8), 0);
    lv_obj_center(l);

    lv_obj_add_event_cb(b, dpad_cb, LV_EVENT_PRESSED, (void *)(uintptr_t)d);
}

/* 右侧方向键, 十字排布: 上下两个长条 + 中间一对左右键 */
static void build_dpad(void)
{
    const int y_lr   = SNK_PAD_TOP + (SNK_BTN_H + SNK_BTN_GAP);
    const int y_down = SNK_PAD_TOP + (SNK_BTN_H + SNK_BTN_GAP) * 2;
    const int x_r    = SNK_PAD_X + SNK_BTN_SW + SNK_BTN_GAP;

    dpad_key(SNK_PAD_X, SNK_PAD_TOP, SNK_BTN_W,  SNK_BTN_H, LV_SYMBOL_UP,    DIR_UP);
    dpad_key(SNK_PAD_X, y_lr,        SNK_BTN_SW, SNK_BTN_H, LV_SYMBOL_LEFT,  DIR_LEFT);
    dpad_key(x_r,       y_lr,        SNK_BTN_SW, SNK_BTN_H, LV_SYMBOL_RIGHT, DIR_RIGHT);
    dpad_key(SNK_PAD_X, y_down,      SNK_BTN_W,  SNK_BTN_H, LV_SYMBOL_DOWN,  DIR_DOWN);
}

static void snake_back_cb(lv_event_t *e) {
    (void)e;
    /* 清理 */
    for (int i = 0; i < SNK_MAX_LEN; i++) {
        if (g_body_objs[i]) { lv_obj_del(g_body_objs[i]); g_body_objs[i] = NULL; }
    }
    if (g_food_obj) { lv_obj_del(g_food_obj); g_food_obj = NULL; }
    g_active = false;
    ui_set_status_bar_visible(true);   /* 退出游戏, 恢复系统状态栏 */
    extern void ui_go_home(void);
    ui_go_home();
}

/* 游戏初始化 */
static void snake_init(void) {
    /* 清理旧 obj */
    for (int i = 0; i < SNK_MAX_LEN; i++) {
        if (g_body_objs[i]) { lv_obj_del(g_body_objs[i]); g_body_objs[i] = NULL; }
    }
    if (g_food_obj) { lv_obj_del(g_food_obj); g_food_obj = NULL; }

    /* 初始蛇: 3 节, 中间位置, 向右 */
    g_snake_len = 3;
    int start_x = SNK_COLS / 2;
    int start_y = SNK_ROWS / 2;
    g_snake[0].x = start_x;     g_snake[0].y = start_y;
    g_snake[1].x = start_x - 1; g_snake[1].y = start_y;
    g_snake[2].x = start_x - 2; g_snake[2].y = start_y;
    g_dir = DIR_RIGHT;
    g_pending_dir = DIR_RIGHT;
    g_score = 0;
    g_game_over = false;
    g_last_step_ms = lv_tick_get();   /* 从现在开始计时, 第一步走 STEP_INTERVAL_MS 后 */

    spawn_food();
    draw_food();
    draw_snake();

    if (g_score_label) lv_label_set_text_fmt(g_score_label, "Score: 0");
    if (g_tip_label) lv_label_set_text(g_tip_label, "Tap arrows");
}

/* 走一步 */
static void snake_step(void) {
    g_dir = g_pending_dir;
    snake_pt_t new_head = g_snake[0];
    switch (g_dir) {
        case DIR_UP:    new_head.y--; break;
        case DIR_DOWN:  new_head.y++; break;
        case DIR_LEFT:  new_head.x--; break;
        case DIR_RIGHT: new_head.x++; break;
    }
    /* 撞墙 */
    if (new_head.x < 0 || new_head.x >= SNK_COLS ||
        new_head.y < 0 || new_head.y >= SNK_ROWS) {
        show_game_over();
        return;
    }
    /* 撞自己 (除尾巴, 尾巴会移走) */
    for (int i = 0; i < g_snake_len - 1; i++) {
        if (g_snake[i].x == new_head.x && g_snake[i].y == new_head.y) {
            show_game_over();
            return;
        }
    }
    /* 移动: 后面节点复制前面位置 */
    int ate_food = (new_head.x == g_food.x && new_head.y == g_food.y);
    if (ate_food) {
        g_snake_len++;
        g_score += 10;
        if (g_score_label)
            lv_label_set_text_fmt(g_score_label, "Score: %d", g_score);
    }
    /* 从尾到头依次复制 (如果吃了食物, 多一个位置) */
    for (int i = g_snake_len - 1; i > 0; i--) {
        g_snake[i] = g_snake[i - 1];
    }
    g_snake[0] = new_head;

    if (ate_food) spawn_food();
    draw_food();
    draw_snake();
}

void snake_update(void) {
    if (!g_active || g_game_over) return;
    /* 没按方向键时保持匀速自动行走 */
    uint32_t now = lv_tick_get();
    if (now - g_last_step_ms < STEP_INTERVAL_MS) return;
    g_last_step_ms = now;
    snake_step();
}

void snake_create_screen(void) {
    /* sprite 数据由 snake_sprites.c 编译期提供 (来自 Snake.png) */
    ui_set_status_bar_visible(false);   /* 进入游戏, 隐藏系统状态栏让游戏占满屏 */

    g_snake_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(g_snake_screen, lv_color_hex(0x0B0B12), 0);
    lv_obj_set_style_bg_opa(g_snake_screen, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(g_snake_screen, 0, 0);
    lv_obj_set_style_border_opa(g_snake_screen, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(g_snake_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(g_snake_screen, LV_SCROLLBAR_MODE_OFF);

    /* 网格背景: 浅色格子 */
    for (int r = 0; r < SNK_ROWS; r++) {
        for (int c = 0; c < SNK_COLS; c++) {
            lv_obj_t *cell = lv_obj_create(g_snake_screen);
            lv_obj_set_size(cell, SNK_CELL_W - 1, SNK_CELL_H - 1);
            lv_obj_set_pos(cell, cell_x(c), cell_y(r));
            lv_obj_set_style_bg_color(cell,
                ((r + c) & 1) ? lv_color_hex(0x1A1A2E) : lv_color_hex(0x0F0F1E), 0);
            lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(cell, 0, 0);
            lv_obj_set_style_border_width(cell, 0, 0);
            lv_obj_set_style_pad_all(cell, 0, 0);
            lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_clear_flag(cell, LV_OBJ_FLAG_CLICKABLE);
        }
    }

    /* 顶部 UI (y=0-32) */
    lv_obj_t *back_btn = lv_button_create(g_snake_screen);
    lv_obj_set_size(back_btn, 72, 28);
    lv_obj_set_pos(back_btn, 4, 2);
    lv_obj_set_style_bg_color(back_btn, lv_color_hex(0x607D8B), 0);
    lv_obj_set_style_bg_opa(back_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(back_btn, 6, 0);
    lv_obj_set_style_pad_hor(back_btn, 6, 0);
    lv_obj_set_style_pad_ver(back_btn, 2, 0);
    lv_obj_set_style_border_width(back_btn, 0, 0);
    lv_obj_t *back_lbl = lv_label_create(back_btn);
    lv_label_set_text(back_lbl, LV_SYMBOL_LEFT " Back");
    lv_obj_center(back_lbl);
    lv_obj_add_event_cb(back_btn, snake_back_cb, LV_EVENT_CLICKED, NULL);

    g_score_label = lv_label_create(g_snake_screen);
    lv_obj_set_pos(g_score_label, 100, 10);
    lv_label_set_text(g_score_label, "Score: 0");
    lv_obj_set_style_text_color(g_score_label, lv_color_hex(0xFFEB3B), 0);

    g_tip_label = lv_label_create(g_snake_screen);
    lv_obj_set_pos(g_tip_label, 200, 10);
    lv_label_set_text(g_tip_label, "Tap arrows");
    lv_obj_set_style_text_color(g_tip_label, lv_color_hex(0xCCCCCC), 0);

    build_dpad();

    snake_init();
    lv_screen_load(g_snake_screen);
    g_active = true;
    ESP_LOGI(TAG, "snake screen created");
}

bool snake_is_active(void) {
    return g_active;
}
