/*
 * TMP102 温度传感器
 *
 * 温度寄存器 0x00：12 位补码，高字节在前，左对齐，0.0625 ℃/LSB。
 * 配置寄存器 0x01：上电默认 0x60A0（12 位、连续转换、4 Hz）。
 */
#include "tmp102.h"

#include "esp_check.h"
#include "esp_log.h"

#define TMP102_REG_TEMP     0x00
#define TMP102_REG_CONFIG   0x01
#define TMP102_SCL_HZ       100000
#define TMP102_TIMEOUT_MS   50

static const char *TAG = "tmp102";

static i2c_master_dev_handle_t s_dev;

static esp_err_t read_reg16(uint8_t reg, uint8_t out[2])
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, out, 2, TMP102_TIMEOUT_MS);
}

esp_err_t tmp102_init(i2c_master_bus_handle_t bus, uint16_t addr)
{
    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = TMP102_SCL_HZ,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &cfg, &s_dev), TAG, "add device failed");

    uint8_t conf[2];
    esp_err_t err = read_reg16(TMP102_REG_CONFIG, conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "no response at 0x%02X (%s)", addr, esp_err_to_name(err));
        i2c_master_bus_rm_device(s_dev);
        s_dev = NULL;
        return err;
    }
    ESP_LOGI(TAG, "found at 0x%02X, config 0x%02X%02X", addr, conf[0], conf[1]);
    return ESP_OK;
}

esp_err_t tmp102_read(float *temp_c)
{
    ESP_RETURN_ON_FALSE(s_dev, ESP_ERR_INVALID_STATE, TAG, "not initialized");

    uint8_t raw[2];
    ESP_RETURN_ON_ERROR(read_reg16(TMP102_REG_TEMP, raw), TAG, "read failed");

    // 拼成 16 位后算术右移 4 位，保留符号
    int16_t v = (int16_t)((raw[0] << 8) | raw[1]) >> 4;
    *temp_c = v * 0.0625f;
    return ESP_OK;
}
