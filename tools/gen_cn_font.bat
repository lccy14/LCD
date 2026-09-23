@echo off
chcp 65001 >nul
REM ============================================================
REM  生成 LVGL 中文字库（命令行方式）
REM
REM  前置条件：
REM   1. 安装 Node.js:      https://nodejs.org/
REM   2. 安装转换工具:       npm install -g lv_font_conv
REM   3. 下载思源黑体:       NotoSansSC-Regular.otf
REM      (Google Fonts 搜 Noto Sans SC，或 GitHub notofonts/noto-cjk)
REM      把 otf 文件放到本 tools 目录下
REM
REM  注意：本文件需保存为 UTF-8 编码。若生成的字出现乱码，
REM        请改用在线转换 https://lvgl.io/tools/fontconverter
REM        （字符集在 cn_chars.txt）
REM ============================================================

set FONT=NotoSansSC-Regular.otf
set CHARS=无线设置时钟音乐游戏天气返回扫描中已关闭找到个网络连接正在失败亮度断开取消未请检查密码的加入同步间我闹每天一次自定义开始重置暂实现分数刷新尚获更新湿风速晴少云阴雾凇毛细密集冻雪米阵强暴雷伴冰雹知雨大小

if not exist "%FONT%" (
    echo [ERROR] 找不到字体文件 %FONT%
    echo         请先下载 NotoSansSC-Regular.otf 放到 tools 目录
    pause
    exit /b 1
)

echo 生成 16px 字库...
lv_font_conv --font %FONT% --size 16 --bpp 4 -r 0x20-0x7E --symbols "%CHARS%" --format lvgl -o lv_font_cn_16.c --force-fast-kern-format

echo 生成 20px 字库...
lv_font_conv --font %FONT% --size 20 --bpp 4 -r 0x20-0x7E --symbols "%CHARS%" --format lvgl -o lv_font_cn_20.c --force-fast-kern-format

echo.
echo 完成！请把生成的 lv_font_cn_16.c 和 lv_font_cn_20.c 复制到 main\ 目录
pause
