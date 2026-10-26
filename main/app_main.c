/*
 * 850 nm 红外 LED 控制验证板固件
 *
 * 当前阶段：
 *   1. USB Serial/JTAG + UART0 日志
 *   2. GPIO1 LED_PWM（LEDC，非阻塞软启动，占空比上限）
 *   3. TMP102 温度读取与过温关断
 *   4. TFT（KK_OLED + KK_UI）、两键操作、WS2812 状态灯
 *
 * 上电后 LED_PWM 保持 0%，只有在“运行”页按 KEY_UP 才会开启。
 * 阶段 5：VBUS 判档，5 V 上限 CONFIG_LED_PWM_LIMIT_5V_PERCENT，9 V 上限 CONFIG_LED_PWM_LIMIT_9V_PERCENT。
 */
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "kk_oled.h"
#include "kk_ui.h"
#include "sdkconfig.h"
#include "board_pins.h"
#include "control.h"
#include "led_pwm.h"
#include "lp8556.h"
#include "ota.h"
#include "remote.h"
#include "settings.h"
#include "tmp102.h"
#include "ui_app.h"
#include "ui_fonts.h"
#include "ui_input.h"
#include "ws2812.h"

#define LOOP_MS             5       // KK_UI 要求 ≤10 ms 调用一次
#define HEARTBEAT_MS        5000
#define TEST_PATTERN_MS     1500
#define DISPLAY_RETRY_MS    1000
#define REMOTE_PUBLISH_MS   100
// OTA 上去的固件要在这段时间内证明自己没崩，否则下次上电自动退回旧版本。
// 取 5 秒：够覆盖外设初始化和主循环头几轮，又短到不会因为随手断电而误判。
#define OTA_CONFIRM_MS      5000

static const char *TAG = "app";

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static const char *reset_reason_str(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON:   return "power-on";
    case ESP_RST_SW:        return "software";
    case ESP_RST_PANIC:     return "panic";
    case ESP_RST_INT_WDT:   return "interrupt watchdog";
    case ESP_RST_TASK_WDT:  return "task watchdog";
    case ESP_RST_WDT:       return "other watchdog";
    case ESP_RST_BROWNOUT:  return "brownout";
    case ESP_RST_USB:       return "USB";
    case ESP_RST_JTAG:      return "JTAG";
    default:                return "other";
    }
}

static void log_boot_info(void)
{
    esp_chip_info_t chip;
    uint32_t flash_size = 0;

    esp_chip_info(&chip);
    esp_flash_get_size(NULL, &flash_size);

    ESP_LOGI(TAG, "850nm LED controller, ESP-IDF %s", esp_get_idf_version());
    ESP_LOGI(TAG, "%s rev v%d.%d, %d core(s), flash %" PRIu32 " MB",
             CONFIG_IDF_TARGET, chip.revision / 100, chip.revision % 100,
             chip.cores, flash_size / (1024 * 1024));
    ESP_LOGI(TAG, "reset reason: %s", reset_reason_str(esp_reset_reason()));
}

/* ---------- I2C ---------- */

// 空闲时 SDA/SCL 应被上拉为高；为低说明短路、上拉缺失或有器件拉住总线
static void i2c_check_idle_levels(void)
{
    const gpio_config_t in = {
        .pin_bit_mask = (1ULL << PIN_I2C_SDA) | (1ULL << PIN_I2C_SCL),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&in);
    vTaskDelay(pdMS_TO_TICKS(2));
    int sda = gpio_get_level(PIN_I2C_SDA);
    int scl = gpio_get_level(PIN_I2C_SCL);
    if (sda && scl) {
        ESP_LOGI(TAG, "I2C idle: SDA=1 SCL=1 (external pull-ups OK)");
    } else {
        ESP_LOGE(TAG, "I2C idle: SDA=%d SCL=%d, line held low or pull-up missing", sda, scl);
    }
}

static i2c_master_bus_handle_t i2c_bus_init(void)
{
    i2c_check_idle_levels();

    const i2c_master_bus_config_t cfg = {
        .i2c_port = -1,
        .sda_io_num = PIN_I2C_SDA,
        .scl_io_num = PIN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,  // 与板上 4.7 kΩ 外部上拉并联
    };
    i2c_master_bus_handle_t bus = NULL;
    if (i2c_new_master_bus(&cfg, &bus) != ESP_OK) {
        ESP_LOGE(TAG, "I2C bus init failed");
        return NULL;
    }
    return bus;
}

