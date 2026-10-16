/*
 * 用户设置掉电保存（NVS 命名空间 "settings"）
 *
 * 温度保护开关也保存；上电为关闭时会在日志和界面提示。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    int32_t softstart_ms;
    int32_t step_percent;
    int32_t led_brightness;
    bool led_enable;
    bool protect;
    int32_t shutdown_c;     // 过温关断阈值（℃）
    int32_t warn_c;         // 预警阈值（℃）
    int32_t resume_c;       // 过温恢复阈值（℃）
} app_settings_t;

// 初始化 NVS；失败时设置只保存在 RAM
esp_err_t settings_init(void);

// 读取已保存的项覆盖 *s，未保存的项保持原值；不做范围检查
void settings_load(app_settings_t *s);

esp_err_t settings_save(const app_settings_t *s);
