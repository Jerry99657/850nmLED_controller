/*
 * TMP102 温度传感器（I²C 地址 0x48，ADD0 接地）
 */
#pragma once

#include "esp_err.h"
#include "driver/i2c_master.h"

// ADD0 接 GND/V+/SDA/SCL 分别对应 0x48~0x4B
#define TMP102_ADDR_MIN 0x48
#define TMP102_ADDR_MAX 0x4B

// 在已创建的 I²C 总线上挂载指定地址的 TMP102，并读一次配置寄存器确认通信
esp_err_t tmp102_init(i2c_master_bus_handle_t bus, uint16_t addr);

// 读取温度（℃），分辨率 0.0625 ℃
esp_err_t tmp102_read(float *temp_c);
