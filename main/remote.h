/*
 * 手机遥控：Wi-Fi 热点 + HTTP 网页 / JSON 接口
 *
 * 手机连接热点后访问 http://192.168.4.1
 *   GET  /                  控制网页
 *   GET  /api/status        状态 JSON
 *   POST /api/duty?pct=N    设定占空比 N%（0~100，0 = 停止）
 *   POST /api/timer?min=N   定时 N 分钟后关断（1~999），N = 0 取消定时
 *   POST /api/temp?c=N      过温关断阈值 N℃（下限 = 板上的预警阈值+1，随预警联动；上限 100）
 *   POST /api/stop          立即停止输出
 *
 * 蓝牙 BLE（可选，见 ble_link.h）：与热点同名广播，命令同样进入本模块的命令队列。
 * 蓝牙连接时关闭热点，断开后恢复；有手机连在热点上时停止蓝牙广播。
 *
 * 线程模型：HTTP 处理在 httpd 任务、蓝牙在 NimBLE 任务中运行，只把命令放进队列；
 * 命令由主循环通过 remote_poll() 取出、执行后 remote_reply() 回复（HTTP 等待结果，
 * 蓝牙结果随下一次状态通知带回）。状态由主循环通过 remote_publish() 定期写入。
 *
 * 安全：热点只有 WPA2 密码一层保护，HTTP 本身没有认证；蓝牙未加密。
 * 能连上的任何人都能控制红外输出。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef enum {
    REMOTE_CMD_DUTY,    // permille
    REMOTE_CMD_TIMER,   // minutes，0 = 取消
    REMOTE_CMD_STOP,
    REMOTE_CMD_TEMP,    // temp_c，过温关断阈值
} remote_cmd_kind_t;

typedef enum {
    REMOTE_SRC_HTTP,
    REMOTE_SRC_BLE,
} remote_src_t;

typedef struct {
    remote_cmd_kind_t kind;
    uint16_t permille;      // REMOTE_CMD_DUTY
    uint16_t minutes;       // REMOTE_CMD_TIMER
    uint16_t temp_c;        // REMOTE_CMD_TEMP：过温关断阈值（℃）
    uint32_t seq;
    remote_src_t source;
} remote_cmd_t;

typedef enum {
    REMOTE_LINK_NONE,
    REMOTE_LINK_WIFI,       // 有手机连在热点上
    REMOTE_LINK_BLE,        // 有手机通过蓝牙连接（此时热点关闭）
} remote_link_t;

typedef enum {
    REMOTE_OK = 0,
    REMOTE_ERR_LOCKED,      // 故障锁定或阶梯测试
    REMOTE_ERR_NOT_RUNNING, // 输出未开启，不能定时
    REMOTE_ERR_BUSY,        // 界面忙（转场中），稍后重试
} remote_result_t;

typedef struct {
    uint16_t pwm, setpoint, limit;     // 千分比
    bool vbus_ok, vbus_9v;
    int vbus_mv;
    bool vbus_saturated;
    bool temp_valid;
    float temp_c;
    uint8_t fault;                     // ctrl_fault_t
    uint16_t shutdown_c;               // 过温关断阈值（℃）
    // 关断阈值的下限，= 预警+1，随预警阈值联动（见 control.h），
    // 所以不能写成编译期常量。上限仍是 CONTROL_SHUTDOWN_C_MAX。
    uint16_t shut_min;
    bool warn, protect, demo;
    uint8_t driver;                    // ctrl_driver_t
    bool timer_active;
    uint32_t timer_left_s;
    bool on_run_page;
} remote_status_t;

// 创建命令队列，并在后台任务中延时 delay_ms 后启动热点与 HTTP 服务。
// 延时是为了避开上电时 3.3 V 尚未稳定的阶段，让 Wi-Fi 射频校准/发射的电流尖峰错开。
// 启动失败只打印日志，不影响本地操作。
esp_err_t remote_start_delayed(uint32_t delay_ms);

bool remote_is_running(void);
bool remote_is_starting(void);
bool remote_wifi_on(void);          // 热点当前是否开启（蓝牙连接时关闭）
bool remote_ble_running(void);
const char *remote_ble_name(void);
remote_link_t remote_link(void);

// 蓝牙命令入口（由 ble_link 在 NimBLE 任务中调用）：op 1 占空比 / 2 定时 / 3 停止 / 4 关断温度
void remote_ble_command(uint8_t op, uint8_t seq, uint16_t value);
const char *remote_ssid(void);
const char *remote_password(void);
int remote_client_count(void);

// 主循环调用：取出一条待执行命令，没有时返回 false
bool remote_poll(remote_cmd_t *cmd);

// 主循环调用：回复 remote_poll() 取出的命令
void remote_reply(const remote_cmd_t *cmd, remote_result_t result);

// 主循环调用：更新状态快照
void remote_publish(const remote_status_t *st);
