#include "ui_chart.h"

#include <stdio.h>
#include <time.h>

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
static lv_obj_t *lbl_clock;
static lv_obj_t *lbl_footer;
static lv_obj_t *dot_live;   // small circle: green = WS connected, grey = down
static lv_obj_t *lbl_live;   // "LIVE" text next to the dot
static lv_obj_t *chart;
static lv_chart_series_t *series;

// Index of the chart's final loaded point (and its close value), so the live
// ticker can nudge just the tip without reloading the whole series.
static int   s_last_idx = -1;
static float s_last_close;

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
    lv_label_set_text(title, "XAU / USD");

    lv_obj_t *sub = mk_label(scr, &lv_font_montserrat_14, COL_GREY);
    lv_label_set_text(sub, "gold spot");

    lbl_clock = mk_label(scr, &lv_font_montserrat_14, COL_CYAN);
    lv_label_set_text(lbl_clock, "syncing time...");

    lbl_price = mk_label(scr, &lv_font_montserrat_28, COL_TEXT);
    lv_label_set_text(lbl_price, "----.--");

    // Change line: percent on the left, a LIVE dot + label pinned to the right.
    lv_obj_t *row = lv_obj_create(scr);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 5, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lbl_change = lv_label_create(row);
    lv_obj_set_style_text_font(lbl_change, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(lbl_change, lv_color_hex(COL_GREY), 0);
    lv_obj_set_flex_grow(lbl_change, 1);
    lv_label_set_text(lbl_change, "+0.00%");

    dot_live = lv_obj_create(row);
    lv_obj_remove_style_all(dot_live);
    lv_obj_set_size(dot_live, 10, 10);
    lv_obj_set_style_radius(dot_live, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(dot_live, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(dot_live, lv_color_hex(COL_GREY), 0);

    lbl_live = lv_label_create(row);
    lv_obj_set_style_text_font(lbl_live, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_live, lv_color_hex(COL_GREY), 0);
    lv_label_set_text(lbl_live, "LIVE");

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

// Render the headline price + 24h change labels. Must run under the LVGL lock.
// libc snprintf for %f — LVGL's lv_snprintf drops floats unless
// LV_SPRINTF_USE_FLOAT is set, rendering "$f" / "+f%" instead of numbers.
static void render_price(float price, float change_pct, bool have_change)
{
    char buf[32];
    if (price > 0.0f) {
        snprintf(buf, sizeof(buf), "$%.2f", price);
        lv_label_set_text(lbl_price, buf);
    }
    if (have_change) {
        bool pos = change_pct >= 0.0f;
        snprintf(buf, sizeof(buf), "%s%.2f%% 24h", pos ? "+" : "", change_pct);
        lv_label_set_text(lbl_change, buf);
        lv_obj_set_style_text_color(lbl_change,
                                    lv_color_hex(pos ? COL_GREEN : COL_RED), 0);
    }
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
        s_last_close = d->closes[d->n_closes - 1];
        s_last_idx   = d->n_closes - 1;
    }

    render_price(d->price, d->change_pct, d->ok);
}

// 1Hz timer: ticks the clock line and refreshes the footer (IP / RSSI / age).
static void footer_cb(lv_timer_t *t)
{
    (void)t;

    // Clock — only show once SNTP has stepped the RTC past the 2020 epoch.
    time_t now = time(NULL);
    if (now > 1600000000) {
        struct tm tm;
        localtime_r(&now, &tm);
        char ts[32];
        strftime(ts, sizeof(ts), "%a %d %b  %H:%M:%S", &tm);
        lv_label_set_text(lbl_clock, ts);
    } else {
        lv_label_set_text(lbl_clock, "syncing time...");
    }

    if (wifi_is_connected()) {
        lv_label_set_text_fmt(lbl_footer, "%s   %ddBm", wifi_ip(), wifi_rssi());
    } else {
        lv_label_set_text(lbl_footer, "Wi-Fi down");
    }

    // LIVE indicator tracks the WebSocket: green when streaming, grey when down.
    uint32_t live_col = market_live_connected() ? COL_GREEN : COL_GREY;
    lv_obj_set_style_bg_color(dot_live, lv_color_hex(live_col), 0);
    lv_obj_set_style_text_color(lbl_live, lv_color_hex(live_col), 0);
}

// Fast timer: pull the latest WebSocket ticker value and update the headline
// price/change live, plus nudge the chart's last point so the line tip tracks.
static void live_cb(lv_timer_t *t)
{
    (void)t;
    float paxg, change, spot;
    bool have_live = market_live_get(&paxg, &change);
    bool have_spot = market_spot_get(&spot);

    // Headline = true spot (gold-api); fall back to the PAXG tick until spot
    // arrives. 24h change always comes from PAXG (gold moves with it).
    if (have_spot) {
        render_price(spot, change, have_live);
    } else if (have_live) {
        render_price(paxg, change, true);
    }

    // Chart tip tracks the live PAXG tick (the series is loaded in PAXG space).
    if (have_live && s_last_idx >= 0) {
        lv_chart_set_value_by_id(chart, series, s_last_idx,
                                 (int32_t)(paxg + 0.5f));
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

    bool live_started = false;
    while (true) {
        // Wait for Wi-Fi before hammering the network.
        if (!wifi_is_connected()) {
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        // Open the live WebSocket once, after the link (and thus DNS) is up.
        if (!live_started) {
            market_live_start();
            live_started = true;
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
    lv_timer_create(live_cb, 500, NULL);
    footer_cb(NULL);
    lvgl_port_unlock();

    xTaskCreate(fetch_task, "market_fetch", 6144, NULL, 4, NULL);
    ESP_LOGI(TAG, "chart UI up");
}
