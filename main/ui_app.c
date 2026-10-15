/*
 * KK_UI 应用页面
 *
 * 首页：运行（自定义页）/ 设置（菜单）/ 状态（信息页）/ 关于（信息页）
 * 运行页：右键（KEY_DOWN）增大、左键（KEY_UP）减小亮度设定，两键同按返回。
 * 本板 KEY_UP 在左、KEY_DOWN 在右，所有加减、开关、左右选择都按物理位置：右 = 增大/开/确定。
 * 设置项（含温度保护开关）掉电保存到 NVS；保护为关闭时上电弹出提示。
 * 离开运行页时 LED_PWM 立即归零。
 *
 * 注意：界面文字修改后需运行 tools/gen_ui_fonts.py 重新生成字模。
 */
#include "ui_app.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_app_desc.h"
#include "esp_system.h"
#include "kk_oled.h"
#include "kk_ui_draw.h"
#include "sdkconfig.h"
#include "control.h"
#include "settings.h"
#include "ui_fonts.h"
#include "ui_icons.h"

#define INFO_REFRESH_MS     500

enum {
    PAGE_HOME = 1,
    PAGE_RUN,
    PAGE_SETTINGS,
    PAGE_STATUS,
    PAGE_ABOUT,
};

enum {
    EVT_SOFTSTART_CHANGED = 1,
    EVT_STEP_CHANGED,
    EVT_LED_CHANGED,
    EVT_RESET_CONFIRMED,
    EVT_RESET_CANCELLED,
    EVT_PROTECT_CHANGED,
};

enum { INT_SOFTSTART, INT_STEP, INT_LED_BRIGHTNESS };
enum { BOOL_PROTECT, BOOL_LED };
enum { CONFIRM_RESET };

/* ---------- 设置 ---------- */

static int32_t s_softstart_ms;
static int32_t s_step_percent;
static int32_t s_led_brightness;
static bool s_led_enable;
static bool s_protect;

static void cfg_defaults(void)
{
    s_softstart_ms = CONFIG_LED_PWM_SOFTSTART_MS;
    s_step_percent = 1;
    s_led_brightness = CONFIG_WS2812_BRIGHTNESS_PERCENT;
    s_led_enable = true;
    s_protect = true;
}

static int32_t in_range(int32_t v, int32_t lo, int32_t hi, int32_t def)
{
    return v < lo || v > hi ? def : v;
}

// 读出的值超出编辑范围时回到默认，避免 KK_UI_Init() 校验失败
static void cfg_load(void)
{
    app_settings_t s = {s_softstart_ms, s_step_percent, s_led_brightness, s_led_enable, s_protect};
    settings_load(&s);
    s_softstart_ms = in_range(s.softstart_ms, 100, 2000, s_softstart_ms);
    s_step_percent = in_range(s.step_percent, 1, 10, s_step_percent);
    s_led_brightness = in_range(s.led_brightness, 5, 100, s_led_brightness);
    s_led_enable = s.led_enable;
    s_protect = s.protect;
}

static bool cfg_save(void)
{
    const app_settings_t s = {s_softstart_ms, s_step_percent, s_led_brightness, s_led_enable, s_protect};
    return settings_save(&s) == ESP_OK;
}

static void cfg_apply(void)
{
    control_set_softstart_ms((uint32_t)s_softstart_ms);
    control_set_status_led(s_led_enable, (uint8_t)s_led_brightness);
    control_set_thermal_protect(s_protect);
}

/* ---------- 动态文本 ---------- */

static char s_txt_temp[16];
static char s_txt_pwm[16];
static char s_txt_set[16];
static char s_txt_limit[16];
static char s_txt_vbus[16];
static char s_txt_vbus_mv[16];
static char s_txt_ssid[33];
static char s_txt_pwd[65];
static char s_txt_fault[24];
static char s_txt_sensor[16];
static char s_txt_driver[16];
static char s_txt_uptime[16];
static char s_txt_fw[32];
static char s_txt_idf[24];

