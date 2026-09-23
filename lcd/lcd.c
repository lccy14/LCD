#include "lcd.h"
#include "lcd_init.h"
#include "lcdfont.h"
#include "delay.h"
#include "esp_attr.h"   // EXT_RAM_BSS_ATTR

/******************************************************************************
      帧缓冲（所有绘图操作只改内存，统一 flush 到屏幕）
      RGB565 两个字节，每像素 2 字节 [高字节, 低字节]——并口顺序直接对应。
      framebuffer[y * LCD_W + x] 对应坐标 (x, y)。
      EXT_RAM_BSS_ATTR 放 PSRAM，约 150KB，LCD_CAM DMA 可直接访问。
******************************************************************************/
EXT_RAM_BSS_ATTR u8 g_framebuffer[LCD_W * LCD_H * 2];

/******************************************************************************
      局部刷新 [x1,x2] x [y1,y2]，走 LCD_CAM 硬件 DMA，单次 draw_bitmap。
******************************************************************************/
void LCD_Flush(u16 x1, u16 y1, u16 x2, u16 y2)
{
    if (x1 > x2) { u16 t = x1; x1 = x2; x2 = t; }
    if (y1 > y2) { u16 t = y1; y1 = y2; y2 = t; }
    if (x1 >= LCD_W || y1 >= LCD_H) return;
    if (x2 >= LCD_W) x2 = LCD_W - 1;
    if (y2 >= LCD_H) y2 = LCD_H - 1;

    u16 w = x2 - x1 + 1;
    u16 h = y2 - y1 + 1;
    const u8 *buf = &g_framebuffer[((u32)y1 * LCD_W + x1) * 2];
    esp_lcd_panel_draw_bitmap(g_lcd_panel, x1, y1, x1 + w, y1 + h, buf);
}

void LCD_Flush_All(void)
{
    /* esp_lcd_panel_draw_bitmap 内部一次搞定：
     *   0x2A 行坐标 + 0x2B 列坐标 + 0x2C 写数据 + 整块 DMA 传输 */
    esp_lcd_panel_draw_bitmap(g_lcd_panel, 0, 0, LCD_W, LCD_H, g_framebuffer);
}

/******************************************************************************
      像素写帧缓冲（封装）
******************************************************************************/
static inline void fb_set_pixel(u16 x, u16 y, u16 color)
{
    if (x >= LCD_W || y >= LCD_H) return;
    u32 idx = ((u32)y * LCD_W + x) * 2;
    g_framebuffer[idx]     = (u8)(color >> 8);
    g_framebuffer[idx + 1] = (u8)(color & 0xFF);
}

/******************************************************************************
      指定区域填充颜色（只写帧缓冲，不立即刷新）
      区域为 [xsta, xend-1] x [ysta, yend-1]
******************************************************************************/
void LCD_Fill(u16 xsta, u16 ysta, u16 xend, u16 yend, u16 color)
{
    u16 xs = xsta, ys = ysta;
    u16 xe = (xend > 0) ? xend - 1 : 0;
    u16 ye = (yend > 0) ? yend - 1 : 0;
    if (xe < xs) { u16 t = xs; xs = xe; xe = t; }
    if (ye < ys) { u16 t = ys; ys = ye; ye = t; }
    if (xs >= LCD_W || ys >= LCD_H) return;
    if (xe >= LCD_W) xe = LCD_W - 1;
    if (ye >= LCD_H) ye = LCD_H - 1;

    u8 hi = (u8)(color >> 8);
    u8 lo = (u8)(color & 0xFF);
    for (u16 y = ys; y <= ye; y++) {
        u32 idx = ((u32)y * LCD_W + xs) * 2;
        for (u16 x = xs; x <= xe; x++) {
            g_framebuffer[idx]     = hi;
            g_framebuffer[idx + 1] = lo;
            idx += 2;
        }
    }
}

/******************************************************************************
      在指定位置画一个点（写帧缓冲）
******************************************************************************/
void LCD_DrawPoint(u16 x, u16 y, u16 color)
{
    fb_set_pixel(x, y, color);
}

/******************************************************************************
      直线：Bresenham 算法
******************************************************************************/
void LCD_DrawLine(u16 x1, u16 y1, u16 x2, u16 y2, u16 color)
{
    int dx = (int)x2 - x1, dy = (int)y2 - y1;
    int incx = (dx > 0) ? 1 : (dx < 0 ? -1 : 0);
    int incy = (dy > 0) ? 1 : (dy < 0 ? -1 : 0);
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    int distance = (dx > dy) ? dx : dy;
    int xerr = 0, yerr = 0;
    int uRow = x1, uCol = y1;

    for (u16 t = 0; t <= (u16)distance; t++) {
        LCD_DrawPoint((u16)uRow, (u16)uCol, color);
        xerr += dx;
        yerr += dy;
        if (xerr > distance) { xerr -= distance; uRow += incx; }
        if (yerr > distance) { yerr -= distance; uCol += incy; }
    }
}

/******************************************************************************
      画矩形边框
******************************************************************************/
void LCD_DrawRectangle(u16 x1, u16 y1, u16 x2, u16 y2, u16 color)
{
    LCD_DrawLine(x1, y1, x2, y1, color);
    LCD_DrawLine(x1, y1, x1, y2, color);
    LCD_DrawLine(x1, y2, x2, y2, color);
    LCD_DrawLine(x2, y1, x2, y2, color);
}

