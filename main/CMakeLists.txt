idf_component_register(SRCS "ble_scan.c" "alarm.c" "app_icons.c" "bg_home.c" "lv_font_degree_16.c" "pc_mon.c" "lv_font_cn_16.c" "weather.c" "time_sync.c" "wifi_scan.c" "ft6336.c" "main.c" "ui.c" "lvgl_port.c" "../lcd/lcd.c" "../lcd/lcd_init.c" "../lcd/delay.c"
                    INCLUDE_DIRS "." "../lcd"
                    REQUIRES lvgl esp_driver_gpio esp_lcd esp_timer esp_driver_i2c esp_driver_ledc
                              esp_wifi esp_netif esp_event nvs_flash lwip
                              esp_http_client esp-tls mbedtls bt)
