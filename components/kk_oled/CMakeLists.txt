# KK_OLED 图形核心（MIT，青岛波特律动科技有限公司）+ ESP32-C3 ST7735S 驱动
idf_component_register(SRCS "graphics/kk_oled.c"
                            "graphics/kk_oled_bitmap.c"
                            "graphics/kk_oled_draw.c"
                            "graphics/kk_oled_font.c"
                            "driver/kk_oled_driver.c"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "graphics" "driver"
                       PRIV_REQUIRES esp_lcd esp_driver_spi esp_driver_gpio)