static ctrl_status_t s_st;
static ctrl_fault_t s_last_fault = CTRL_FAULT_NONE;
static bool s_last_9v;
static const char *s_pending_toast;
static uint32_t s_info_at;
static uint32_t s_started_at;

// 千分比格式化为 "12.3%"，整数时省略小数
static void fmt_permille(char *buf, size_t len, uint16_t v)
{
    if (v % 10 == 0) {
        snprintf(buf, len, "%u%%", v / 10);
    } else {
        snprintf(buf, len, "%u.%u%%", v / 10, v % 10);
    }
}

static void fmt_temp(char *buf, size_t len, const ctrl_status_t *st)
{
    if (!st->temp_valid) {
        snprintf(buf, len, "--");
        return;
    }
    int t10 = (int)(st->temp_c * 10 + (st->temp_c >= 0 ? 0.5f : -0.5f));
    snprintf(buf, len, "%s%d.%d℃", t10 < 0 ? "-" : "", abs(t10) / 10, abs(t10) % 10);
}

static const char *fault_text(const ctrl_status_t *st)
{
    switch (st->fault) {
    case CTRL_FAULT_OVERTEMP:    return "过温关断";
    case CTRL_FAULT_TEMP_SENSOR: return "传感器故障";
    case CTRL_FAULT_DRIVER:      return "驱动故障";
    default:                     return !st->protect ? "已关闭" : st->warn ? "温度预警" : "正常";
    }
}

static void refresh_texts(uint32_t now_ms)
{
    control_get_status(&s_st);
    fmt_temp(s_txt_temp, sizeof(s_txt_temp), &s_st);
    fmt_permille(s_txt_pwm, sizeof(s_txt_pwm), s_st.pwm);
    fmt_permille(s_txt_set, sizeof(s_txt_set), s_st.setpoint);
    fmt_permille(s_txt_limit, sizeof(s_txt_limit), s_st.limit);
    snprintf(s_txt_vbus, sizeof(s_txt_vbus), "%s", !s_st.vbus_ok ? "--" : s_st.vbus_9v ? "9V" : "5V");
    if (!s_st.vbus_ok) {
        snprintf(s_txt_vbus_mv, sizeof(s_txt_vbus_mv), "--");
    } else {
        // ADC 饱和时只能给出下限
        snprintf(s_txt_vbus_mv, sizeof(s_txt_vbus_mv), "%s%d.%dV", s_st.vbus_saturated ? ">" : "",
                 s_st.vbus_mv / 1000, s_st.vbus_mv % 1000 / 100);
    }
    snprintf(s_txt_fault, sizeof(s_txt_fault), "%s", fault_text(&s_st));
    {
        static const char *drv[] = {
            [CTRL_DRV_NO_POWER] = "无供电", [CTRL_DRV_PENDING] = "配置中",
            [CTRL_DRV_OK] = "已配置", [CTRL_DRV_FAIL] = "配置失败",
        };
        if (s_st.driver == CTRL_DRV_OK) {
            snprintf(s_txt_driver, sizeof(s_txt_driver), "%d路", __builtin_popcount(CONFIG_LP8556_LED_EN & 0x3F));
        } else {
            snprintf(s_txt_driver, sizeof(s_txt_driver), "%s", drv[s_st.driver]);
        }
    }
    if (s_st.tmp102_addr) {
        snprintf(s_txt_sensor, sizeof(s_txt_sensor), "0x%02X", s_st.tmp102_addr);
    } else {
        snprintf(s_txt_sensor, sizeof(s_txt_sensor), "未找到");
    }
    snprintf(s_txt_uptime, sizeof(s_txt_uptime), "%lus", (unsigned long)((now_ms - s_started_at) / 1000));
    // Wi-Fi 延时启动，热点信息随状态刷新
    snprintf(s_txt_ssid, sizeof(s_txt_ssid), "%s",
             remote_is_running() ? remote_ssid() : remote_is_starting() ? "启动中" : "未开启");
    snprintf(s_txt_pwd, sizeof(s_txt_pwd), "%s", remote_is_running() ? remote_password() : "--");
}

