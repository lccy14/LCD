#ifndef __BOARD_H
#define __BOARD_H

/* ============================================================
 *  板级引脚 / 设备地址 集中配置（BSP 层）
 * ------------------------------------------------------------
 *  换板子：只改本文件（必要再动 bsp.c）。
 *  驱动 / 服务 / 界面一律引用这里的宏，不得出现任何裸 GPIO 号，
 *  这样硬件接线变了只动这一层，上层一行不用改。
 *
 *  当前板：ST7789 1.14" 并口 8bit + FT6336 触摸(I2C) + SD(SDMMC)
 *           + MAX98357AETE+T 功放(I2S，仅第二版焊接)。
 *  RGB LED 暂未焊接，仅在底部留位置占位。
 * ============================================================ */

#include <stdint.h>
#include "driver/gpio.h"   /* GPIO_NUM_x 需要；v6 已无 driver/i2c.h，I2C 总线号留在 ft6336.h */

/* ============================================================
 *  板级版本开关
 * ------------------------------------------------------------
 *  1 = 第一版 PCB（旧引脚分配，无板载功放）
 *  2 = 第二版 PCB（默认，全部外设重新布线，音频走 GPIO4~7）
 *
 *  ⚠️ 两版的 LCD / 触摸 / SD / 音频引脚完全不同，绝不能混用。
 *     如果手上还是第一版板子，把 BOARD_REV 改成 1 再烧录，
 *     否则 LCD 不会亮、SD 也挂不上。其它代码一行都不用动。
 * ============================================================ */
#ifndef BOARD_REV
#define BOARD_REV 2
#endif

#define FT6336_I2C_ADDR     0x38    /* 主地址 */
#define FT6336_I2C_ADDR_ALT 0x39    /* 部分模组/上电时序为 0x39，init 会两个都探测 */

#if BOARD_REV == 1

/* ===================== 第一版 PCB ===================== */

/* ---------- LCD：ST7789 并行 8bit ---------- */
#define LCD_CS_GPIO     GPIO_NUM_37  /* 片选 */
#define LCD_DC_GPIO     GPIO_NUM_35  /* 数据/命令 RS */
#define LCD_WR_GPIO     GPIO_NUM_36  /* 写时钟 PCLK */
#define LCD_RST_GPIO    GPIO_NUM_21  /* 复位 */
#define LCD_BLK_GPIO    GPIO_NUM_38  /* 背光 */
#define LCD_D0_GPIO     GPIO_NUM_6
#define LCD_D1_GPIO     GPIO_NUM_7
#define LCD_D2_GPIO     GPIO_NUM_17
#define LCD_D3_GPIO     GPIO_NUM_18
#define LCD_D4_GPIO     GPIO_NUM_8
#define LCD_D5_GPIO     GPIO_NUM_19
#define LCD_D6_GPIO     GPIO_NUM_20
#define LCD_D7_GPIO     GPIO_NUM_3
/* RD 引脚在并口只读屏幕必须接 3.3V，软件不控制 */

/* ---------- 触摸：FT6336（I2C） ---------- */
#define FT6336_SDA_GPIO GPIO_NUM_5
#define FT6336_SCL_GPIO GPIO_NUM_4
#define FT6336_INT_GPIO GPIO_NUM_40
/* 第一版 FT6336 的 RST 直接接 3V3 / 悬空，不需要宏 */

/* ---------- SD 卡：SDMMC 4 线 ---------- */
#define SDMMC_PIN_CLK   41
#define SDMMC_PIN_CMD   42
#define SDMMC_PIN_D0    47
#define SDMMC_PIN_D1    48
#define SDMMC_PIN_D2    1
#define SDMMC_PIN_D3    2

/* ---------- 音频 ----------
 * 第一版没有焊接功放。这几个脚是早期预留假设值，仅保证代码可编译，
 * 实际使用音频请切到 BOARD_REV 2。 */
