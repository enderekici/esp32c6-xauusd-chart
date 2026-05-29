idf_component_register(
    SRCS "main.c" "lcd.c" "font_spleen.c" "Vernon_ST7789T.c" "wifi.c" "market.c" "ui_chart.c"
    INCLUDE_DIRS "."
    REQUIRES driver esp_lcd esp_wifi esp_netif esp_event esp_http_client esp_websocket_client esp-tls json nvs_flash esp_timer lvgl esp_lvgl_port led_strip
)
