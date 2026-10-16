/*
 * 输出控制与保护
 */
#include "control.h"

#include "esp_log.h"
#include "sdkconfig.h"
#include "led_pwm.h"
#include "lp8556.h"
#include "tmp102.h"
#include "vbus.h"
#include "ws2812.h"

#define TEMP_PERIOD_MS      500
#define TEMP_FAIL_MAX       3       // 连续读失败次数达到后判定传感器故障
#define LED_PERIOD_MS       50      // 状态灯刷新周期，跟随占空比变化
#define LED_BLINK_MS        250     // 故障闪烁半周期
#define LED_PWM_RED_MAX     128     // PWM 100% 对应红色亮度 50%
#define DEMO_HOLD_MS        3000
#define VBUS_PERIOD_MS      100
#define VBUS_UP_COUNT       3       // 5V→9V 需连续 3 次（约 300 ms），等 PD 协商稳定
#define LP_VBUS_ON_MV       4000    // VBUS 高于此值视为 LP8556 已上电
#define LP_VBUS_OFF_MV      3000
#define LP_QUIET_MS         100     // PWM 为低持续时间，器件约 50 ms 后进入 Standby
#define LP_RETRY_MS         200
#define LP_FAIL_MS          2000    // VBUS 有电但持续写不进配置，判定驱动故障

#ifdef CONFIG_LED_PWM_DEMO
#define DEMO_ENABLED        1
#else
#define DEMO_ENABLED        0
#endif

static const char *TAG = "control";

static bool s_sensor_ok;
static uint16_t s_tmp102_addr;
static float s_temp;
static int s_temp_fail = TEMP_FAIL_MAX;    // 首次读成功前视为无效
static ctrl_fault_t s_fault;            // 当前生效的故障 = 温度故障优先，其次驱动故障
static ctrl_fault_t s_fault_thermal;
static ctrl_fault_t s_fault_driver;
static bool s_lp_dev_ok;
static bool s_vbus_present;
static bool s_lp_ok;                    // 本次 VBUS 上电后 LED_EN 已写入
static bool s_vbus_stop_event;
static uint32_t s_t_pwm_low, s_t_lp_try, s_lp_fail_since;
static uint16_t s_setpoint;
static uint32_t s_softstart_ms = CONFIG_LED_PWM_SOFTSTART_MS;
static bool s_led_enable = true;
static uint8_t s_led_brightness = CONFIG_WS2812_BRIGHTNESS_PERCENT;
static bool s_protect = true;               // 温度保护，由 UI 按保存的设置设定
static uint16_t s_shutdown_c = CONFIG_THERMAL_SHUTDOWN_C;  // 过温关断阈值，可在运行时修改
static uint16_t s_warn_c = CONFIG_THERMAL_WARN_C;          // 预警阈值，可在运行时修改
static uint16_t s_resume_c = CONFIG_THERMAL_RESUME_C;      // 过温恢复阈值，可在运行时修改
// 过温关断后等温度降到 s_resume_c 以下再自动恢复：
// 记下关断前的设定值和关断时刻，恢复时把设定值还回去，并把定时暂停掉的时间补回来。
static bool s_overtemp_hold;
static uint16_t s_hold_setpoint;
static uint32_t s_hold_start_ms;
static uint8_t s_idle_rgb[3] = {0, 0, 255}; // 页面颜色（满亮度），按 s_led_brightness 缩放
static uint8_t s_led_shown[3];
static bool s_led_valid;

static bool s_vbus_ok;
static bool s_vbus_9v;
static int s_adc_mv, s_vbus_mv;
static int s_vbus_up_cnt;

static bool s_timer_active;
static uint32_t s_timer_end_ms;
static bool s_timer_expired;
static uint32_t s_now;

static uint32_t s_t_temp, s_t_led, s_t_demo, s_t_ramp, s_t_vbus;
static int32_t s_ramp_q;
static bool s_first = true;

static const char *fault_name(ctrl_fault_t f)
{
    switch (f) {
    case CTRL_FAULT_TEMP_SENSOR: return "temp sensor";
    case CTRL_FAULT_OVERTEMP:    return "overtemp";
    case CTRL_FAULT_DRIVER:      return "LP8556 config";
    default:                     return "none";
    }
}

static bool temp_valid(void)
{
    return s_temp_fail < TEMP_FAIL_MAX;
}

