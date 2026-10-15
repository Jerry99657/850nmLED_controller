/*
 * LP8556SQ-E09 LED 驱动配置（三颗同地址 0x2C，I²C 写入为广播，禁止读取）
 *
 * 上电默认 16h LED_EN = 0x3F（6 路全开）。本板每颗只接 5 路，LED6 悬空；
 * 未接的通道若保持使能，自适应升压会一直抬高 VBOOST 到上限（数据手册 8.3.2 / 8.3.3.1.1）。
 * 因此每次 VBUS 上电后、在 PWM 为低（Standby）时写 LED_EN = 0x1F 关闭第 6 路。
 *
 * 数据手册 8.4.2：Active 模式下配置寄存器不可访问，必须在 PWM 为低时写入。
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

#define LP8556_ADDR         0x2C
#define LP8556_REG_LED_EN   0x16

esp_err_t lp8556_init(i2c_master_bus_handle_t bus);

// 写 LED_EN（bit5:0 对应 LED6:1）；VDD 未上电时器件不应答，返回错误
esp_err_t lp8556_set_led_enable(uint8_t mask);
