/*
 * KK_UI 应用：首页（运行 / 设置 / 状态 / 关于）及各页面
 *
 * 调用顺序：OLED_Init() → OLED_SetRotation() → ui_app_reset() → KK_UI_Init(ui_app_get())；
 * 主循环依次调用 ui_app_update()、KK_UI_Update()、ui_app_process_events()。
 */
#pragma once

#include <stdint.h>
#include "kk_ui.h"
#include "remote.h"

const KK_UI_App *ui_app_get(void);

// 在 KK_UI_Init() 之前调用，恢复默认设置并填充静态文本
void ui_app_reset(uint32_t now_ms);

// 刷新状态页文本
void ui_app_update(uint32_t now_ms);

// 处理 KK_UI 事件（设置变更、确认框）及故障提示
void ui_app_process_events(void);

// 执行一条手机遥控命令（主循环中调用）；设定占空比 > 0 时自动进入运行页
remote_result_t ui_app_remote_cmd(const remote_cmd_t *cmd, uint32_t now_ms);

// 填写遥控状态快照
void ui_app_remote_status(remote_status_t *out);
