#pragma once

#include "lcd.h"

#ifdef __cplusplus
extern "C" {
#endif

// Bring up LVGL on the given panel, build the gold-price chart UI, and spawn a
// background task that fetches PAXGUSDT market data over HTTPS every ~45s and
// updates the chart/labels under the LVGL lock. Call once at boot after
// lcd_init() and wifi_start().
void ui_chart_start(lcd_t *lcd);

#ifdef __cplusplus
}
#endif
