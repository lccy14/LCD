#ifndef __LCD_H
#define __LCD_H		
#include "sys.h"
#include "lcd_init.h"   // 引入 LCD_W / LCD_H / USE_HORIZONTAL 等定义
#include "esp_attr.h"   // EXT_RAM_BSS_ATTR：把帧缓冲放进 PSRAM，省出内部 DRAM

// 帧缓冲（framebuffer）：所有绘制只改内存，最后统一 flush 到屏幕。
// 标 EXT_RAM_BSS_ATTR 后由链接器搬到 PSRAM（需 CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY），
// 否则 150KB 会挤爆内部 DRAM；ESP32-S3 的 SPI DMA 可访问 PSRAM，刷屏不受影响。
extern u8 g_framebuffer[LCD_W * LCD_H * 2];

void LCD_Flush(u16 x1, u16 y1, u16 x2, u16 y2);   // 把缓冲指定区域 DMA 刷到屏幕
void LCD_Flush_All(void);                          // 全屏刷新

void LCD_Fill(u16 xsta,u16 ysta,u16 xend,u16 yend,u16 color);//指定区域填充颜色（写缓冲）
void LCD_DrawPoint(u16 x,u16 y,u16 color);//在指定位置画一个点（写缓冲）
void LCD_DrawLine(u16 x1,u16 y1,u16 x2,u16 y2,u16 color);//在指定位置画一条线
void LCD_DrawRectangle(u16 x1, u16 y1, u16 x2, u16 y2,u16 color);//在指定位置画一个矩形
void Draw_Circle(u16 x0,u16 y0,u8 r,u16 color);//在指定位置画一个圆

void LCD_ShowChar(u16 x,u16 y,u8 num,u16 fc,u16 bc,u8 sizey,u8 mode);//显示一个字符
void LCD_ShowString(u16 x,u16 y,const u8 *p,u16 fc,u16 bc,u8 sizey,u8 mode);//显示字符串
u32 mypow(u8 m,u8 n);//求幂
void LCD_ShowIntNum(u16 x,u16 y,u16 num,u8 len,u16 fc,u16 bc,u8 sizey);//显示整数变量
void LCD_ShowFloatNum1(u16 x,u16 y,float num,u8 len,u16 fc,u16 bc,u8 sizey);//显示两位小数变量

void LCD_ShowPicture(u16 x,u16 y,u16 length,u16 width,const u8 pic[]);//显示图片


//画笔颜色
#define WHITE         	 0xFFFF
#define BLACK         	 0x0000	  
#define BLUE           	 0x001F  
#define BRED             0XF81F
#define GRED 			       0XFFE0
#define GBLUE			       0X07FF
#define RED           	 0xF800
#define MAGENTA       	 0xF81F
#define GREEN         	 0x07E0
#define CYAN          	 0x7FFF
#define YELLOW        	 0xFFE0
#define BROWN 			     0XBC40 //棕色
#define BRRED 			     0XFC07 //棕红色
#define GRAY  			     0X8430 //灰色
#define DARKBLUE      	 0X01CF	//深蓝色
#define LIGHTBLUE      	 0X7D7C	//浅蓝色  
#define GRAYBLUE       	 0X5458 //灰蓝色
#define LIGHTGREEN     	 0X841F //浅绿色
#define LGRAY 			     0XC618 //浅灰色(PANNEL),窗体背景色
#define LGRAYBLUE        0XA651 //浅灰蓝色(中间层颜色)
#define LBBLUE           0X2B12 //浅棕蓝色(选择条目的反色)

#endif
