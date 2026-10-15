/*
 * WS2812-V6 状态灯，基于 IDF 示例 rmt/led_strip 的编码器
 */
#include "ws2812.h"

#include "driver/rmt_tx.h"
#include "esp_check.h"
#include "board_pins.h"
#include "led_strip_encoder.h"

#define WS2812_RESOLUTION_HZ    10000000    // 10 MHz，1 tick = 0.1 us

static const char *TAG = "ws2812";

static rmt_channel_handle_t s_chan;
static rmt_encoder_handle_t s_encoder;

esp_err_t ws2812_init(void)
{
    const rmt_tx_channel_config_t chan_cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .gpio_num = PIN_WS2812,
        .mem_block_symbols = 48,
        .resolution_hz = WS2812_RESOLUTION_HZ,
        .trans_queue_depth = 4,
    };
    ESP_RETURN_ON_ERROR(rmt_new_tx_channel(&chan_cfg, &s_chan), TAG, "new channel failed");

    const led_strip_encoder_config_t enc_cfg = {
        .resolution = WS2812_RESOLUTION_HZ,
    };
    ESP_RETURN_ON_ERROR(rmt_new_led_strip_encoder(&enc_cfg, &s_encoder), TAG, "new encoder failed");
    ESP_RETURN_ON_ERROR(rmt_enable(s_chan), TAG, "enable failed");

    return ws2812_set(0, 0, 0);
}

esp_err_t ws2812_set(uint8_t r, uint8_t g, uint8_t b)
{
    ESP_RETURN_ON_FALSE(s_chan, ESP_ERR_INVALID_STATE, TAG, "not initialized");

    // WS2812 数据顺序为 GRB
    const uint8_t grb[3] = {g, r, b};
    const rmt_transmit_config_t tx_cfg = {
        .loop_count = 0,
    };
    ESP_RETURN_ON_ERROR(rmt_transmit(s_chan, s_encoder, grb, sizeof(grb), &tx_cfg), TAG, "transmit failed");
    return rmt_tx_wait_all_done(s_chan, 100);
}