// 当前生效的故障：温度故障优先，其次 LED 驱动故障
static void fault_recompute(void)
{
    s_fault = s_fault_thermal != CTRL_FAULT_NONE ? s_fault_thermal : s_fault_driver;
}

static void set_fault(ctrl_fault_t f)
{
    if (s_fault == CTRL_FAULT_NONE) {
        led_pwm_off();
        ESP_LOGE(TAG, "FAULT: %s, LED_PWM off", fault_name(f));
    }
    if (f == CTRL_FAULT_DRIVER) {
        s_fault_driver = f;
    } else {
        s_fault_thermal = f;
    }
    fault_recompute();
    s_setpoint = 0;
}

static void thermal_update(void)
{
    if (s_sensor_ok) {
        float t;
        if (tmp102_read(&t) == ESP_OK) {
            s_temp = t;
            s_temp_fail = 0;
        } else if (s_temp_fail < TEMP_FAIL_MAX) {
            s_temp_fail++;
        }
    }

    // 保护关闭时只采样显示，不判定故障；占空比上限不受影响
    if (!s_protect) {
        return;
    }
    if (!temp_valid()) {
        set_fault(CTRL_FAULT_TEMP_SENSOR);
    } else if (s_temp >= s_shutdown_c) {
        // 只在这一轮刚进入过温时记一次：set_fault() 会把设定值清零
        if (!s_overtemp_hold) {
            s_overtemp_hold = true;
            s_hold_setpoint = s_setpoint;
            s_hold_start_ms = s_now;
            if (s_hold_setpoint) {
                ESP_LOGW(TAG, "overtemp %.1f C >= %u C, holding %u.%u%% until %u C",
                         s_temp, s_shutdown_c, s_hold_setpoint / 10, s_hold_setpoint % 10,
                         s_resume_c);
            }
        }
        set_fault(CTRL_FAULT_OVERTEMP);
    } else if (s_fault_thermal != CTRL_FAULT_NONE && s_temp < s_resume_c) {
        // 降到恢复温度以下：解除故障，过温的话把关断前的输出还回去
        ESP_LOGW(TAG, "fault cleared (%s)", fault_name(s_fault_thermal));
        s_fault_thermal = CTRL_FAULT_NONE;
        fault_recompute();
        if (s_overtemp_hold) {
            s_overtemp_hold = false;
            if (s_hold_setpoint) {      // 关断时本来就没输出（设定为 0）就不要自己开起来
                uint16_t limit = led_pwm_get_limit();
                s_setpoint = s_hold_setpoint > limit ? limit : s_hold_setpoint;
                if (s_timer_active) {
                    s_timer_end_ms += s_now - s_hold_start_ms;
                }
                ESP_LOGW(TAG, "overtemp recovery %.1f C, resume %u.%u%% (%s)",
                         s_temp, s_setpoint / 10, s_setpoint % 10,
                         s_timer_active ? "timer resumed" : "no timer");
            }
        }
    }
}

/*
 * LP8556 配置：VDD 来自 VBUS，每次 VBUS 上电寄存器都会回到默认 LED_EN = 0x3F。
 * 检测到 VBUS 后，在 PWM 保持为 0 至少 LP_QUIET_MS（器件进入 Standby）时写 LED_EN；
 * 写入成功前不允许输出。持续 LP_FAIL_MS 仍写不进去则锁存驱动故障。
 */
