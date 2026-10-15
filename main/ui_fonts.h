/*
 * KK_UI 界面字模（u8g2 格式），由 tools/gen_ui_fonts.py 生成到 ui_fonts.c。
 * 修改 ui_app.c 中的界面文字后需重新运行脚本。
 */
#pragma once

#include <stdint.h>

extern const uint8_t ui_font_home[];    // 首页标签
extern const uint8_t ui_font_title[];   // 标题栏
extern const uint8_t ui_font_body[];    // 正文、菜单、弹框
extern const uint8_t ui_font_big[];     // 运行页大号数字
