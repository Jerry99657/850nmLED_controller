/*
 * 板载单颗 WS2812-V6 状态灯（GPIO8，RMT）
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"

esp_err_t ws2812_init(void);

// 设置颜色，0~255，内部不做亮度缩放
esp_err_t ws2812_set(uint8_t r, uint8_t g, uint8_t b);
