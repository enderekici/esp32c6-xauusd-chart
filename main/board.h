#pragma once

// Waveshare ESP32-C6-LCD-1.47 pinout
// Display: ST7789, 172x320, SPI
// Source: Waveshare wiki

#define BOARD_LCD_SPI_HOST   SPI2_HOST
#define BOARD_LCD_PIN_MOSI   6
#define BOARD_LCD_PIN_SCLK   7
#define BOARD_LCD_PIN_CS     14
#define BOARD_LCD_PIN_DC     15
#define BOARD_LCD_PIN_RST    21
#define BOARD_LCD_PIN_BL     22

// Portrait: 172 wide x 320 tall, matches Waveshare's reference LVGL config.
// The user holds the board with USB at bottom; we draw portrait. The 34-col
// offset (240-col ST7789 controller, visible window 34..205) is applied
// manually inside the draw functions, exactly like Waveshare's flush_cb.
#define BOARD_LCD_H_RES      172
#define BOARD_LCD_V_RES      320
#define BOARD_LCD_OFFSET_X   34
#define BOARD_LCD_OFFSET_Y   0

#define BOARD_LCD_SPI_HZ     (10 * 1000 * 1000)
#define BOARD_LCD_CMD_BITS   8
#define BOARD_LCD_PARAM_BITS 8

#define BOARD_LCD_BL_ON_LEVEL  1
#define BOARD_LCD_BL_OFF_LEVEL 0
