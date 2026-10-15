/*
 * 两键 → KK_UI 三键输入适配
 *
 * KEY_UP（GPIO2）、KEY_DOWN/BOOT（GPIO9）均为低有效。
 * 两键在 UI_CHORD_WINDOW_MS 内先后按下视为“确定”；单键保持原样，
 * 可长按连发。输出的是按键电平，去抖与连发由 KK_UI_Update() 完成。
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"

esp_err_t ui_input_init(void);

// 返回 KK_UI_KEY_* 位组合，需每 ≤10 ms 调用一次
uint8_t ui_input_poll(uint32_t now_ms);