/* ---------- 页面定义 ---------- */

static const KK_UI_HomeItem home_items[] = {
    {"运行", home_icons[2], PAGE_RUN},
    {"设置", home_icons[0], PAGE_SETTINGS},
    {"状态", home_icons[1], PAGE_STATUS},
    {"关于", home_icons[3], PAGE_ABOUT},
};

static const KK_UI_MenuItem settings_items[] = {
    {"温度保护", KK_UI_MENU_BOOL, BOOL_PROTECT},
    {"软启动", KK_UI_MENU_INT, INT_SOFTSTART},
    {"调光步进", KK_UI_MENU_INT, INT_STEP},
    {"状态灯", KK_UI_MENU_BOOL, BOOL_LED},
    {"状态灯亮度", KK_UI_MENU_INT, INT_LED_BRIGHTNESS},
    {"恢复默认", KK_UI_MENU_CONFIRM, CONFIRM_RESET},
};

static const KK_UI_InfoRow status_rows[] = {
    {"温度", s_txt_temp},
    {"输出", s_txt_pwm},
    {"设定", s_txt_set},
    {"上限", s_txt_limit},
    {"VBUS档位", s_txt_vbus},
    {"VBUS电压", s_txt_vbus_mv},
    {"保护", s_txt_fault},
    {"温度传感器", s_txt_sensor},
    {"LED驱动", s_txt_driver},
    {"运行时间", s_txt_uptime},
};

static const KK_UI_InfoRow about_rows[] = {
    {"设备", "850nm控制板"},
    {"固件", s_txt_fw},
    {"ESP-IDF", s_txt_idf},
    {"屏幕", "160x80 ST7735S"},
    {"界面", "KK_UI"},
    {"左键", "上一项/减小"},
    {"右键", "下一项/增大"},
    {"两键同按", "确定/返回"},
    {"WiFi", s_txt_ssid},
    {"密码", s_txt_pwd},
    {"网址", "192.168.4.1"},
    {"设置保存", "掉电保存"},
    {"离开运行页", "输出归零"},
};

static const KK_UI_HomePage home_pages[] = {
    {home_items, sizeof(home_items) / sizeof(home_items[0])},
};

static const KK_UI_MenuPage menu_pages[] = {
    {"设置", settings_items, NULL, sizeof(settings_items) / sizeof(settings_items[0])},
};

static const KK_UI_InfoPage info_pages[] = {
    {"状态", status_rows, sizeof(status_rows) / sizeof(status_rows[0])},
    {"关于", about_rows, sizeof(about_rows) / sizeof(about_rows[0])},
};

static const KK_UI_IntBinding int_bindings[] = {
    [INT_SOFTSTART] = {"软启动时间", &s_softstart_ms, 100, 2000, 50U, "ms", EVT_SOFTSTART_CHANGED},
    [INT_STEP] = {"调光步进", &s_step_percent, 1, 10, 1U, "%", EVT_STEP_CHANGED},
    [INT_LED_BRIGHTNESS] = {"状态灯亮度", &s_led_brightness, 5, 100, 5U, "%", EVT_LED_CHANGED},
};

static const KK_UI_BoolBinding bool_bindings[] = {
    [BOOL_PROTECT] = {"温度保护", &s_protect, EVT_PROTECT_CHANGED},
    [BOOL_LED] = {"状态灯", &s_led_enable, EVT_LED_CHANGED},
};

static const KK_UI_ConfirmDesc confirm_descs[] = {
    [CONFIRM_RESET] = {"恢复默认设置？\n温度保护将开启", EVT_RESET_CONFIRMED, EVT_RESET_CANCELLED},
};

// 下标 = 页面 ID - 1
static const KK_UI_PageRoute routes[] = {
    {KK_UI_PAGE_HOME, 0U},
    {KK_UI_PAGE_CUSTOM, 0U},
    {KK_UI_PAGE_MENU, 0U},
    {KK_UI_PAGE_INFO, 0U},
    {KK_UI_PAGE_INFO, 1U},
};