/******************************************************************************
      中点圆算法（只写帧缓冲，支持 8 方向对称）
******************************************************************************/
void Draw_Circle(u16 x0, u16 y0, u8 r, u16 color)
{
    int d = 3 - 2 * (int)r;
    int x = 0, y = (int)r;

    while (x <= y) {
        // 8 对称点
        fb_set_pixel((u16)(x0 + x), (u16)(y0 + y), color);
        fb_set_pixel((u16)(x0 - x), (u16)(y0 + y), color);
        fb_set_pixel((u16)(x0 + x), (u16)(y0 - y), color);
        fb_set_pixel((u16)(x0 - x), (u16)(y0 - y), color);
        fb_set_pixel((u16)(x0 + y), (u16)(y0 + x), color);
        fb_set_pixel((u16)(x0 - y), (u16)(y0 + x), color);
        fb_set_pixel((u16)(x0 + y), (u16)(y0 - x), color);
        fb_set_pixel((u16)(x0 - y), (u16)(y0 - x), color);

        if (d < 0) d += 4 * x + 6;
        else       { d += 4 * (x - y) + 10; y--; }
        x++;
    }
}

/******************************************************************************
      显示一个字符
      mode: 0 不透明（前景+背景）  1 透明（只前景，背景保留）
      点阵按字节存储，每行 (sizex+7)/8 字节，bit0 对应列最小
******************************************************************************/
void LCD_ShowChar(u16 x, u16 y, u8 num, u16 fc, u16 bc, u8 sizey, u8 mode)
{
    u8 sizex;
    u16 row;
    const u8 *font;
    sizex = sizey / 2;
    num = num - ' ';

    if (sizey == 12)      font = ascii_1206[num];
    else if (sizey == 16) font = ascii_1608[num];
    else if (sizey == 24) font = ascii_2412[num];
    else if (sizey == 32) font = ascii_3216[num];
    else return;

    u8 bytes_per_row = (sizex + 7) / 8;
    for (row = 0; row < sizey; row++) {
        const u8 *line = &font[row * bytes_per_row];
        for (u16 col = 0; col < sizex; col++) {
            u8 byte = line[col / 8];
            u8 bit  = col % 8;
            if (byte & (0x01 << bit)) {
                fb_set_pixel((u16)(x + col), (u16)(y + row), fc);
            } else if (!mode) {
                fb_set_pixel((u16)(x + col), (u16)(y + row), bc);
            }
        }
    }
}

/******************************************************************************
      显示字符串
******************************************************************************/
void LCD_ShowString(u16 x, u16 y, const u8 *p, u16 fc, u16 bc, u8 sizey, u8 mode)
{
    while (*p != '\0') {
        LCD_ShowChar(x, y, *p, fc, bc, sizey, mode);
        x += sizey / 2;
        p++;
    }
}

/******************************************************************************
      乘方工具
******************************************************************************/
u32 mypow(u8 m, u8 n)
{
    u32 result = 1;
    while (n--) result *= m;
    return result;
}

/******************************************************************************
      显示整数（前导零不显示）
******************************************************************************/
void LCD_ShowIntNum(u16 x, u16 y, u16 num, u8 len, u16 fc, u16 bc, u8 sizey)
{
    u8 t, temp;
    u8 enshow = 0;
    u8 sizex = sizey / 2;
    for (t = 0; t < len; t++) {
        temp = (num / mypow(10, len - t - 1)) % 10;
        if (enshow == 0 && t < (len - 1)) {
            if (temp == 0) {
                LCD_ShowChar(x + t * sizex, y, ' ', fc, bc, sizey, 0);
                continue;
            } else enshow = 1;
        }
        LCD_ShowChar(x + t * sizex, y, temp + 48, fc, bc, sizey, 0);
    }
}

/******************************************************************************
      显示两位小数
******************************************************************************/
void LCD_ShowFloatNum1(u16 x, u16 y, float num, u8 len, u16 fc, u16 bc, u8 sizey)
{
    u8 t, temp, sizex;
    u16 num1;
    sizex = sizey / 2;
    num1 = (u16)(num * 100);
    for (t = 0; t < len; t++) {
        temp = (num1 / mypow(10, len - t - 1)) % 10;
        if (t == (len - 2)) {
            LCD_ShowChar(x + (len - 2) * sizex, y, '.', fc, bc, sizey, 0);
            t++;
            len += 1;
        }
        LCD_ShowChar(x + t * sizex, y, temp + 48, fc, bc, sizey, 0);
    }
}

/******************************************************************************
      显示图片（RGB565 数组，写帧缓冲）
******************************************************************************/
void LCD_ShowPicture(u16 x, u16 y, u16 length, u16 width, const u8 pic[])
{
    u16 i, j;
    u32 k = 0;
    for (i = 0; i < length; i++) {
        for (j = 0; j < width; j++) {
            if (x + i < LCD_W && y + j < LCD_H) {
                u32 idx = ((u32)(y + j) * LCD_W + (x + i)) * 2;
                g_framebuffer[idx]     = pic[k * 2];
                g_framebuffer[idx + 1] = pic[k * 2 + 1];
            }
            k++;
        }
    }
}
