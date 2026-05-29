#include "lcd.h"
#include "board.h"
#include "font_spleen.h"

// Spleen 8x16 native — designed at this size, not scaled.
#define GLYPH_W   8
#define GLYPH_H   16

#include <string.h>
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "Vernon_ST7789T.h"

static const char *TAG = "lcd";

esp_err_t lcd_init(lcd_t *lcd)
{
    memset(lcd, 0, sizeof(*lcd));

    // Backlight via LEDC PWM — dim instead of full brightness to cut heat.
    ledc_timer_config_t bl_tmr = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .timer_num       = LEDC_TIMER_0,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .freq_hz         = 5000,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&bl_tmr));
    ledc_channel_config_t bl_ch = {
        .gpio_num   = BOARD_LCD_PIN_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = LEDC_CHANNEL_0,
        .timer_sel  = LEDC_TIMER_0,
        .duty       = 0,
        .hpoint     = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&bl_ch));

    spi_bus_config_t bus = {
        .mosi_io_num = BOARD_LCD_PIN_MOSI,
        .sclk_io_num = BOARD_LCD_PIN_SCLK,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = BOARD_LCD_H_RES * 80 * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(BOARD_LCD_SPI_HOST, &bus, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num = BOARD_LCD_PIN_DC,
        .cs_gpio_num = BOARD_LCD_PIN_CS,
        .pclk_hz = BOARD_LCD_SPI_HZ,
        .lcd_cmd_bits = BOARD_LCD_CMD_BITS,
        .lcd_param_bits = BOARD_LCD_PARAM_BITS,
        .spi_mode = 0,
        .trans_queue_depth = 1,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)BOARD_LCD_SPI_HOST,
                                             &io_cfg, &lcd->io));

    // Exact replication of Waveshare's reference (LCD_Driver/ST7789.c):
    // ST7789T + BGR + mirror(true, false) only. They do NOT call set_gap;
    // they add Offset_X=34 manually inside their flush_cb (LVGL_Driver.c L35).
    // We mirror that pattern below in draw helpers.
    esp_lcd_panel_dev_st7789t_config_t panel_cfg = {
        .reset_gpio_num = BOARD_LCD_PIN_RST,
        .rgb_endian = LCD_RGB_ENDIAN_BGR,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789t(lcd->io, &panel_cfg, &lcd->panel));

    ESP_ERROR_CHECK(esp_lcd_panel_reset(lcd->panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(lcd->panel));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(lcd->panel, true, false));
    // Apply the 172x320 centered-window offset at the panel level (instead of
    // adding it per-blit) so both our blitter and LVGL's flushes land aligned.
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(lcd->panel, BOARD_LCD_OFFSET_X, BOARD_LCD_OFFSET_Y));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(lcd->panel, true));

    lcd->fb = heap_caps_malloc(BOARD_LCD_H_RES * 8 * sizeof(uint16_t), MALLOC_CAP_DMA);
    if (!lcd->fb) {
        ESP_LOGE(TAG, "fb alloc failed");
        return ESP_ERR_NO_MEM;
    }

    // Double-buffer the per-glyph DMA buffer so we never overwrite a buffer
    // while the previous SPI transfer is still draining it (trans_queue_depth
    // serializes commands but not the in-flight color DMA).
    for (int i = 0; i < 2; i++) {
        lcd->glyph_buf[i] = heap_caps_malloc(GLYPH_W * GLYPH_H * 4 * sizeof(uint16_t),
                                             MALLOC_CAP_DMA);
        if (!lcd->glyph_buf[i]) {
            ESP_LOGE(TAG, "glyph_buf[%d] alloc failed", i);
            return ESP_ERR_NO_MEM;
        }
    }
    lcd->glyph_idx = 0;

    // 30% duty — bright enough to read indoors, runs cool.
    ESP_ERROR_CHECK(ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 300));
    ESP_ERROR_CHECK(ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0));
    ESP_LOGI(TAG, "ST7789 %dx%d initialised", BOARD_LCD_H_RES, BOARD_LCD_V_RES);
    return ESP_OK;
}

// The 172x320 centered-window offset is applied by the panel via set_gap()
// (see lcd_init), so coordinates here are 0-based for both us and LVGL.
static inline void panel_blit(lcd_t *lcd, int x1, int y1, int x2, int y2,
                              const void *pixels)
{
    esp_lcd_panel_draw_bitmap(lcd->panel, x1, y1, x2, y2, pixels);
}

void lcd_fill(lcd_t *lcd, uint16_t color)
{
    for (int i = 0; i < BOARD_LCD_H_RES * 8; i++) lcd->fb[i] = color;
    for (int y = 0; y < BOARD_LCD_V_RES; y += 8) {
        int h = (y + 8 <= BOARD_LCD_V_RES) ? 8 : (BOARD_LCD_V_RES - y);
        panel_blit(lcd, 0, y, BOARD_LCD_H_RES, y + h, lcd->fb);
    }
}

// Render one Spleen 6x12 glyph. Bytes are row-major, bit 7 = leftmost pixel.
static void draw_glyph(lcd_t *lcd, int x, int y, char c,
                       uint16_t fg, uint16_t bg, int scale)
{
    if (c < 0x20 || c > 0x7E) c = '?';
    const uint8_t *g = font_spleen8x16[c - 0x20];

    const int gw = GLYPH_W * scale;
    const int gh = GLYPH_H * scale;
    if (x + gw > BOARD_LCD_H_RES || y + gh > BOARD_LCD_V_RES) return;

    // Pick the next double-buffer so the previous glyph's DMA can drain in
    // the background while we fill this one.
    uint16_t *buf = lcd->glyph_buf[lcd->glyph_idx];
    lcd->glyph_idx ^= 1;
    if (gw * gh > GLYPH_W * GLYPH_H * 4) return;

    for (int row = 0; row < GLYPH_H; row++) {
        uint8_t bits = g[row];
        for (int col = 0; col < GLYPH_W; col++) {
            uint16_t px = (bits & (1u << (7 - col))) ? fg : bg;
            int dx0 = col * scale;
            int dy0 = row * scale;
            for (int sy = 0; sy < scale; sy++) {
                for (int sx = 0; sx < scale; sx++) {
                    buf[(dy0 + sy) * gw + (dx0 + sx)] = px;
                }
            }
        }
    }
    panel_blit(lcd, x, y, x + gw, y + gh, buf);
}

void lcd_draw_text_scaled(lcd_t *lcd, int x, int y, const char *text,
                          uint16_t fg, uint16_t bg, int scale)
{
    if (!text || scale < 1 || scale > 2) return;
    const int gw = GLYPH_W * scale;
    const int gh = GLYPH_H * scale;
    while (*text) {
        if (x + gw > BOARD_LCD_H_RES) {
            x = 0; y += gh + 1;
            if (y + gh > BOARD_LCD_V_RES) return;
        }
        draw_glyph(lcd, x, y, *text++, fg, bg, scale);
        x += gw;
    }
}

void lcd_draw_text(lcd_t *lcd, int x, int y, const char *text,
                   uint16_t fg, uint16_t bg)
{
    lcd_draw_text_scaled(lcd, x, y, text, fg, bg, 1);
}
