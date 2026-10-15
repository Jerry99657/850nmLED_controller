/*
 * VBUS 电压采样（GPIO0 / ADC1_CH0，分压比 CONFIG_VBUS_DIVIDER_PERMILLE / 1000）
 *
 * ADC 使用 12 dB 衰减，校准后有效量程约 0~2.5 V；9 V 输入时 ADC 端约 3.0 V
 * 会饱和，因此只用于 5 V / 9 V 判档，不作为 9 V 的精确测量。
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

esp_err_t vbus_init(void);

// 多次采样平均；adc_mv 为 ADC 引脚电压，vbus_mv 为按分压比换算的 VBUS
esp_err_t vbus_read(int *adc_mv, int *vbus_mv);

// ADC 引脚电压是否已到校准量程上限（读数不可信，只能说明 VBUS 很高）
bool vbus_adc_saturated(int adc_mv);
