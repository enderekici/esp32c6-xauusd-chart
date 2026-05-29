#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "board.h"
#include "lcd.h"
#include "wifi.h"
#include "ui_chart.h"
#include "led_strip.h"

static const char *TAG = "app";

// Onboard WS2812 RGB LED (GPIO8). Unused here; clear it at boot so the board
// isn't glowing on the back.
#define BOARD_RGB_LED_GPIO 8

static void rgb_led_off(void)
{
    led_strip_config_t scfg = {
        .strip_gpio_num   = BOARD_RGB_LED_GPIO,
        .max_leds         = 1,
        .led_pixel_format = LED_PIXEL_FORMAT_GRB,
        .led_model        = LED_MODEL_WS2812,
    };
    led_strip_rmt_config_t rmt = {
        .clk_src       = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
    };
    led_strip_handle_t strip;
    if (led_strip_new_rmt_device(&scfg, &rmt, &strip) == ESP_OK) {
        led_strip_clear(strip);
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "PAXG/USD gold chart booting");

    esp_err_t nvs = nvs_flash_init();
    if (nvs == ESP_ERR_NVS_NO_FREE_PAGES || nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(nvs);
    }

    rgb_led_off();

    static lcd_t lcd;
    ESP_ERROR_CHECK(lcd_init(&lcd));

    wifi_start();
    ui_chart_start(&lcd);

    ESP_LOGI(TAG, "init complete");
}