// 扫描并打印所有应答地址，返回找到的 TMP102 地址，未找到返回 0。
// 只做地址应答探测；三颗 LP8556 同地址，禁止读其寄存器。
static uint16_t i2c_scan(i2c_master_bus_handle_t bus)
{
    char list[96] = "";
    size_t n = 0;
    uint16_t tmp102 = 0;
    bool lp8556 = false;

    for (uint16_t a = 0x08; a <= 0x77 && n < sizeof(list); a++) {
        if (i2c_master_probe(bus, a, 20) != ESP_OK) {
            continue;
        }
        n += snprintf(list + n, sizeof(list) - n, " 0x%02X", a);
        if (!tmp102 && a >= TMP102_ADDR_MIN && a <= TMP102_ADDR_MAX) {
            tmp102 = a;
        }
        if (a == LP8556_ADDR) {
            lp8556 = true;
        }
    }
    ESP_LOGI(TAG, "I2C scan:%s", n ? list : " no device");
    ESP_LOGI(TAG, "LP8556 0x%02X %s", LP8556_ADDR, lp8556 ? "ACK" : "no ACK (normal without VBUS)");
    return tmp102;
}

/* ---------- 显示 ---------- */

#ifdef CONFIG_UI_ENABLE
// 边框 + 对角线 + 尺寸文字：用于确认偏移、方向和剪切
static void draw_test_pattern(void)
{
    const int16_t w = (int16_t)OLED_GetWidth();
    const int16_t h = (int16_t)OLED_GetHeight();

    OLED_Clear();
    OLED_DrawFrame(0, 0, (uint16_t)w, (uint16_t)h);
    OLED_DrawLine(0, 0, (int16_t)(w - 1), (int16_t)(h - 1));
    OLED_DrawLine(0, (int16_t)(h - 1), (int16_t)(w - 1), 0);
    OLED_DrawBox(2, 2, 6, 6);   // 左上角标记
    OLED_SetFont(ui_font_body);
    OLED_SetFontPosition(OLED_FONT_POS_TOP);
    OLED_SetBackgroundMode(OLED_BG_SOLID);
    const char *label = "160x80";
    OLED_DrawUTF8((int16_t)((w - OLED_GetUTF8Width(label)) / 2), 10, label);
    OLED_SetBackgroundMode(OLED_BG_TRANSPARENT);
    OLED_Update();
}

static bool display_init(void)
{
    if (OLED_Init() != OLED_OK) {
        ESP_LOGE(TAG, "TFT init failed");
        return false;
    }
    // 面板原生竖屏 80x160，软件旋转成 160x80 横屏
#ifdef CONFIG_TFT_ROTATE_180
    OLED_SetRotation(OLED_ROTATION_270);
#else
    OLED_SetRotation(OLED_ROTATION_90);
#endif
    ESP_LOGI(TAG, "TFT logical %ux%u", OLED_GetWidth(), OLED_GetHeight());

    draw_test_pattern();
    vTaskDelay(pdMS_TO_TICKS(TEST_PATTERN_MS));
    return true;
}
#endif  // CONFIG_UI_ENABLE

/* ---------- 主流程 ---------- */