static const KK_UI_App s_app = {
    .root_page = PAGE_HOME,
    .routes = routes,
    .route_count = sizeof(routes) / sizeof(routes[0]),
    .home_pages = home_pages,
    .home_page_count = sizeof(home_pages) / sizeof(home_pages[0]),
    .menu_pages = menu_pages,
    .menu_page_count = sizeof(menu_pages) / sizeof(menu_pages[0]),
    .info_pages = info_pages,
    .info_page_count = sizeof(info_pages) / sizeof(info_pages[0]),
    .custom_page_count = 1U,
    .int_bindings = int_bindings,
    .int_binding_count = sizeof(int_bindings) / sizeof(int_bindings[0]),
    .bool_bindings = bool_bindings,
    .bool_binding_count = sizeof(bool_bindings) / sizeof(bool_bindings[0]),
    .confirm_descs = confirm_descs,
    .confirm_desc_count = sizeof(confirm_descs) / sizeof(confirm_descs[0]),
    .fonts = {ui_font_home, ui_font_title, ui_font_body},
    .texts = {"返回", "取消", "确定", "开", "关", "提示"},
};

/* ---------- 公共接口 ---------- */

const KK_UI_App *ui_app_get(void)
{
    return &s_app;
}

void ui_app_reset(uint32_t now_ms)
{
    cfg_defaults();
    cfg_load();
    cfg_apply();
    snprintf(s_txt_fw, sizeof(s_txt_fw), "%s", esp_app_get_description()->version);
    snprintf(s_txt_idf, sizeof(s_txt_idf), "%s", esp_get_idf_version());
    s_started_at = now_ms;
    s_info_at = now_ms;
    s_last_fault = CTRL_FAULT_NONE;
    // 保存的设置为保护关闭时，上电提示（在首次 KK_UI_Update 之后显示）
    s_pending_toast = s_protect ? NULL : "温度保护已关闭";
    refresh_texts(now_ms);
    s_last_9v = s_st.vbus_9v;   // 上电档位不提示，只提示之后的变化
}

// 各页面的状态灯颜色（满亮度，实际按状态灯亮度缩放）
static void page_led_color(KK_UI_PageId page)
{
    switch (page) {
    case PAGE_RUN:      control_set_idle_color(0, 255, 0);     break;  // 绿：待机，可输出
    case PAGE_SETTINGS: control_set_idle_color(160, 0, 255);   break;  // 紫
    case PAGE_STATUS:   control_set_idle_color(0, 200, 255);   break;  // 青
    case PAGE_ABOUT:    control_set_idle_color(255, 255, 255); break;  // 白
    default:            control_set_idle_color(0, 0, 255);     break;  // 蓝：首页
    }
}

void ui_app_update(uint32_t now_ms)
{
    KK_UI_PageId page = KK_UI_GetCurrentPage();
    page_led_color(page);

    // 只有运行页允许输出；切出运行页（含转场动画期间）一律立即归零
    control_get_status(&s_st);
    if (page != PAGE_RUN && !s_st.demo && (s_st.setpoint || s_st.pwm)) {
        control_force_off();
    }

    if (now_ms - s_info_at >= INFO_REFRESH_MS) {
        s_info_at = now_ms;
        refresh_texts(now_ms);
        KK_UI_Invalidate();     // 差分刷新，内容未变时不产生 SPI 传输
    }
}

static void show_toast(const char *text)
{
    // 弹框打开期间 KK_UI 返回 BUSY，留到下次再显示
    s_pending_toast = KK_UI_ShowToast(text, 0U) == KK_UI_BUSY ? text : NULL;
}

static const char *fault_toast(ctrl_fault_t f)
{
    return f == CTRL_FAULT_OVERTEMP ? "过温关断" : f == CTRL_FAULT_DRIVER ? "LED驱动配置失败" : "温度传感器故障";
}

