idf_component_register(SRCS "main.c" "lcd_st7789.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_driver_gpio esp_driver_ledc esp_driver_spi
                                esp_lcd esp_system esp_hw_support log)