static void driver_update(uint32_t now_ms)
{
    // 带回差判断 VBUS 是否有电（只插下载口时约 0 V）。
    // ADC 不可用时无法判断，按“可能有电”处理：持续重试写入，成功前不允许输出，但不判故障。
    bool present = !s_vbus_ok || s_vbus_mv > (s_vbus_present ? LP_VBUS_OFF_MV : LP_VBUS_ON_MV);

    if (!present) {
        if (s_vbus_present) {
            ESP_LOGI(TAG, "VBUS removed, LP8556 unpowered");
        }
        s_vbus_present = false;
        s_lp_ok = false;
        s_lp_fail_since = 0;
        if (s_fault_driver != CTRL_FAULT_NONE) {
            s_fault_driver = CTRL_FAULT_NONE;
            fault_recompute();
        }
        return;
    }

    if (!s_vbus_present) {
        s_vbus_present = true;
        s_lp_ok = false;
        s_lp_fail_since = now_ms;
        ESP_LOGI(TAG, "VBUS present, configuring LP8556 LED_EN = 0x%02X", CONFIG_LP8556_LED_EN);
        // 器件刚上电即为 Active 时无法写配置：先停输出，设定清零，需重新开启
        if (s_setpoint || led_pwm_get()) {
            s_setpoint = 0;
            s_vbus_stop_event = true;
            ESP_LOGW(TAG, "output stopped for LP8556 configuration");
        }
        s_ramp_q = 0;
        led_pwm_off();
        s_t_pwm_low = now_ms;
    }
    if (s_lp_ok) {
        return;
    }

    if (led_pwm_get() != 0) {
        led_pwm_off();
        s_t_pwm_low = now_ms;
    }
    if (now_ms - s_t_pwm_low < LP_QUIET_MS || now_ms - s_t_lp_try < LP_RETRY_MS) {
        return;
    }
    s_t_lp_try = now_ms;

    if (s_lp_dev_ok && lp8556_set_led_enable(CONFIG_LP8556_LED_EN) == ESP_OK) {
        s_lp_ok = true;
        ESP_LOGI(TAG, "LP8556 LED_EN = 0x%02X written (%d strings)",
                 CONFIG_LP8556_LED_EN, __builtin_popcount(CONFIG_LP8556_LED_EN & 0x3F));
        if (s_fault_driver != CTRL_FAULT_NONE) {
            s_fault_driver = CTRL_FAULT_NONE;
            fault_recompute();
            ESP_LOGW(TAG, "fault cleared (%s)", fault_name(CTRL_FAULT_DRIVER));
        }
    } else if (s_vbus_ok && s_fault_driver == CTRL_FAULT_NONE && now_ms - s_lp_fail_since >= LP_FAIL_MS) {
        set_fault(CTRL_FAULT_DRIVER);
    }
}

// 非阻塞斜坡：每次调用按经过时间向设定值靠近，0 → 上限 用时 s_softstart_ms。
// 故障时立即关断，不走斜坡。
static void pwm_apply(uint32_t now_ms)
{
    uint32_t dt = now_ms - s_t_ramp;
    s_t_ramp = now_ms;

    // 故障，或功率输入已接入但 LP8556 尚未配置为 5 路：不输出
    if (s_fault != CTRL_FAULT_NONE || (s_vbus_present && !s_lp_ok)) {
        if (led_pwm_get() != 0) {
            led_pwm_off();
        }
        s_ramp_q = 0;
        return;
    }

    // s_ramp_q 为千分比 x 1000，避免小步进被截断
    int32_t target = (int32_t)s_setpoint * 1000;
    int32_t step = (int32_t)((uint64_t)led_pwm_get_limit() * 1000 * dt / (s_softstart_ms ? s_softstart_ms : 1));
    if (step < 1) {
        step = 1;
    }
    if (s_ramp_q < target) {
        s_ramp_q = s_ramp_q + step > target ? target : s_ramp_q + step;
    } else if (s_ramp_q > target) {
        s_ramp_q = s_ramp_q - step < target ? target : s_ramp_q - step;
    }
    led_pwm_set((uint16_t)(s_ramp_q / 1000));
}

// 阶梯测试：按固定节奏改设定值，仍受过温保护约束
static void demo_update(uint32_t now_ms)
{
    static const uint16_t steps[] = {50, 100, LED_PWM_PERMILLE_MAX, 0};
    static size_t idx;

    if (!DEMO_ENABLED || s_fault != CTRL_FAULT_NONE || now_ms - s_t_demo < DEMO_HOLD_MS) {
        return;
    }
    s_t_demo = now_ms;
    uint16_t sp = steps[idx];
    s_setpoint = sp > led_pwm_get_limit() ? led_pwm_get_limit() : sp;
    idx = (idx + 1) % (sizeof(steps) / sizeof(steps[0]));
}

// 只在颜色变化时发送，避免 50 ms 周期里重复占用 RMT
static void led_out(uint8_t r, uint8_t g, uint8_t b)
{
    if (s_led_valid && s_led_shown[0] == r && s_led_shown[1] == g && s_led_shown[2] == b) {
        return;
    }
    if (ws2812_set(r, g, b) == ESP_OK) {
        s_led_shown[0] = r;
        s_led_shown[1] = g;
        s_led_shown[2] = b;
        s_led_valid = true;
    }
}

