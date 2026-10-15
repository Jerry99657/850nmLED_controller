/*
 * LED_PWM 公共调光驱动
 *
 * GPIO1 同时驱动三颗 LP8556 的 PWM 输入和 LED52（1 kΩ 限流）。
 * 外部 10 kΩ 下拉保证复位期间为低；本驱动初始化时也以 0% 输出。
 */
#include "led_pwm.h"

#include "driver/ledc.h"
#include "esp_check.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include "board_pins.h"

#define LED_PWM_MODE        LEDC_LOW_SPEED_MODE
#define LED_PWM_TIMER       LEDC_TIMER_0
#define LED_PWM_CHANNEL     LEDC_CHANNEL_0
#define LED_PWM_RES_BITS    LEDC_TIMER_12_BIT
#define LED_PWM_DUTY_FULL   (1U << 12)  // 2^res 为 100% 常高

static const char *TAG = "led_pwm";

static bool s_inited;
static uint16_t s_permille;
static uint16_t s_limit = CONFIG_LED_PWM_LIMIT_5V_PERCENT * 10;   // 首次 VBUS 判档前按 5 V

static uint32_t permille_to_duty(uint16_t permille)
{
    return (uint32_t)permille * LED_PWM_DUTY_FULL / LED_PWM_PERMILLE_MAX;
}

static uint16_t clamp(uint16_t permille)
{
    return permille > s_limit ? s_limit : permille;
}

esp_err_t led_pwm_init(void)
{
    const ledc_timer_config_t timer = {
        .speed_mode = LED_PWM_MODE,
        .timer_num = LED_PWM_TIMER,
        .duty_resolution = LED_PWM_RES_BITS,
        .freq_hz = CONFIG_LED_PWM_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), TAG, "timer config failed");

    const ledc_channel_config_t channel = {
        .gpio_num = PIN_LED_PWM,
        .speed_mode = LED_PWM_MODE,
        .channel = LED_PWM_CHANNEL,
        .timer_sel = LED_PWM_TIMER,
        .duty = 0,
        .hpoint = 0,
    };
    ESP_RETURN_ON_ERROR(ledc_channel_config(&channel), TAG, "channel config failed");
    ESP_RETURN_ON_ERROR(ledc_fade_func_install(0), TAG, "fade install failed");

    s_permille = 0;
    s_inited = true;
    ESP_LOGI(TAG, "GPIO%d, %d Hz, 12-bit, limit %u.%u%%",
             PIN_LED_PWM, CONFIG_LED_PWM_FREQ_HZ, s_limit / 10, s_limit % 10);
    return ESP_OK;
}

esp_err_t led_pwm_set_limit(uint16_t limit_permille)
{
    if (limit_permille > LED_PWM_PERMILLE_MAX) {
        limit_permille = LED_PWM_PERMILLE_MAX;
    }
    s_limit = limit_permille;
    if (s_inited && s_permille > s_limit) {
        // 上限下调时不渐变，直接降到上限
        ESP_RETURN_ON_ERROR(ledc_set_duty_and_update(LED_PWM_MODE, LED_PWM_CHANNEL,
                                                     permille_to_duty(s_limit), 0),
                            TAG, "set duty failed");
        s_permille = s_limit;
    }
    return ESP_OK;
}

esp_err_t led_pwm_set(uint16_t permille)
{
    ESP_RETURN_ON_FALSE(s_inited, ESP_ERR_INVALID_STATE, TAG, "not initialized");

    uint16_t target = clamp(permille);
    if (target == s_permille) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(ledc_set_duty_and_update(LED_PWM_MODE, LED_PWM_CHANNEL,
                                                 permille_to_duty(target), 0),
                        TAG, "set duty failed");
    s_permille = target;
    return ESP_OK;
}

esp_err_t led_pwm_off(void)
{
    ESP_RETURN_ON_FALSE(s_inited, ESP_ERR_INVALID_STATE, TAG, "not initialized");
    ESP_RETURN_ON_ERROR(ledc_set_duty_and_update(LED_PWM_MODE, LED_PWM_CHANNEL, 0, 0),
                        TAG, "off failed");
    s_permille = 0;
    return ESP_OK;
}

uint16_t led_pwm_get(void)
{
    return s_permille;
}

uint16_t led_pwm_get_limit(void)
{
    return s_limit;
}
