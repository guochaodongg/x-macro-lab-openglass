idf_component_register(SRCS "lcd_st7789.c" "main.c" "dns_server.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp32-camera esp_driver_gpio esp_driver_ledc esp_driver_spi
                                esp_event esp_http_server esp_lcd esp_netif esp_system
                                esp_timer esp_wifi nvs_flash lwip)
