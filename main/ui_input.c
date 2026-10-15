/*
 * 两键 → KK_UI 三键输入适配
 *
 * 状态机：
 *   IDLE     无键
 *   PENDING  有一键按下，等待 UI_CHORD_WINDOW_MS 看另一键是否跟上；期间不输出
 *   CHORD    两键都按下 → 输出 OK，直到两键都松开
 *   SINGLE   窗口结束仍只有一键 → 输出该键电平，KK_UI 负责长按连发
 *   TAP      窗口内就松开的短按 → 补发 UI_TAP_MS 的单键脉冲
 *   RELEASE  等待所有键松开（单键期间又按下另一键时进入，不再产生输入）
 */
#include "ui_input.h"

#include "driver/gpio.h"
#include "esp_check.h"
#include "kk_ui.h"
#include "board_pins.h"

#define UI_DEBOUNCE_MS      15      // 电平稳定时间；板上另有 10 kΩ + 100 nF
#define UI_CHORD_WINDOW_MS  60
#define UI_TAP_MS           40      // 需大于 KK_UI_KEY_DEBOUNCE_MS（20 ms）

typedef enum {
    ST_IDLE,
    ST_PENDING,
    ST_CHORD,
    ST_SINGLE,
    ST_TAP,
    ST_RELEASE,
} state_t;

static const char *TAG = "ui_input";

static state_t s_state;
static uint32_t s_since;        // 进入当前状态的时间
static uint8_t s_key;           // PENDING/SINGLE/TAP 对应的单键
static uint8_t s_raw, s_stable;
static uint32_t s_raw_changed;

esp_err_t ui_input_init(void)
{
    const gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << PIN_KEY_UP) | (1ULL << PIN_KEY_DOWN_BOOT),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,   // 与外部上拉并联，无副作用
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&cfg), TAG, "gpio config failed");
    return ESP_OK;
}

static uint8_t read_raw(void)
{
    uint8_t k = 0;
    if (gpio_get_level(PIN_KEY_UP) == 0) {
        k |= KK_UI_KEY_UP;
    }
    if (gpio_get_level(PIN_KEY_DOWN_BOOT) == 0) {
        k |= KK_UI_KEY_DOWN;
    }
    return k;
}

static void go(state_t st, uint32_t now_ms)
{
    s_state = st;
    s_since = now_ms;
}

uint8_t ui_input_poll(uint32_t now_ms)
{
    const uint8_t both = KK_UI_KEY_UP | KK_UI_KEY_DOWN;

    uint8_t raw = read_raw();
    if (raw != s_raw) {
        s_raw = raw;
        s_raw_changed = now_ms;
    }
    if (now_ms - s_raw_changed >= UI_DEBOUNCE_MS) {
        s_stable = s_raw;
    }
    const uint8_t k = s_stable;

    switch (s_state) {
    case ST_IDLE:
        if (k == both) {
            go(ST_CHORD, now_ms);
        } else if (k) {
            s_key = k;
            go(ST_PENDING, now_ms);
        }
        break;

    case ST_PENDING:
        if (k == both) {
            go(ST_CHORD, now_ms);
        } else if (!k) {
            go(ST_TAP, now_ms);
        } else if (now_ms - s_since >= UI_CHORD_WINDOW_MS) {
            go(ST_SINGLE, now_ms);
        }
        break;

    case ST_CHORD:
        if (!k) {
            go(ST_IDLE, now_ms);
        }
        break;

    case ST_SINGLE:
        if (!k) {
            go(ST_IDLE, now_ms);
        } else if (k != s_key) {
            go(ST_RELEASE, now_ms);
        }
        break;

    case ST_TAP:
        if (now_ms - s_since >= UI_TAP_MS) {
            go(k ? ST_RELEASE : ST_IDLE, now_ms);
        }
        break;

    case ST_RELEASE:
        if (!k) {
            go(ST_IDLE, now_ms);
        }
        break;
    }

    switch (s_state) {
    case ST_CHORD:
        return KK_UI_KEY_OK;
    case ST_SINGLE:
    case ST_TAP:
        return s_key;
    default:
        return 0;
    }
}