#define AUDIO_I2S_BCLK_GPIO    GPIO_NUM_9
#define AUDIO_I2S_LRCK_GPIO    GPIO_NUM_10
#define AUDIO_I2S_DOUT_GPIO    GPIO_NUM_11
#define AUDIO_I2S_SD_MODE_GPIO GPIO_NUM_12

#else

/* ===================== 第二版 PCB =====================
 * 依据第二版原理图 U6（ESP32-S3-WROOM-1）：黑色 IOx 为 GPIO 编号，
 * 蓝色下划线网络名为实际连接。以下逐一对应，勿与第一版混用。
 * ===================================================== */

/* ---------- 音频：MAX98357AETE+T（I2S 输入、Class D 功放） ----------
 * 原理图左侧引脚 4~7：SD_MODE / DIN / BCLK / LRCLK。
 * 只需 3 根信号线：BCLK / LRCK(WS) / DIN(接主控 DOUT)。
 * MAX98357A 内部从 BCLK 恢复时钟，不需要 MCLK；无需 I2C 配置寄存器。
 * SD_MODE：低=shutdown(静音省电)，高=正常工作。
 *   若硬件把 SD_MODE 直接上拉到 VDD，把下面宏改成 GPIO_NUM_NC 即跳过控制。 */
#define AUDIO_I2S_BCLK_GPIO    GPIO_NUM_6   /* IO6  → BCLK */
#define AUDIO_I2S_LRCK_GPIO    GPIO_NUM_7   /* IO7  → LRCLK */
#define AUDIO_I2S_DOUT_GPIO    GPIO_NUM_5   /* IO5  → 功放 DIN */
#define AUDIO_I2S_SD_MODE_GPIO GPIO_NUM_4   /* IO4  → SD_MODE */

/* ---------- 触摸：FT6336（I2C） ----------
 * 原理图左侧引脚 8~11：TP_RES / TP_INT / TP_CLK / TP_SDA。
 * 第二版把复位脚也接到了 MCU，ft6336.c 会先拉高释放复位。 */
#define FT6336_SDA_GPIO GPIO_NUM_18  /* IO18 → TP_SDA */
#define FT6336_SCL_GPIO GPIO_NUM_17  /* IO17 → TP_CLK(SCL) */
#define FT6336_INT_GPIO GPIO_NUM_16  /* IO16 → TP_INT */
#define FT6336_RST_GPIO GPIO_NUM_15  /* IO15 → TP_RES */

/* ---------- LCD：ST7789 并行 8bit ----------
 * 原理图底部引脚 15~26，网络名是双模式复用标注（8080 / SPI）：
 *   IO46 = "DC\SPICLK"、IO9 = "WR/DC" —— 这里按代码实际使用的 8080 并口模式
 *   取前者，即 DC=IO46、WR=IO9。若硬件跳线成 SPI 模式，需改用 esp_lcd SPI 驱动。
 * IO45 = TE（ tearing effect 输入，代码未使用）；IO8 = RESET；IO35 = BL_EN。
 * ⚠️ GPIO45/46 是 ESP32-S3 的 strapping 引脚，外围不要加强上拉/下拉，
 *    否则会影响上电启动模式。 */
#define LCD_CS_GPIO     GPIO_NUM_3   /* IO3  → CS */
#define LCD_DC_GPIO     GPIO_NUM_46  /* IO46 → DC */
#define LCD_WR_GPIO     GPIO_NUM_9   /* IO9  → WR */
#define LCD_RST_GPIO    GPIO_NUM_8   /* IO8  → RESET */
#define LCD_BLK_GPIO    GPIO_NUM_35  /* IO35 → BL_EN */
#define LCD_TE_GPIO     GPIO_NUM_45  /* IO45 → TE（预留，代码未使用） */
#define LCD_D0_GPIO     GPIO_NUM_10  /* IO10 */
#define LCD_D1_GPIO     GPIO_NUM_11  /* IO11 */
#define LCD_D2_GPIO     GPIO_NUM_12  /* IO12 */
#define LCD_D3_GPIO     GPIO_NUM_13  /* IO13 */
#define LCD_D4_GPIO     GPIO_NUM_14  /* IO14 */
#define LCD_D5_GPIO     GPIO_NUM_21  /* IO21 */
#define LCD_D6_GPIO     GPIO_NUM_47  /* IO47 */
#define LCD_D7_GPIO     GPIO_NUM_48  /* IO48 */
/* RD 引脚在并口只读屏幕必须接 3.3V，软件不控制 */

