#include "snake.h"
#include "snake_sprites.h"   /* CC0 Snake.png by eugeneloza, 自动转换 */
#include "ui.h"              /* ui_set_status_bar_visible 进入游戏隐藏状态栏 */
#include "lcd.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>

/* ===================== 贪吃蛇 =====================
 * 网格 20×13 (每格 16px), 留 32px 给顶部 UI
 * Sprite 素材: Snake.png (CC0, eugeneloza github.com/eugeneloza/SnakeGame)
 *   - 蛇头 4 方向: idx 0=UP 1=RIGHT 2=DOWN 3=LEFT
 *   - 蛇身水平: idx 4
 *   - 食物: idx 14
 * 滑动控制方向, 吃食物 +10 分, 撞墙/撞自己 = 死亡
 */

#define TAG "SNAKE"

#define SNK_CELL_W        16
#define SNK_CELL_H        16
#define SNK_GRID_X0       0
#define SNK_GRID_Y0       32       /* 顶部 UI 32px */
#define SNK_COLS          20        /* 320/16 */
#define SNK_ROWS          13        /* (240-32)/16 = 13 */
#define SNK_MAX_LEN       (SNK_COLS * SNK_ROWS)

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
static int g_step_timer = 0;
static int g_score = 0;
static bool g_active = false;
static bool g_game_over = false;
static uint32_t g_last_ms = 0;

/* 滑动检测: 记录按下点, 抬起时根据偏移判断方向 */
static lv_point_t g_touch_start = {0, 0};
static bool g_touch_down = false;

#define STEP_INTERVAL_MS  300    /* 每 300ms 走一步 */

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
    lv_obj_center(bg);
    lv_obj_set_style_bg_color(bg, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(bg, LV_OPA_90, 0);
    lv_obj_set_style_radius(bg, 10, 0);
    lv_obj_clear_flag(bg, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *lbl = lv_label_create(bg);
    lv_label_set_text_fmt(lbl, "GAME OVER\nScore: %d", g_score);
    lv_obj_set_style_text_color(bg, lv_color_hex(0xF44336), 0);
    lv_obj_center(lbl);
}

/* 方向控制: 滑动检测 */
static void snake_screen_event_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev = lv_indev_get_act();
    lv_point_t p;
    lv_indev_get_point(indev, &p);

    if (code == LV_EVENT_PRESSED) {
        g_touch_start = p;
        g_touch_down = true;
    } else if (code == LV_EVENT_RELEASED && g_touch_down) {
        g_touch_down = false;
        int dx = p.x - g_touch_start.x;
        int dy = p.y - g_touch_start.y;
        if (dx*dx + dy*dy < 100) return;  /* 太短忽略 */
        if (abs(dx) > abs(dy)) {
            /* 水平 */
            if (dx > 0 && g_dir != DIR_LEFT)  g_pending_dir = DIR_RIGHT;
            else if (dx < 0 && g_dir != DIR_RIGHT) g_pending_dir = DIR_LEFT;
        } else {
            /* 垂直 */
            if (dy > 0 && g_dir != DIR_UP)    g_pending_dir = DIR_DOWN;
            else if (dy < 0 && g_dir != DIR_DOWN) g_pending_dir = DIR_UP;
        }
    }
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
    g_step_timer = 0;
    g_game_over = false;
    g_last_ms = 0;

    spawn_food();
    draw_food();
    draw_snake();

    if (g_score_label) lv_label_set_text_fmt(g_score_label, "Score: 0");
    if (g_tip_label) lv_label_set_text(g_tip_label, "Swipe to move");
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
    uint32_t now = lv_tick_get();
    if (g_last_ms == 0) g_last_ms = now;
    int dt = (int)(now - g_last_ms);
    if (dt < STEP_INTERVAL_MS) return;
    g_last_ms = now;
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
    lv_label_set_text(g_tip_label, "Swipe to move");
    lv_obj_set_style_text_color(g_tip_label, lv_color_hex(0xCCCCCC), 0);

    /* 整屏事件: 滑动检测 */
    lv_obj_add_event_cb(g_snake_screen, snake_screen_event_cb,
                        LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(g_snake_screen, snake_screen_event_cb,
                        LV_EVENT_RELEASED, NULL);

    snake_init();
    lv_screen_load(g_snake_screen);
    g_active = true;
    ESP_LOGI(TAG, "snake screen created");
}

bool snake_is_active(void) {
    return g_active;
}
