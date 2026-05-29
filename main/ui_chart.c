#include "ui_chart.h"

#include "board.h"
#include "wifi.h"
#include "market.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "lvgl.h"
#include "esp_lvgl_port.h"

static const char *TAG = "ui";

// Dark theme palette.
#define COL_BG     0x0b0e14
#define COL_CARD   0x131722
#define COL_BORDER 0x222838
#define COL_CYAN   0x5ccfe6
#define COL_GREEN  0x7ee787
#define COL_RED    0xff8a8a
#define COL_GREY   0x7a88a0
#define COL_TEXT   0xd6deeb

#define REFRESH_PERIOD_MS 45000

static lv_obj_t *lbl_price;
static lv_obj_t *lbl_change;
static lv_obj_t *lbl_footer;
static lv_obj_t *chart;
static lv_chart_series_t *series;

// Wall-clock (esp_timer) seconds of the last successful data update.
static int64_t s_last_update_us;

static lv_obj_t *mk_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(l, lv_pct(100));
    return l;
}

static void build_ui(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(scr, 8, 0);
    lv_obj_set_style_pad_row(scr, 2, 0);
    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = mk_label(scr, &lv_font_montserrat_20, COL_CYAN);
    lv_label_set_text(title, "PAXG / USD");

    lv_obj_t *sub = mk_label(scr, &lv_font_montserrat_14, COL_GREY);
    lv_label_set_text(sub, "gold proxy");

    lbl_price = mk_label(scr, &lv_font_montserrat_28, COL_TEXT);
    lv_label_set_text(lbl_price, "----.--");

    lbl_change = mk_label(scr, &lv_font_montserrat_16, COL_GREY);
    lv_label_set_text(lbl_change, "+0.00%");

    // Chart fills the remaining vertical space.
    chart = lv_chart_create(scr);
    lv_obj_set_width(chart, lv_pct(100));
    lv_obj_set_flex_grow(chart, 1);
    lv_obj_set_style_bg_color(chart, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_bg_opa(chart, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(chart, lv_color_hex(COL_BORDER), 0);
    lv_obj_set_style_border_width(chart, 1, 0);
    lv_obj_set_style_radius(chart, 8, 0);
    lv_obj_set_style_pad_all(chart, 4, 0);
    lv_obj_set_style_size(chart, 0, 0, LV_PART_INDICATOR);  // hide point markers
    lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
    lv_chart_set_div_line_count(chart, 3, 0);
    lv_obj_set_style_line_color(chart, lv_color_hex(COL_BORDER), LV_PART_MAIN);
    lv_obj_set_style_line_opa(chart, LV_OPA_30, LV_PART_MAIN);
    lv_chart_set_update_mode(chart, LV_CHART_UPDATE_MODE_SHIFT);
    lv_chart_set_point_count(chart, MARKET_MAX_CLOSES);

    series = lv_chart_add_series(chart, lv_color_hex(COL_CYAN),
                                 LV_CHART_AXIS_PRIMARY_Y);
    lv_obj_set_style_line_width(chart, 2, LV_PART_ITEMS);

    lbl_footer = mk_label(scr, &lv_font_montserrat_14, COL_GREY);
    lv_label_set_text(lbl_footer, "connecting...");
}

// Recompute the y-range from the data and (re)load the series. Must run under
// the LVGL lock.
static void apply_data(const market_data_t *d)
{
    if (d->n_closes > 0) {
        float lo = d->closes[0], hi = d->closes[0];
        for (int i = 1; i < d->n_closes; i++) {
            if (d->closes[i] < lo) lo = d->closes[i];
            if (d->closes[i] > hi) hi = d->closes[i];
        }
        // Pad the range a touch so the line isn't glued to the edges.
        float pad = (hi - lo) * 0.08f;
        if (pad < 0.5f) pad = 0.5f;
        int ymin = (int)(lo - pad);
        int ymax = (int)(hi + pad + 0.5f);
        if (ymax <= ymin) ymax = ymin + 1;
        lv_chart_set_range(chart, LV_CHART_AXIS_PRIMARY_Y, ymin, ymax);

        lv_chart_set_point_count(chart, d->n_closes);
        for (int i = 0; i < d->n_closes; i++) {
            lv_chart_set_value_by_id(chart, series, i, (int32_t)(d->closes[i] + 0.5f));
        }
        lv_chart_refresh(chart);

        // Color price line + change by direction over the window.
        bool up = d->closes[d->n_closes - 1] >= d->closes[0];
        lv_chart_set_series_color(chart, series,
                                  lv_color_hex(up ? COL_GREEN : COL_CYAN));
    }

    if (d->price > 0.0f) {
        lv_label_set_text_fmt(lbl_price, "$%.2f", d->price);
    }

    if (d->ok) {
        bool pos = d->change_pct >= 0.0f;
        lv_label_set_text_fmt(lbl_change, "%s%.2f%% 24h",
                              pos ? "+" : "", d->change_pct);
        lv_obj_set_style_text_color(lbl_change,
                                    lv_color_hex(pos ? COL_GREEN : COL_RED), 0);
    }
}

// Refresh just the footer (called from a 1s LVGL timer so "Ns ago" ticks).
static void footer_cb(lv_timer_t *t)
{
    (void)t;
    int ago = -1;
    if (s_last_update_us > 0) {
        ago = (int)((esp_timer_get_time() - s_last_update_us) / 1000000LL);
    }
    if (wifi_is_connected()) {
        if (ago >= 0) {
            lv_label_set_text_fmt(lbl_footer, "%s  %ddBm  upd %ds ago",
                                  wifi_ip(), wifi_rssi(), ago);
        } else {
            lv_label_set_text_fmt(lbl_footer, "%s  %ddBm  fetching...",
                                  wifi_ip(), wifi_rssi());
        }
    } else {
        lv_label_set_text(lbl_footer, "Wi-Fi down");
    }
}

static void fetch_task(void *arg)
{
    (void)arg;
    market_data_t *d = malloc(sizeof(market_data_t));
    if (!d) {
        ESP_LOGE(TAG, "market_data_t alloc failed");
        vTaskDelete(NULL);
        return;
    }

    while (true) {
        // Wait for Wi-Fi before hammering the network.
        if (!wifi_is_connected()) {
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        // Network I/O happens OUTSIDE the LVGL lock.
        bool ok = market_fetch(d);

        lvgl_port_lock(0);
        apply_data(d);
        lvgl_port_unlock();

        if (ok) {
            s_last_update_us = esp_timer_get_time();
        }

        vTaskDelay(pdMS_TO_TICKS(ok ? REFRESH_PERIOD_MS : 10000));
    }
}

void ui_chart_start(lcd_t *lcd)
{
    lvgl_port_cfg_t pcfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_ERROR_CHECK(lvgl_port_init(&pcfg));

    lvgl_port_display_cfg_t dcfg = {
        .io_handle     = lcd->io,
        .panel_handle  = lcd->panel,
        .buffer_size   = BOARD_LCD_H_RES * 40,
        .double_buffer = true,
        .hres          = BOARD_LCD_H_RES,
        .vres          = BOARD_LCD_V_RES,
        .monochrome    = false,
        .color_format  = LV_COLOR_FORMAT_RGB565,
        // mirror_x MUST be true: esp_lvgl_port re-applies panel mirror from
        // these flags during add_disp; the panel needs mirror(true) to render
        // non-mirrored. swap_bytes MUST be false: true double-swaps RGB565.
        .rotation      = { .swap_xy = false, .mirror_x = true, .mirror_y = false },
        .flags = {
            .buff_dma   = true,
            .swap_bytes = false,
        },
    };
    lv_display_t *disp = lvgl_port_add_disp(&dcfg);
    if (!disp) {
        ESP_LOGE(TAG, "lvgl_port_add_disp failed");
        return;
    }

    lvgl_port_lock(0);
    build_ui();
    lv_timer_create(footer_cb, 1000, NULL);
    footer_cb(NULL);
    lvgl_port_unlock();

    xTaskCreate(fetch_task, "market_fetch", 6144, NULL, 4, NULL);
    ESP_LOGI(TAG, "chart UI up");
}
