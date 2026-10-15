/*
 * LP8556SQ-E09 LED 驱动配置
 */
#include "lp8556.h"

#include "esp_check.h"

#define LP8556_SCL_HZ       100000
#define LP8556_TIMEOUT_MS   20

static const char *TAG = "lp8556";

static i2c_master_dev_handle_t s_dev;

esp_err_t lp8556_init(i2c_master_bus_handle_t bus)
{
    ESP_RETURN_ON_FALSE(bus, ESP_ERR_INVALID_ARG, TAG, "no I2C bus");
    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = LP8556_ADDR,
        .scl_speed_hz = LP8556_SCL_HZ,
    };
    return i2c_master_bus_add_device(bus, &cfg, &s_dev);
}

esp_err_t lp8556_set_led_enable(uint8_t mask)
{
    ESP_RETURN_ON_FALSE(s_dev, ESP_ERR_INVALID_STATE, TAG, "not initialized");
    // 三颗器件同地址同时应答，等效于广播写；只写不读
    const uint8_t buf[2] = {LP8556_REG_LED_EN, (uint8_t)(mask & 0x3F)};
    return i2c_master_transmit(s_dev, buf, sizeof(buf), LP8556_TIMEOUT_MS);
}
