#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_handle_t panel;
    uint16_t *fb;             // strip buffer for fills
    uint16_t *glyph_buf[2];   // double-buffered glyph buf (DMA-capable)
    int       glyph_idx;      // round-robin index
} lcd_t;

esp_err_t lcd_init(lcd_t *lcd);

// Fill the whole panel with a 16-bit RGB565 colour (big-endian on the wire).
void lcd_fill(lcd_t *lcd, uint16_t color);

// Draw an ASCII string at (x,y) with an 8x8 built-in font.
void lcd_draw_text(lcd_t *lcd, int x, int y, const char *text,
                   uint16_t fg, uint16_t bg);

// Same as lcd_draw_text but each glyph pixel is rendered as a scale*scale block.
// scale=2 → 16x16 glyphs, scale=3 → 24x24, etc.
void lcd_draw_text_scaled(lcd_t *lcd, int x, int y, const char *text,
                          uint16_t fg, uint16_t bg, int scale);

// RGB565 helper.
static inline uint16_t lcd_rgb(uint8_t r, uint8_t g, uint8_t b) {
    uint16_t c = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
    return (c << 8) | (c >> 8);  // byte-swap for big-endian SPI
}

#ifdef __cplusplus
}
#endif
