/*
 * LED_PWM 公共调光驱动（GPIO1，LEDC）
 *
 * 占空比单位为千分比（0~1000 = 0.0%~100.0%）。
 * 所有设置都会被 led_pwm_set_limit() 设定的上限钳位。
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"

#define LED_PWM_PERMILLE_MAX 1000

// 初始化 LEDC，输出保持 0%
esp_err_t led_pwm_init(void);

// 设置占空比上限（千分比），若当前占空比超过新上限则立即降到上限
esp_err_t led_pwm_set_limit(uint16_t limit_permille);

// 立即设置占空比（被上限钳位），不阻塞；软启动由调用方按时间步进
esp_err_t led_pwm_set(uint16_t permille);

// 立即关断（故障/过温路径使用，不渐变）
esp_err_t led_pwm_off(void);

uint16_t led_pwm_get(void);
uint16_t led_pwm_get_limit(void);