// 设置变更后同步故障状态：变更后已锁定则提示故障原因，否则提示 text，
// 避免下一轮再额外弹出“保护已解除”之类的提示
static void sync_fault_toast(const char *text)
{
    control_get_status(&s_st);
    s_last_fault = s_st.fault;
    show_toast(s_st.fault != CTRL_FAULT_NONE ? fault_toast(s_st.fault) : text);
}

void ui_app_process_events(void)
{
    KK_UI_EventId evt;

    while (KK_UI_PollEvent(&evt)) {
        switch (evt) {
        case EVT_SOFTSTART_CHANGED:
        case EVT_STEP_CHANGED:
        case EVT_LED_CHANGED:
            cfg_apply();
            show_toast(cfg_save() ? "设置已保存" : "保存失败");
            break;
        case EVT_RESET_CONFIRMED:
            cfg_defaults();
            cfg_apply();
            KK_UI_Invalidate();
            sync_fault_toast(cfg_save() ? "已恢复默认" : "保存失败");
            break;
        case EVT_PROTECT_CHANGED:
            cfg_apply();
            sync_fault_toast(!cfg_save() ? "保存失败" : s_protect ? "温度保护已开启" : "温度保护已关闭");
            break;
        default:
            break;
        }
    }

    // 故障状态变化提示，任何页面都显示
    control_get_status(&s_st);
    if (s_st.fault != s_last_fault) {
        show_toast(s_st.fault != CTRL_FAULT_NONE ? fault_toast(s_st.fault) : "保护已解除");
        s_last_fault = s_st.fault;
    } else if (control_take_vbus_stop()) {
        show_toast("功率接入 已停止输出");
    } else if (control_take_timer_expired()) {
        show_toast("定时结束 已停止");
    } else if (s_st.vbus_9v != s_last_9v) {
        s_last_9v = s_st.vbus_9v;
        static char txt[24];
        snprintf(txt, sizeof(txt), "%s 上限%u%%", s_st.vbus_9v ? "9V" : "5V", s_st.limit / 10);
        show_toast(txt);
        KK_UI_Invalidate();
    } else if (s_pending_toast) {
        show_toast(s_pending_toast);
    }
}

/* ---------- 运行页（自定义页） ---------- */

static ctrl_status_t s_run_shown;

