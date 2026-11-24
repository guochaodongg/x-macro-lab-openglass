idf_component_register(SRCS "main.c" "lcd_st7789.c" "dns_server.c"
                            "photo_wifi.c" "photo_display.c" "photo_httpd.c"
                       INCLUDE_DIRS "."
                       EMBED_TXTFILES "web/upload.html"
                       REQUIRES esp_driver_gpio esp_driver_ledc esp_driver_spi
                                esp_lcd esp_system esp_hw_support log
                                esp_wifi esp_netif esp_event esp_http_server
                                nvs_flash lwip esp_jpeg)
