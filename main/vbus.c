/*
 * VBUS 电压采样
 */
#include "vbus.h"

#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_check.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include "board_pins.h"

#define VBUS_SAMPLES        8
#define VBUS_ADC_FULL_MV    2450    // 12 dB 衰减下校准曲线的可信上限（略低于 2.5 V）

static const char *TAG = "vbus";

static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali;
static adc_channel_t s_chan;

esp_err_t vbus_init(void)
{
    adc_unit_t unit;
    ESP_RETURN_ON_ERROR(adc_oneshot_io_to_channel(PIN_VBUS_ADC, &unit, &s_chan), TAG, "GPIO%d is not an ADC pin", PIN_VBUS_ADC);

    const adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id = unit,
    };
    ESP_RETURN_ON_ERROR(adc_oneshot_new_unit(&unit_cfg, &s_adc), TAG, "adc unit init failed");

    const adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    ESP_RETURN_ON_ERROR(adc_oneshot_config_channel(s_adc, s_chan, &chan_cfg), TAG, "adc channel config failed");

    const adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id = unit,
        .chan = s_chan,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_cali) != ESP_OK) {
        // 芯片未烧录校准值时退化为按满量程线性换算，误差较大但足够判档
        s_cali = NULL;
        ESP_LOGW(TAG, "no ADC calibration, using raw linear conversion");
    }
    ESP_LOGI(TAG, "ADC1_CH%d (GPIO%d), divider %d/1000, 9V threshold %d mV +/- %d mV",
             s_chan, PIN_VBUS_ADC, CONFIG_VBUS_DIVIDER_PERMILLE,
             CONFIG_VBUS_9V_THRESHOLD_MV, CONFIG_VBUS_HYSTERESIS_MV);
    return ESP_OK;
}

esp_err_t vbus_read(int *adc_mv, int *vbus_mv)
{
    ESP_RETURN_ON_FALSE(s_adc, ESP_ERR_INVALID_STATE, TAG, "not initialized");

    int sum = 0;
    for (int i = 0; i < VBUS_SAMPLES; i++) {
        int raw;
        ESP_RETURN_ON_ERROR(adc_oneshot_read(s_adc, s_chan, &raw), TAG, "adc read failed");
        sum += raw;
    }
    int raw = sum / VBUS_SAMPLES;

    int mv;
    if (s_cali) {
        ESP_RETURN_ON_ERROR(adc_cali_raw_to_voltage(s_cali, raw, &mv), TAG, "adc cali failed");
    } else {
        mv = raw * 2500 / 4095;
    }
    *adc_mv = mv;
    *vbus_mv = mv * 1000 / CONFIG_VBUS_DIVIDER_PERMILLE;
    return ESP_OK;
}

bool vbus_adc_saturated(int adc_mv)
{
    return adc_mv >= VBUS_ADC_FULL_MV;
}
