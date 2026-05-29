#pragma once

#include "lcd.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Bring up LVGL on the given panel, build the gold-price chart UI, and spawn
// background tasks for live prices plus low-rate chart-history refreshes. Call
// once at boot after lcd_init() and wifi_start().
void ui_chart_start(lcd_t *lcd);

typedef enum {
    UI_CHART_DISPLAY_AUTO = 0,
    UI_CHART_DISPLAY_DAY,
    UI_CHART_DISPLAY_DIM,
    UI_CHART_DISPLAY_OFF,
    UI_CHART_DISPLAY_CUSTOM,
} ui_chart_display_mode_t;

typedef struct {
    ui_chart_display_mode_t mode;
    uint8_t configured_percent;
    uint8_t effective_percent;
    uint32_t wake_remaining_s;
    uint32_t uptime_s;
    uint32_t last_history_age_s;
    uint32_t history_success_count;
    uint32_t history_fail_count;
    int chart_points;
    bool history_valid;
} ui_chart_status_t;

const char *ui_chart_display_mode_name(ui_chart_display_mode_t mode);
bool ui_chart_parse_display_mode(const char *mode, ui_chart_display_mode_t *out);
bool ui_chart_set_display_mode(ui_chart_display_mode_t mode);
bool ui_chart_set_custom_brightness(uint8_t percent);
void ui_chart_wake_for(uint32_t seconds);
void ui_chart_force_refresh(void);
void ui_chart_status_get(ui_chart_status_t *out);

#ifdef __cplusplus
}
#endif
