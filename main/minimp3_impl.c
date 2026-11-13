/* minimp3 单头文件的实现编译单元。
 * 其它 .c 只 #include "minimp3.h" 拿声明，实现只在这里展开一次。
 *
 * 上游：https://github.com/lieff/minimp3  (CC0 / 公共领域，无版权限制)
 *
 * 裁剪宏说明：
 *  - MINIMP3_ONLY_MP3 : 只编 Layer III，去掉 Layer I/II 解码表，省 Flash
 *  - MINIMP3_NO_SIMD  : ESP32-S3 是 Xtensa，没有 SSE/NEON，走通用标量实现
 *  - 不定义 MINIMP3_FLOAT_OUTPUT：直接输出 int16_t PCM，正好喂 I2S
 */
#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ONLY_MP3
#define MINIMP3_NO_SIMD
#include "minimp3.h"