// 优先级：故障黄闪 > PWM 输出红色（亮度随占空比）> 状态灯关闭 > 温度预警橙色 > 页面颜色。
// 故障与 PWM 指示不受状态灯开关影响：850 nm 输出肉眼不可见，必须有可见提示。
static void status_led_update(uint32_t now_ms)
{
    const uint8_t b = (uint16_t)s_led_brightness * 255 / 100;
    const uint16_t pwm = led_pwm_get();

    if (s_fault != CTRL_FAULT_NONE) {
        bool on = (now_ms / LED_BLINK_MS) % 2 == 0;
        led_out(on ? b : 0, on ? b * 2 / 3 : 0, 0);
    } else if (pwm > 0) {
        // 占空比 0~100% → 红色亮度 0~50%，最低 1 级保证可见
        uint32_t r = (uint32_t)pwm * LED_PWM_RED_MAX / LED_PWM_PERMILLE_MAX;
        led_out(r ? (uint8_t)r : 1, 0, 0);
    } else if (!s_led_enable) {
        led_out(0, 0, 0);
    } else if (temp_valid() && s_temp >= s_warn_c) {
        led_out(b, b / 4, 0);
    } else {
        led_out(s_idle_rgb[0] * b / 255, s_idle_rgb[1] * b / 255, s_idle_rgb[2] * b / 255);
    }
}

static void apply_vbus_mode(bool is_9v)
{
    uint16_t limit = (is_9v ? CONFIG_LED_PWM_LIMIT_9V_PERCENT : CONFIG_LED_PWM_LIMIT_5V_PERCENT) * 10;
    s_vbus_9v = is_9v;
    led_pwm_set_limit(limit);       // 上限下调时立即把输出压到上限
    if (s_setpoint > limit) {
        s_setpoint = limit;
    }
    if (s_ramp_q > (int32_t)limit * 1000) {
        s_ramp_q = (int32_t)limit * 1000;
    }
}

// 升到 9 V 需连续 VBUS_UP_COUNT 次判定；降到 5 V 或读失败立即生效
static void vbus_update(bool initial)
{
    int adc_mv, vbus_mv;
    bool ok = s_vbus_ok && vbus_read(&adc_mv, &vbus_mv) == ESP_OK;
    if (ok) {
        s_adc_mv = adc_mv;
        s_vbus_mv = vbus_mv;
    }

    bool want_9v = s_vbus_9v;
    if (!ok || vbus_mv < CONFIG_VBUS_9V_THRESHOLD_MV - CONFIG_VBUS_HYSTERESIS_MV) {
        want_9v = false;
    } else if (vbus_mv > CONFIG_VBUS_9V_THRESHOLD_MV + CONFIG_VBUS_HYSTERESIS_MV) {
        want_9v = true;
    }

    if (want_9v && !s_vbus_9v) {
        if (!initial && ++s_vbus_up_cnt < VBUS_UP_COUNT) {
            return;
        }
    }
    s_vbus_up_cnt = 0;
    if (initial || want_9v != s_vbus_9v) {
        apply_vbus_mode(want_9v);
        ESP_LOGI(TAG, "VBUS %s (ADC %d mV%s, VBUS ~%d mV), limit %u%%",
                 want_9v ? "9V" : "5V", s_adc_mv, vbus_adc_saturated(s_adc_mv) ? " saturated" : "",
                 s_vbus_mv, led_pwm_get_limit() / 10);
    }
}

void control_init(i2c_master_bus_handle_t bus, uint16_t tmp102_addr)
{
    s_tmp102_addr = tmp102_addr;
    s_sensor_ok = bus && tmp102_addr && tmp102_init(bus, tmp102_addr) == ESP_OK;
    if (!s_sensor_ok) {
        ESP_LOGE(TAG, "TMP102 not available, LED_PWM locked off");
    }

    s_lp_dev_ok = bus && lp8556_init(bus) == ESP_OK;
    if (!s_lp_dev_ok) {
        ESP_LOGE(TAG, "LP8556 I2C device not available, output blocked when VBUS present");
    }

    s_vbus_ok = vbus_init() == ESP_OK;
    if (!s_vbus_ok) {
        ESP_LOGE(TAG, "VBUS ADC not available, treated as 5V");
    }
    vbus_update(true);
}