void app_main(void)
{
    log_boot_info();
    ota_report_boot();      // 跑在哪个分区、OTA 后有没有回过滚，日志里能看出来

    // 最先把 PWM 初始化为 0%
    ESP_ERROR_CHECK(led_pwm_init());

    if (ws2812_init() != ESP_OK) {
        ESP_LOGE(TAG, "WS2812 init failed");
    }
    ESP_ERROR_CHECK(ui_input_init());
    settings_init();    // 失败时设置只保存在 RAM，不影响运行

    i2c_master_bus_handle_t bus = i2c_bus_init();
    control_init(bus, bus ? i2c_scan(bus) : 0);

    // 设置与屏幕无关：读 NVS 并应用（阈值、软启动、状态灯）。没装 LCD 也要跑，
    // 否则热保护和遥控用的还是编译期默认值。
    ui_app_config_init();

#ifdef CONFIG_UI_ENABLE
    bool ui_ok = display_init();
#else
    bool ui_ok = false;
    ESP_LOGW(TAG, "UI disabled in menuconfig, running headless");
#endif
    if (ui_ok) {
        ui_app_reset(now_ms());
        KK_UI_Status st = KK_UI_Init(ui_app_get());
        if (st != KK_UI_OK) {
            ESP_LOGE(TAG, "KK_UI init failed: %d", st);
            ui_ok = false;
        }
    }
    ui_app_set_ui_ready(ui_ok);
    if (!ui_ok) {
        // 没装屏、屏坏了都走这里：不画图、不读按键菜单，其余功能照常
        ESP_LOGW(TAG, "no LCD/UI: 只保留遥控、热保护、状态灯和 OTA，LED_PWM 上电仍为 0%%");
    }

#ifdef CONFIG_REMOTE_ENABLE
    // 上电顺序：先完成全部小电流外设，Wi-Fi（射频校准 + 发射，电流最大）最后延时启动。
    // 若上次是欠压复位，延时加长，给 3.3 V DCDC 和输入电源更多稳定时间。
    uint32_t wifi_delay = CONFIG_REMOTE_START_DELAY_MS;
    if (esp_reset_reason() == ESP_RST_BROWNOUT) {
        wifi_delay *= 3;
        ESP_LOGW(TAG, "last reset was brownout, Wi-Fi delayed to %lu ms", (unsigned long)wifi_delay);
    }
    if (remote_start_delayed(wifi_delay) != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi remote not available");
    }
#endif

    ESP_LOGI(TAG, "ready, LED_PWM 0%%, limit %u.%u%%",
             led_pwm_get_limit() / 10, led_pwm_get_limit() % 10);

    uint32_t t_beat = now_ms();
    uint32_t t_retry = 0;
    uint32_t t_publish = 0;
    bool boot_confirmed = false;
    while (true) {
        uint32_t now = now_ms();
        uint8_t keys = ui_input_poll(now);

        control_update(now);

        // 跑到这里说明初始化都过了、循环也在转，认下这次启动
        if (!boot_confirmed && now >= OTA_CONFIRM_MS) {
            boot_confirmed = true;
            ota_confirm_boot();
        }

        if (ui_ok) {
            ui_app_update(now);
            KK_UI_Status st = KK_UI_Update(now, (KK_UI_Input){.keys = keys});
            if (st == KK_UI_DISPLAY_ERROR || st == KK_UI_DISPLAY_TIMEOUT) {
                // 刷新失败后限速重试，避免刷屏日志
                if (now - t_retry >= DISPLAY_RETRY_MS) {
                    t_retry = now;
                    ESP_LOGE(TAG, "display error %d, recovering", st);
                    KK_UI_RecoverDisplay();
                }
            }
            ui_app_process_events();
        }

        // 手机命令和状态上报跟界面无关：没装屏也得照常收发，否则板子就没法控制了
        remote_cmd_t cmd;
        while (remote_poll(&cmd)) {
            remote_reply(&cmd, ui_app_remote_cmd(&cmd, now));
        }
        if (now - t_publish >= REMOTE_PUBLISH_MS) {
            t_publish = now;
            remote_status_t rs;
            ui_app_remote_status(&rs);
            remote_publish(&rs);
        }

        if (now - t_beat >= HEARTBEAT_MS) {
            t_beat = now;
            ctrl_status_t cs;
            control_get_status(&cs);
            char t[16] = "--";
            if (cs.temp_valid) {
                int t10 = (int)(cs.temp_c * 10);
                snprintf(t, sizeof(t), "%s%d.%d C", t10 < 0 ? "-" : "",
                         (t10 < 0 ? -t10 : t10) / 10, (t10 < 0 ? -t10 : t10) % 10);
            }
            ESP_LOGI(TAG, "alive, PWM %u.%u%%, set %u.%u%%, T %s, fault %d",
                     cs.pwm / 10, cs.pwm % 10, cs.setpoint / 10, cs.setpoint % 10, t, cs.fault);
        }
        vTaskDelay(pdMS_TO_TICKS(LOOP_MS));
    }
}
