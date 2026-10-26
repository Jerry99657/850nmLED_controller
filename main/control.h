/*
 * 输出控制与保护
 *
 * 管理亮度设定、TMP102 过温保护、LED_PWM 输出和 WS2812 状态灯。
 * 只在主循环任务中调用，非线程安全。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "driver/i2c_master.h"
#include "sdkconfig.h"

/*
 * 三个温度阈值必须保持 恢复 < 预警 < 关断：
 *   关断  温度到这儿就切输出
 *   预警  状态灯转橙色、状态页显示“温度预警”，纯提示，不影响输出
 *   恢复  过温关断后降到这儿以下自动接着输出
 * 关断和恢复挨得太近会来回打摆：刚恢复就再次关断。
 *
 * 顺序靠“范围联动”保证：每个阈值的可调范围由另外两个的当前值算出来，
 * 所以怎么按都设不坏。范围在 ui_app.c 的 cfg_update_ranges() 里每改一次值就重算：
 *   恢复  0 ~ 预警-1 ／ 预警  恢复+1 ~ 关断-1 ／ 关断  预警+1 ~ 100
 * 关断的实时下限因此跟着预警走（默认预警 55 → 关断最低 56）。
 *
 * 下面这几个只留绝对上下限：给 Kconfig 默认值和旧的 NVS 值兜底，
 * 也保证联动算出来的区间不会是空的（先钳绝对范围，顺序就总成立）。
 * 关断的实时下限不是常量，网页和手机 App 要用 remote_status_t.shut_min，
 * 别在页面里写死（tools/ble_control.html 从状态包字节 [18] 的高 7 位取关断下限，
 * 预警温度 = 下限 − 1，页面上就这么显示）。
 * 这三个值在 Kconfig 里各有一份对应的 range，改这里记得一起改。
 */
#define CONTROL_RESUME_C_MIN    0
#define CONTROL_RESUME_C_MAX    90
#define CONTROL_WARN_C_MIN      2
#define CONTROL_WARN_C_MAX      99
#define CONTROL_SHUTDOWN_C_MIN  3
#define CONTROL_SHUTDOWN_C_MAX  100

typedef enum {
    CTRL_FAULT_NONE = 0,
    CTRL_FAULT_TEMP_SENSOR,
    CTRL_FAULT_OVERTEMP,
    CTRL_FAULT_DRIVER,          // VBUS 有电但 LP8556 LED_EN 写不进去
} ctrl_fault_t;

typedef enum {
    CTRL_DRV_NO_POWER = 0,      // VBUS 无电，LP8556 未上电（只接下载口）
    CTRL_DRV_PENDING,           // VBUS 已接入，等待写入 LED_EN
    CTRL_DRV_OK,                // 已配置为 CONFIG_LP8556_LED_EN
    CTRL_DRV_FAIL,
} ctrl_driver_t;

typedef struct {
    bool temp_valid;
    float temp_c;
    uint16_t tmp102_addr;   // 0 表示未找到
    uint16_t pwm;           // 当前输出，千分比
    uint16_t setpoint;      // 亮度设定，千分比
    uint16_t limit;         // 占空比上限，千分比
    ctrl_fault_t fault;
    bool warn;              // 温度达到预警阈值
    uint16_t shutdown_c;    // 过温关断阈值，可在运行时修改
    uint16_t warn_c;        // 预警阈值
    uint16_t resume_c;      // 过温恢复阈值
    bool demo;              // 阶梯测试模式
    bool protect;           // 温度保护是否开启
    bool vbus_ok;           // VBUS ADC 可用
    bool vbus_9v;           // true = 9 V 档（上限 CONFIG_LED_PWM_LIMIT_9V_PERCENT），false = 5 V 档
    int vbus_mv;            // 按分压比换算的 VBUS
    bool vbus_saturated;    // ADC 已饱和，vbus_mv 只是下限
    ctrl_driver_t driver;   // LP8556 配置状态
    bool timer_active;      // 定时关断已开启
    uint32_t timer_left_s;  // 剩余秒数
} ctrl_status_t;

// LED_PWM 必须已初始化为 0%；bus 为 NULL 时视为无温度传感器
void control_init(i2c_master_bus_handle_t bus, uint16_t tmp102_addr);

// 主循环每次调用：温度采样、保护判定、PWM 输出、状态灯
void control_update(uint32_t now_ms);

// 按步进调整设定值（千分比，可为负）；故障或阶梯测试时拒绝并返回 false
bool control_adjust(int delta_permille);

// 直接设定亮度（千分比，超过上限会被钳位）；故障或阶梯测试时拒绝并返回 false
bool control_set_setpoint(uint16_t permille);

/*
 * 定时关断：输出运行 minutes 分钟后自动归零。
 * 只能在设定值 > 0 时开启（否则返回 false）；minutes = 0 取消定时。
 * 设定值因任何原因归零（停止、故障、离开运行页、到时）都会同时取消定时。
 */
bool control_set_timer_min(uint32_t minutes);

// 定时到时后返回一次 true（供界面提示）
bool control_take_timer_expired(void);

// 输出中接入功率输入、为配置 LP8556 而停止输出后返回一次 true（供界面提示）
bool control_take_vbus_stop(void);

void control_get_status(ctrl_status_t *out);

void control_set_softstart_ms(uint32_t ms);

/*
 * 设定过温关断阈值（℃）。低于 预警+1 时顶到 预警+1（保证 预警 < 关断），
 * 高于 CONTROL_SHUTDOWN_C_MAX 时钳位。
 * 调低到当前温度以下会立即判定并锁存过温故障（下一次 control_update 之前生效）。
 */
void control_set_shutdown_c(uint16_t c);

/*
 * 设定预警阈值（℃），钳到 (恢复, 关断) 之间。只影响状态灯颜色和状态显示。
 */
void control_set_warn_c(uint16_t c);

/*
 * 设定过温恢复阈值（℃），高于 预警-1 时顶到 预警-1（保证 恢复 < 预警）。
 * 调高到当前温度以上会立即解除已锁存的过温故障并恢复输出。
 */
void control_set_resume_c(uint16_t c);

// 关闭后不再因过温或传感器故障关断输出（仍受占空比上限约束），并清除已锁存的温度故障；
// 重新开启时立即判定。
void control_set_thermal_protect(bool enable);
void control_set_status_led(bool enable, uint8_t brightness_percent);

// 设定值清零并立即关断 LED_PWM，不走软启动斜坡（离开运行页时调用）
void control_force_off(void);

/*
 * 状态灯颜色优先级：
 *   故障        黄色闪烁（不受状态灯开关影响）
 *   PWM > 0     红色，亮度 = 占空比 x 50%（不受状态灯开关影响）
 *   状态灯关闭  熄灭
 *   温度预警    橙色
 *   其它        页面颜色，由 UI 通过 control_set_idle_color() 设定（满亮度值，按状态灯亮度缩放）
 */
void control_set_idle_color(uint8_t r, uint8_t g, uint8_t b);
