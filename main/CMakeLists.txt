idf_component_register(SRCS "ble_link.c" "lp8556.c" "vbus.c" "app_main.c"
                            "control.c"
                            "settings.c"
                            "remote.c"
                            "led_pwm.c"
                            "tmp102.c"
                            "ws2812.c"
                            "led_strip_encoder.c"
                            "ui_input.c"
                            "ui_app.c"
                            "ui_fonts.c"
                       INCLUDE_DIRS "."
                       EMBED_TXTFILES "web/index.html"
                       PRIV_REQUIRES esp_driver_ledc esp_driver_gpio esp_driver_i2c
                                     esp_driver_rmt esp_timer esp_app_format spi_flash nvs_flash esp_adc
                                     esp_wifi esp_netif esp_event esp_http_server dns_server bt
                                     kk_oled kk_ui)
