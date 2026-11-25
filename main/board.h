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
 *  2 = 第二版 PCB（外接 Quad PSRAM，SD 4线，背光 IO35）
 *  3 = 第三版 PCB（默认，ESP32-S3R8 内置 Octal PSRAM，SD 1线，背光 IO41）
 *
 *  ⚠️ 三版的 LCD / 触摸 / SD / 音频 / 背光引脚不同，绝不能混用。
 *     换板只改 BOARD_REV：1=第一版、2=第二版、3=第三版（默认）。
 *     第三版芯片是 S3R8，IO35/36/37 被内部 PSRAM 占用，代码里已避开。
 * ============================================================ */
#ifndef BOARD_REV
#define BOARD_REV 3
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

#else  /* BOARD_REV == 2 或 3：公共引脚定义 + 末尾版本差异 */

/* ===================== 第二/三版 PCB 公共引脚 =====================
 * 依据原理图 U6：音频 IO4~7、触摸 IO15~18、LCD 数据 IO3/46/9/8/10~14/21/47/48/45、
 * 麦克风 WS=IO1/SCK=IO2/SD=IO42、USB IO19/20、UART0 IO43/44 —— 两版完全一致。
 * 【LCD 背光】和【SD 卡数据线】两版不同，在本块末尾用 #if BOARD_REV 区分。
 * ================================================================ */

/* ---------- 音频：MAX98357AETE+T（I2S 输入、Class D 功放） ----------
 * 原理图左侧引脚 4~7：SD_MODE / DIN / BCLK / LRCLK。
 * 只需 3 根信号线：BCLK / LRCK(WS) / DIN(接主控 DOUT)。
 * MAX98357A 内部从 BCLK 恢复时钟，不需要 MCLK；无需 I2C 配置寄存器。
 * SD_MODE：低=shutdown(静音省电)，高=正常工作。
 *   若硬件把 SD_MODE 直接上拉到 VDD，把下面宏改成 GPIO_NUM_NC 即跳过控制。 */
#define AUDIO_I2S_BCLK_GPIO    GPIO_NUM_6   /* IO6  → BCLK */
#define AUDIO_I2S_LRCK_GPIO    GPIO_NUM_7   /* IO7  → LRCLK */
#define AUDIO_I2S_DOUT_GPIO    GPIO_NUM_5   /* IO5  → 功放 DIN */
/* 第二版硬件：MAX98357A 的 SD_MODE 经 10K 上拉到 VDD。
 * 由软件控制：播放时拉高(功放工作)，暂停/停止时拉低(关断、消除底噪滋滋声)。
 * GPIO 推挽输出的低阻抗远小于 10K，写 0 能可靠拉低关断。 */
#define AUDIO_I2S_SD_MODE_GPIO GPIO_NUM_4   /* IO4  → SD_MODE（软件控制，拉高=工作） */

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
 * IO45 = TE（ tearing effect 输入，代码未使用）；IO8 = RESET。
 * 背光 BL_EN 第二版=IO35、第三版=IO41（见文件末尾版本差异）。
 * ⚠️ GPIO45/46 是 ESP32-S3 的 strapping 引脚，外围不要加强上拉/下拉，
 *    否则会影响上电启动模式。 */
#define LCD_CS_GPIO     GPIO_NUM_3   /* IO3  → CS */
#define LCD_DC_GPIO     GPIO_NUM_46  /* IO46 → DC */
#define LCD_WR_GPIO     GPIO_NUM_9   /* IO9  → WR */
#define LCD_RST_GPIO    GPIO_NUM_8   /* IO8  → RESET */
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

/* ---------- SD 卡引脚：见文件末尾的版本差异（第二版 4 线 / 第三版 1 线） ---------- */

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

/* ===================== 版本差异：背光 + SD 卡 ===================== */
#if BOARD_REV == 2
/* ---------- 第二版：背光 IO35，SD 4 线 ----------
 * 注意：第二版用外接 Quad PSRAM（IO30/26），IO35/36/37 是普通 GPIO，可作 SD D3/D2/D1。 */
#define LCD_BLK_GPIO    GPIO_NUM_35  /* IO35 → BL_EN */
#define SDMMC_PIN_CLK   39           /* IO39 → SD_SCLK */
#define SDMMC_PIN_CMD   38           /* IO38 → SD_CMD */
#define SDMMC_PIN_D0    40           /* IO40 → SD_DATA0 */
#define SDMMC_PIN_D1    41           /* IO41 → SD_DATA1 */
#define SDMMC_PIN_D2    36           /* IO36 → SD_DATA2 */
#define SDMMC_PIN_D3    37           /* IO37 → SD_DATA3 */

#else  /* BOARD_REV == 3：第三版 S3R8 */
/* ---------- 第三版：背光 IO41，SD 1 线 ----------
 * ⚠️ S3R8 的 IO35/36/37 被模组内部 Octal PSRAM 占用，绝不可用！
 *    IO41 从原 SD D1 让给 LCD 背光；SD 只用 CLK/CMD/D0 三根线。 */
#define LCD_BLK_GPIO    GPIO_NUM_41  /* IO41 → BL_EN */
#define SDMMC_PIN_CLK   39           /* IO39 → SD_SCLK */
#define SDMMC_PIN_CMD   38           /* IO38 → SD_CMD */
#define SDMMC_PIN_D0    40           /* IO40 → SD_DATA0 */
/* 1 线用不到 D1~D3，但 sd_config_host_slot 的 4 线分支会引用这些宏，
 * 给 NC 占位保证编译；运行时 width=1 不会真的配置它们。 */
#define SDMMC_PIN_D1    GPIO_NUM_NC
#define SDMMC_PIN_D2    GPIO_NUM_NC
#define SDMMC_PIN_D3    GPIO_NUM_NC
#endif

/* 第三版其余已被占用 / 预留的脚，登记备查，勿复用：
 *   IO0(引脚27) 启动按键；IO19/IO20（引脚13/14，USB OTG D-/D+）；
 *   TXD0/RXD0 = GPIO43/44（UART0 调试口）。
 *   IO35/36/37 第三版被内部 PSRAM 占用，第二版是 SD D3/D2/D1，第一版是 LCD。 */

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
