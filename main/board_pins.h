/*
 * 850 nm 控制验证板 GPIO 分配
 * 与 README.md / 设计报告第 7 节保持一致
 */
#pragma once

#define PIN_VBUS_ADC        0   // ADC1_CH0，40k/10k 分压
#define PIN_LED_PWM         1   // 三颗 LP8556 公共调光 + LED52 指示灯，外部 10k 下拉
#define PIN_KEY_UP          2   // 低有效，启动绑带脚
#define PIN_TFT_DC          3
#define PIN_I2C_SDA         4   // TMP102 (0x48)，LP8556 (0x2C) 预留
#define PIN_I2C_SCL         5
#define PIN_TFT_SCLK        6
#define PIN_TFT_MOSI        7
#define PIN_WS2812          8   // 启动绑带脚
#define PIN_KEY_DOWN_BOOT   9   // 低有效，复位时按住进入下载模式
#define PIN_TFT_CS          10
#define PIN_UART_RXD        20
#define PIN_UART_TXD        21