// 倒计时 "12:34"，超过 1 小时 "1:02:03"
static void fmt_countdown(char *buf, size_t len, uint32_t s)
{
    if (s >= 3600) {
        snprintf(buf, len, "%lu:%02lu:%02lu", (unsigned long)(s / 3600),
                 (unsigned long)(s / 60 % 60), (unsigned long)(s % 60));
    } else {
        snprintf(buf, len, "%lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
    }
}

static const char *run_tag(const ctrl_status_t *st)
{
    static char buf[40];

    if (st->fault == CTRL_FAULT_OVERTEMP) {
        return "过温关断";
    }
    if (st->fault == CTRL_FAULT_TEMP_SENSOR) {
        return "传感器故障";
    }
    if (st->fault == CTRL_FAULT_DRIVER) {
        return "驱动故障";
    }
    if (st->driver == CTRL_DRV_PENDING) {
        return "驱动配置中";
    }
    if (st->demo) {
        return "阶梯测试";
    }
    if (st->timer_active) {
        // 保护关闭时由左侧档位后的 "!" 提示，这里只显示倒计时
        char t[16];
        fmt_countdown(t, sizeof(t), st->timer_left_s);
        snprintf(buf, sizeof(buf), "定时 %s", t);
        return buf;
    }
    if (!st->protect) {
        return "保护关闭";
    }
    if (st->warn) {
        return "温度预警";
    }
    return st->pwm ? "输出中" : "待机";
}

void KK_UI_CustomOnEnter(KK_UI_PageId page)
{
    (void)page;
    control_get_status(&s_run_shown);
}

void KK_UI_CustomOnLeave(KK_UI_PageId page)
{
    (void)page;
    if (!s_run_shown.demo) {
        control_force_off();
    }
}

void KK_UI_CustomOnInput(KK_UI_PageId page, KK_UI_InputEvent event)
{
    (void)page;
    if (event.action == KK_UI_INPUT_OK) {
        // 按下返回即归零，不等转场动画结束
        if (!s_run_shown.demo) {
            control_force_off();
        }
        (void)KK_UI_CustomRequestClose();
        return;
    }

    int delta = (int)s_step_percent * 10 * event.steps;
    if (event.action == KK_UI_INPUT_UP) {     // 左键减小
        delta = -delta;
    }
    if (!control_adjust(delta)) {
        (void)KK_UI_ShowToast(s_run_shown.demo ? "阶梯测试中" : "保护锁定中", 0U);
    }
    KK_UI_Invalidate();
}

bool KK_UI_CustomOnTick(KK_UI_PageId page, uint32_t now_ms)
{
    ctrl_status_t st;
    (void)page;
    (void)now_ms;

    control_get_status(&st);
    // 温度只按 0.1 ℃ 比较，避免浮点抖动引起无谓重绘
    bool changed = st.pwm != s_run_shown.pwm || st.setpoint != s_run_shown.setpoint ||
                   st.fault != s_run_shown.fault || st.warn != s_run_shown.warn ||
                   st.vbus_9v != s_run_shown.vbus_9v || st.limit != s_run_shown.limit ||
                   st.timer_active != s_run_shown.timer_active ||
                   st.timer_left_s != s_run_shown.timer_left_s ||
                   st.temp_valid != s_run_shown.temp_valid ||
                   (int)(st.temp_c * 10) != (int)(s_run_shown.temp_c * 10);
    s_run_shown = st;
    return changed;
}

static int16_t text_height(void)
{
    return (int16_t)(OLED_GetFontAscent() - OLED_GetFontDescent());
}

void KK_UI_CustomOnDraw(KK_UI_PageId page, int16_t x_offset, int16_t clip_x, uint16_t clip_width)
{
    const ctrl_status_t *st = &s_run_shown;
    char buf[40], buf2[24];
    const int16_t W = (int16_t)OLED_GetWidth();
    (void)page;

    OLED_SetClipWindow(clip_x, 0, clip_width, OLED_GetHeight());

    // 标题栏
    OLED_SetFont(ui_font_title);
    OLED_DrawUTF8((int16_t)(4 + x_offset), 1,
                  st->vbus_9v ? (st->protect ? "运行 9V" : "运行 9V!") : (st->protect ? "运行 5V" : "运行 5V!"));
    const char *tag = run_tag(st);
    OLED_DrawUTF8((int16_t)(W - 4 - OLED_GetUTF8Width(tag) + x_offset), 1, tag);
    OLED_DrawHLine(x_offset, 14, W);

    if (st->fault != CTRL_FAULT_NONE) {
        // 故障画面：原因 + 恢复提示
        OLED_SetFont(ui_font_body);
        const char *l1 = st->fault == CTRL_FAULT_OVERTEMP ? "过温关断"
                       : st->fault == CTRL_FAULT_DRIVER ? "LED驱动配置失败" : "读不到温度传感器";
        const char *l2 = st->fault == CTRL_FAULT_OVERTEMP ? "降温后按右键开启"
                       : st->fault == CTRL_FAULT_DRIVER ? "检查LP8556与I2C" : "检查TMP102与I2C";
        OLED_DrawUTF8((int16_t)((W - OLED_GetUTF8Width(l1)) / 2 + x_offset), 22, l1);
        OLED_DrawUTF8((int16_t)((W - OLED_GetUTF8Width(l2)) / 2 + x_offset), 40, l2);
    } else {
        // 大号输出值
        OLED_SetFont(ui_font_big);
        fmt_permille(buf, sizeof(buf), st->pwm);
        int16_t h = text_height();
        int16_t y = (int16_t)(15 + (31 - h) / 2);
        OLED_DrawUTF8((int16_t)((W - OLED_GetUTF8Width(buf)) / 2 + x_offset), y, buf);

        // 输出条：满量程 100%，竖线标出当前上限
        KK_UI_DrawProgressBar((int16_t)(6 + x_offset), 47, W - 12, 7, st->pwm, 1000);
        int16_t lx = (int16_t)(6 + (int32_t)(W - 12) * st->limit / 1000 + x_offset);
        OLED_DrawVLine(lx, 45, 11);
    }

    // 底部：设定值与温度
    OLED_SetFont(ui_font_body);
    fmt_permille(buf2, sizeof(buf2), st->setpoint);
    snprintf(buf, sizeof(buf), "设定 %s", buf2);
    OLED_DrawUTF8((int16_t)(4 + x_offset), 63, buf);
    fmt_temp(buf2, sizeof(buf2), st);
    OLED_DrawUTF8((int16_t)(W - 4 - OLED_GetUTF8Width(buf2) + x_offset), 63, buf2);

    OLED_ResetClipWindow();
}

/* ---------- 手机遥控 ---------- */

// 回到首页后进入运行页；已在运行页时不动
static bool goto_run_page(uint32_t now_ms)
{
    if (KK_UI_GetCurrentPage() == PAGE_RUN) {
        return true;
    }
    // 重新初始化以关闭弹框、清空导航栈，再从首页进入运行页
    if (KK_UI_Init(&s_app) != KK_UI_OK) {
        return false;
    }
    return KK_UI_GoTo(PAGE_RUN, now_ms) == KK_UI_OK;
}

remote_result_t ui_app_remote_cmd(const remote_cmd_t *cmd, uint32_t now_ms)
{
    control_get_status(&s_st);

    switch (cmd->kind) {
    case REMOTE_CMD_STOP:
        control_force_off();
        return REMOTE_OK;

    case REMOTE_CMD_DUTY:
        if (cmd->permille == 0) {
            control_force_off();
            return REMOTE_OK;
        }
        if (s_st.demo || s_st.fault != CTRL_FAULT_NONE) {
            return REMOTE_ERR_LOCKED;
        }
        // 只有运行页允许输出：先进入运行页，再设定
        if (!goto_run_page(now_ms)) {
            return REMOTE_ERR_BUSY;
        }
        return control_set_setpoint(cmd->permille) ? REMOTE_OK : REMOTE_ERR_LOCKED;

    case REMOTE_CMD_TIMER:
        if (cmd->minutes == 0) {
            control_set_timer_min(0);
            return REMOTE_OK;
        }
        if (s_st.fault != CTRL_FAULT_NONE) {
            return REMOTE_ERR_LOCKED;
        }
        if (!control_set_timer_min(cmd->minutes)) {
            return REMOTE_ERR_NOT_RUNNING;
        }
        {
            static char txt[24];
            snprintf(txt, sizeof(txt), "定时 %u 分钟", cmd->minutes);
            show_toast(txt);
        }
        return REMOTE_OK;
    }
    return REMOTE_ERR_BUSY;
}

void ui_app_remote_status(remote_status_t *out)
{
    ctrl_status_t st;
    control_get_status(&st);
    *out = (remote_status_t){
        .pwm = st.pwm,
        .setpoint = st.setpoint,
        .limit = st.limit,
        .vbus_ok = st.vbus_ok,
        .vbus_9v = st.vbus_9v,
        .vbus_mv = st.vbus_mv,
        .vbus_saturated = st.vbus_saturated,
        .temp_valid = st.temp_valid,
        .temp_c = st.temp_c,
        .fault = (uint8_t)st.fault,
        .warn = st.warn,
        .protect = st.protect,
        .demo = st.demo,
        .driver = (uint8_t)st.driver,
        .timer_active = st.timer_active,
        .timer_left_s = st.timer_left_s,
        .on_run_page = KK_UI_GetCurrentPage() == PAGE_RUN,
    };
}