static void timer_update(void)
{
    if (!s_timer_active) {
        return;
    }
    if (s_setpoint == 0 && !s_overtemp_hold) {
        // 停止、故障、离开运行页等任何原因归零都取消定时。
        // 过温关断例外：输出停着但定时只是暂停，降温恢复后接着走。
        s_timer_active = false;
        ESP_LOGI(TAG, "timer cancelled (output off)");
    } else if (s_setpoint != 0 && (int32_t)(s_now - s_timer_end_ms) >= 0) {
        s_timer_active = false;
        s_timer_expired = true;
        s_setpoint = 0;
        s_ramp_q = 0;
        led_pwm_off();
        ESP_LOGI(TAG, "timer expired, LED_PWM off");
    }
}

void control_update(uint32_t now_ms)
{
    s_now = now_ms;
    if (s_first) {
        s_t_ramp = now_ms;
        s_t_vbus = now_ms;
    }
    if (now_ms - s_t_vbus >= VBUS_PERIOD_MS) {
        s_t_vbus = now_ms;
        vbus_update(false);
    }
    if (s_first || now_ms - s_t_temp >= TEMP_PERIOD_MS) {
        s_t_temp = now_ms;
        thermal_update();
    }
    demo_update(now_ms);
    driver_update(now_ms);
    timer_update();
    pwm_apply(now_ms);
    if (s_first || now_ms - s_t_led >= LED_PERIOD_MS) {
        s_t_led = now_ms;
        status_led_update(now_ms);
    }
    s_first = false;
}

bool control_adjust(int delta_permille)
{
    if (DEMO_ENABLED || s_fault != CTRL_FAULT_NONE) {
        return false;
    }
    int sp = (int)s_setpoint + delta_permille;
    int limit = led_pwm_get_limit();
    if (sp < 0) {
        sp = 0;
    }
    if (sp > limit) {
        sp = limit;
    }
    if (sp != s_setpoint) {
        s_setpoint = (uint16_t)sp;
        ESP_LOGI(TAG, "setpoint %u.%u%%", s_setpoint / 10, s_setpoint % 10);
    }
    return true;
}

bool control_set_setpoint(uint16_t permille)
{
    if (DEMO_ENABLED || s_fault != CTRL_FAULT_NONE) {
        return false;
    }
    uint16_t limit = led_pwm_get_limit();
    s_setpoint = permille > limit ? limit : permille;
    ESP_LOGI(TAG, "setpoint %u.%u%% (remote)", s_setpoint / 10, s_setpoint % 10);
    return true;
}

bool control_set_timer_min(uint32_t minutes)
{
    if (minutes == 0) {
        if (s_timer_active) {
            s_timer_active = false;
            ESP_LOGI(TAG, "timer cancelled");
        }
        return true;
    }
    if (s_setpoint == 0 || s_fault != CTRL_FAULT_NONE) {
        return false;
    }
    s_timer_active = true;
    s_timer_end_ms = s_now + minutes * 60000U;
    ESP_LOGI(TAG, "timer %lu min", (unsigned long)minutes);
    return true;
}

bool control_take_vbus_stop(void)
{
    bool e = s_vbus_stop_event;
    s_vbus_stop_event = false;
    return e;
}

bool control_take_timer_expired(void)
{
    bool e = s_timer_expired;
    s_timer_expired = false;
    return e;
}

void control_get_status(ctrl_status_t *out)
{
    out->temp_valid = temp_valid();
    out->temp_c = s_temp;
    out->tmp102_addr = s_sensor_ok ? s_tmp102_addr : 0;
    out->pwm = led_pwm_get();
    out->setpoint = s_setpoint;
    out->limit = led_pwm_get_limit();
    out->fault = s_fault;
    out->warn = temp_valid() && s_temp >= s_warn_c;
    out->shutdown_c = s_shutdown_c;
    out->warn_c = s_warn_c;
    out->resume_c = s_resume_c;
    out->demo = DEMO_ENABLED;
    out->protect = s_protect;
    out->vbus_ok = s_vbus_ok;
    out->vbus_9v = s_vbus_9v;
    out->vbus_mv = s_vbus_mv;
    out->vbus_saturated = vbus_adc_saturated(s_adc_mv);
    out->driver = !s_vbus_present ? CTRL_DRV_NO_POWER
                  : s_lp_ok ? CTRL_DRV_OK
                  : s_fault_driver != CTRL_FAULT_NONE ? CTRL_DRV_FAIL : CTRL_DRV_PENDING;
    out->timer_active = s_timer_active;
    out->timer_left_s = 0;
    if (s_timer_active && (int32_t)(s_timer_end_ms - s_now) > 0) {
        // 向上取整，最后一秒显示 0:01 而不是 0:00
        out->timer_left_s = (s_timer_end_ms - s_now + 999) / 1000;
    }
}