/* ---------- SD 卡：SDMMC 4 线（裸 GPIO 号，sd_card.c 用 (gpio_num_t) 转换） ----------
 * 原理图右侧引脚 29~34：SD_DATA2/3、SD_CMD、SD_SCLK、SD_DATA0/1。
 * IO42 上的 "SD" 网络是卡座检测/控制信号，代码用 SDMMC_SLOT_NO_CD，未使用。 */
#define SDMMC_PIN_CLK   39           /* IO39 → SD_SCLK */
#define SDMMC_PIN_CMD   38           /* IO38 → SD_CMD */
#define SDMMC_PIN_D0    40           /* IO40 → SD_DATA0 */
#define SDMMC_PIN_D1    41           /* IO41 → SD_DATA1 */
#define SDMMC_PIN_D2    36           /* IO36 → SD_DATA2 */
#define SDMMC_PIN_D3    37           /* IO37 → SD_DATA3 */

/* ---------- 麦克风：I2S 数字 MEMS（INMP441 / ICS-43432 等） ----------
 * 第二版预留引出脚，按用户实际飞线：WS=IO1、SCK(BCLK)=IO2、SD(数据)=IO42。
 * 麦克风侧的 SD 是数据输出，接主控的 DIN；SCK 即 I2S 位时钟 BCLK。
 * 走独立 I2S_NUM_1 控制器，与功放的 I2S_NUM_0 完全隔离，采样率互不干扰。
 * 注：IO42 在原理图上也标了 "SD"（卡座检测），但 sd_card.c 用 NO_CD 未占用。
 * ⚠️ 第一版 IO1/IO2 是 SD D2/D3、IO42 是 SD CMD，绝不能接麦克风，
 *    因此这三个宏只在第二版分支定义，mic_rec.c 里用 #ifdef 判空实现。 */
#define MIC_HW_PRESENT     1
#define MIC_I2S_WS_GPIO    GPIO_NUM_1   /* IO1  → WS (LRCK) */
#define MIC_I2S_BCLK_GPIO  GPIO_NUM_2   /* IO2  → SCK (BCLK) */
#define MIC_I2S_DIN_GPIO   GPIO_NUM_42  /* IO42 → SD  (麦克风数据 → 主控 DIN) */
/* 模块 L/R 引脚接 VDD 时数据在右声道，改成 1；接 GND(常见)保持 0 */
#define MIC_SLOT_RIGHT     0

/* 第二版其余已被占用 / 预留的脚，登记备查，勿复用：
 *   IO0(引脚27) 启动按键；IO19/IO20（引脚13/14，空闲引出）；
 *   TXD0/RXD0 = GPIO43/44（UART0 调试口）。
 */

#endif /* BOARD_REV */

/* 第一版 PCB 没有麦克风引出脚：给编译期占位，mic_rec.c 靠 MIC_HW_PRESENT
 * 在 init 里禁用整个功能（GPIO_NUM_NC 不会真的被配置）。 */
#ifndef MIC_HW_PRESENT
#define MIC_HW_PRESENT     0
#define MIC_I2S_WS_GPIO    GPIO_NUM_NC
#define MIC_I2S_BCLK_GPIO  GPIO_NUM_NC
#define MIC_I2S_DIN_GPIO   GPIO_NUM_NC
#define MIC_SLOT_RIGHT     0
#endif

/* ---------- 预留（暂未焊接，加功能/换板时在此登记） ----------
 * RGB LED WS2812 (RMT 方案) : #define BOARD_LED_PIN    GPIO_NUM_x
 * 触摸备选 IQS333 (I2C)     : 复用 FT6336 的 SDA/SCL，改地址即可
 */

#endif /* __BOARD_H */
