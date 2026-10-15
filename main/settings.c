/*
 * 用户设置掉电保存
 */
#include "settings.h"

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#define NS              "settings"
#define KEY_SOFTSTART   "softstart"
#define KEY_STEP        "step"
#define KEY_LED_BRI     "led_bri"
#define KEY_LED_EN      "led_en"
#define KEY_PROTECT     "protect"

static const char *TAG = "settings";

static bool s_ready;

esp_err_t settings_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // NVS 分区格式不兼容或已满：擦除后重新初始化，已保存的设置会丢失
        ESP_LOGW(TAG, "NVS needs erase (%s)", esp_err_to_name(err));
        err = nvs_flash_erase();
        if (err == ESP_OK) {
            err = nvs_flash_init();
        }
    }
    s_ready = err == ESP_OK;
    if (!s_ready) {
        ESP_LOGE(TAG, "NVS init failed (%s), settings kept in RAM only", esp_err_to_name(err));
    }
    return err;
}

void settings_load(app_settings_t *s)
{
    nvs_handle_t h;
    if (!s_ready || nvs_open(NS, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "no saved settings, using defaults");
        return;
    }
    uint8_t en;
    nvs_get_i32(h, KEY_SOFTSTART, &s->softstart_ms);
    nvs_get_i32(h, KEY_STEP, &s->step_percent);
    nvs_get_i32(h, KEY_LED_BRI, &s->led_brightness);
    if (nvs_get_u8(h, KEY_LED_EN, &en) == ESP_OK) {
        s->led_enable = en != 0;
    }
    if (nvs_get_u8(h, KEY_PROTECT, &en) == ESP_OK) {
        s->protect = en != 0;
    }
    nvs_close(h);
    ESP_LOGI(TAG, "loaded: softstart %ld ms, step %ld%%, led %s %ld%%, thermal protect %s",
             (long)s->softstart_ms, (long)s->step_percent,
             s->led_enable ? "on" : "off", (long)s->led_brightness, s->protect ? "on" : "off");
    if (!s->protect) {
        ESP_LOGW(TAG, "thermal protection is OFF (saved setting)");
    }
}

esp_err_t settings_save(const app_settings_t *s)
{
    nvs_handle_t h;
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    // NVS 对值相同的写入不会再擦写 Flash
    err = nvs_set_i32(h, KEY_SOFTSTART, s->softstart_ms);
    if (err == ESP_OK) err = nvs_set_i32(h, KEY_STEP, s->step_percent);
    if (err == ESP_OK) err = nvs_set_i32(h, KEY_LED_BRI, s->led_brightness);
    if (err == ESP_OK) err = nvs_set_u8(h, KEY_LED_EN, s->led_enable);
    if (err == ESP_OK) err = nvs_set_u8(h, KEY_PROTECT, s->protect);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "save failed (%s)", esp_err_to_name(err));
    }
    return err;
}