void control_set_softstart_ms(uint32_t ms)
{
    s_softstart_ms = ms;
}

// 三档阈值的范围联动（见 control.h）：设置页按当前值算出可调范围，正常路径
// 传进来的值一定合法。这里只是兜底——万一范围是旧快照或者 NVS 里存过乱序值，
// 宁可把值顶到边界，也不能让 恢复 < 预警 < 关断 被破坏。
void control_set_shutdown_c(uint16_t c)
{
    // 预警上限 99，所以这里是 ≤ 100，不会越过 CONTROL_SHUTDOWN_C_MAX
    if (c < (uint16_t)(s_warn_c + 1)) {
        c = (uint16_t)(s_warn_c + 1);
    } else if (c > CONTROL_SHUTDOWN_C_MAX) {
        c = CONTROL_SHUTDOWN_C_MAX;
    }
    if (c == s_shutdown_c) {
        return;
    }
    s_shutdown_c = c;
    ESP_LOGI(TAG, "shutdown threshold %u C", c);
    thermal_update();   // 调低到当前温度以下时立即关断，不等下一个采样周期
}

void control_set_warn_c(uint16_t c)
{
    // 夹在恢复和关断之间。顺序没坏时 s_resume_c + 1 <= s_shutdown_c - 1 恒成立，
    // 所以这里不会算出空区间。
    if (c < (uint16_t)(s_resume_c + 1)) {
        c = (uint16_t)(s_resume_c + 1);
    } else if (c > (uint16_t)(s_shutdown_c - 1)) {
        c = (uint16_t)(s_shutdown_c - 1);
    }
    if (c == s_warn_c) {
        return;
    }
    s_warn_c = c;
    ESP_LOGI(TAG, "warn threshold %u C", c);
    // 只影响状态灯颜色和状态页显示，下一次 control_update 就会刷新
}

void control_set_resume_c(uint16_t c)
{
    // 必须低于预警。下限是 0（= 永不自动恢复），无符号参数不可能低于它，不用判。
    uint16_t hi = (uint16_t)(s_warn_c - 1);
    if (hi > CONTROL_RESUME_C_MAX) {
        hi = CONTROL_RESUME_C_MAX;
    }
    if (c > hi) {
        c = hi;
    }
    if (c == s_resume_c) {
        return;
    }
    s_resume_c = c;
    ESP_LOGI(TAG, "resume threshold %u C", c);
    thermal_update();   // 调高到当前温度以上时立即解除过温并恢复输出
}

void control_set_thermal_protect(bool enable)
{
    if (enable == s_protect) {
        return;
    }
    s_protect = enable;
    if (enable) {
        ESP_LOGI(TAG, "thermal protection ON");
        thermal_update();       // 立即重新判定，传感器无效或已过温则马上锁定
    } else {
        // 清除已锁存的温度故障（驱动故障不受影响）；亮度设定已在故障时清零，需重新按键开启
        s_fault_thermal = CTRL_FAULT_NONE;
        s_overtemp_hold = false;    // 保护都关了，不要再自动恢复输出
        fault_recompute();
        ESP_LOGW(TAG, "thermal protection OFF, limit stays %u.%u%%",
                 led_pwm_get_limit() / 10, led_pwm_get_limit() % 10);
    }
}

void control_force_off(void)
{
    if (s_setpoint || led_pwm_get()) {
        ESP_LOGI(TAG, "force off (was %u.%u%%)", led_pwm_get() / 10, led_pwm_get() % 10);
    }
    // 人主动停的（停止按钮、离开运行页）：取消待恢复的过温输出，别回来时自己开起来
    s_overtemp_hold = false;
    s_setpoint = 0;
    s_ramp_q = 0;
    led_pwm_off();
}

// 以下两项在下一次 control_update() 的状态灯刷新（≤50 ms）时生效
void control_set_status_led(bool enable, uint8_t brightness_percent)
{
    s_led_enable = enable;
    s_led_brightness = brightness_percent > 100 ? 100 : brightness_percent;
}

void control_set_idle_color(uint8_t r, uint8_t g, uint8_t b)
{
    s_idle_rgb[0] = r;
    s_idle_rgb[1] = g;
    s_idle_rgb[2] = b;
}
