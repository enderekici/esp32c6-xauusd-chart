#include "ui_chart.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "board.h"
#include "wifi.h"
#include "market.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "nvs.h"
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

#define HISTORY_REFRESH_PERIOD_MS (15 * 60 * 1000)
#define HISTORY_RETRY_PERIOD_MS   30000
#define LIVE_UI_PERIOD_MS         1000
#define FOOTER_PERIOD_MS          5000
#define BACKLIGHT_DAY_PERCENT     30
#define BACKLIGHT_NIGHT_PERCENT   6
#define SETTINGS_NS               "chart_ui"
#define SETTINGS_KEY_MODE         "mode"
#define SETTINGS_KEY_BRIGHTNESS   "bright"

static lv_obj_t *lbl_price;
static lv_obj_t *lbl_change;
static lv_obj_t *lbl_clock;
static lv_obj_t *lbl_footer;
static lv_obj_t *dot_live;   // small circle: green = WS connected, grey = down
static lv_obj_t *lbl_live;   // "LIVE" text next to the dot
static lv_obj_t *chart;
static lv_chart_series_t *series;
static lv_chart_series_t *series_hi;   // flat 24h-high reference line
static lv_chart_series_t *series_lo;   // flat 24h-low reference line
static lv_obj_t *lbl_hi;               // "H ####" pinned top-right of chart
static lv_obj_t *lbl_lo;               // "L ####" pinned bottom-right of chart
static lv_obj_t *vol_chart;            // volume histogram pane below the price chart
static lv_chart_series_t *vol_series;
static uint32_t s_fill_color = COL_CYAN;  // glow-fill color, tracks the price line

// Last hi/lo (PAXG space) drawn into the flat reference series + the point
// count they were drawn at, so the overlay only rewrites on change.
static float    s_drawn_hi = -1.0f;
static float    s_drawn_lo = -1.0f;
static uint32_t s_drawn_pc;
static SemaphoreHandle_t s_state_mtx;
static TaskHandle_t s_fetch_task_handle;
static ui_chart_display_mode_t s_display_mode = UI_CHART_DISPLAY_AUTO;
static uint8_t s_configured_percent = BACKLIGHT_DAY_PERCENT;
static uint8_t s_backlight_percent = BACKLIGHT_DAY_PERCENT;
static int64_t s_wake_until_us;
static uint32_t s_history_success_count;
static uint32_t s_history_fail_count;
static int s_chart_points;
static bool s_history_valid;

// Index of the chart's final loaded point (and its close value), so the live
// ticker can nudge just the tip without reloading the whole series.
static int   s_last_idx = -1;
static float s_last_close;

// Wall-clock (esp_timer) seconds of the last successful data update.
static int64_t s_last_update_us;

static void load_display_settings(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(SETTINGS_NS, NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        return;
    }

    uint8_t mode = UI_CHART_DISPLAY_AUTO;
    uint8_t brightness = BACKLIGHT_DAY_PERCENT;
    nvs_get_u8(nvs, SETTINGS_KEY_MODE, &mode);
    nvs_get_u8(nvs, SETTINGS_KEY_BRIGHTNESS, &brightness);
    nvs_close(nvs);

    if (mode <= UI_CHART_DISPLAY_CUSTOM && brightness <= 100) {
        xSemaphoreTake(s_state_mtx, portMAX_DELAY);
        s_display_mode = (ui_chart_display_mode_t)mode;
        s_configured_percent = brightness;
        xSemaphoreGive(s_state_mtx);
        ESP_LOGI(TAG, "loaded display settings: mode=%s brightness=%u%%",
                 ui_chart_display_mode_name(s_display_mode), (unsigned)brightness);
    }
}

static void save_display_settings(ui_chart_display_mode_t mode, uint8_t brightness)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(SETTINGS_NS, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "settings open failed: %s", esp_err_to_name(err));
        return;
    }
    err = nvs_set_u8(nvs, SETTINGS_KEY_MODE, (uint8_t)mode);
    if (err == ESP_OK) err = nvs_set_u8(nvs, SETTINGS_KEY_BRIGHTNESS, brightness);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "settings save failed: %s", esp_err_to_name(err));
    }
}

static lv_obj_t *mk_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(l, lv_pct(100));
    return l;
}

// Fill the area under the price line with a vertical gradient (line color at the
// top fading to transparent at the baseline) for a Binance-style glow. Drawn in
// the POST phase, on top of the line; fill color == line color so the line stays
// crisp. At ~96 points over ~150px each segment is ~1-2px wide, so a flat-top
// rect per segment tracks the slope closely enough to be invisible under the line.
static void chart_glow_cb(lv_event_t *e)
{
    if (s_last_idx < 0) return;  // no data loaded yet
    lv_obj_t *c = lv_event_get_target_obj(e);
    uint32_t pc = lv_chart_get_point_count(c);
    if (pc < 2) return;

    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t coords, content;
    lv_obj_get_coords(c, &coords);
    lv_obj_get_content_coords(c, &content);
    int32_t baseline = content.y2;

    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_opa = LV_OPA_COVER;
    d.bg_grad.dir = LV_GRAD_DIR_VER;
    d.bg_grad.stops_count = 2;
    d.bg_grad.stops[0].color = lv_color_hex(s_fill_color);
    d.bg_grad.stops[0].opa = LV_OPA_40;
    d.bg_grad.stops[0].frac = 0;
    d.bg_grad.stops[1].color = lv_color_hex(s_fill_color);
    d.bg_grad.stops[1].opa = LV_OPA_TRANSP;
    d.bg_grad.stops[1].frac = 255;

    lv_point_t p0, p1;
    lv_chart_get_point_pos_by_id(c, series, 0, &p0);
    for (uint32_t i = 1; i < pc; i++) {
        lv_chart_get_point_pos_by_id(c, series, i, &p1);
        lv_area_t a;
        a.x1 = coords.x1 + p0.x;
        a.x2 = coords.x1 + p1.x;
        a.y1 = coords.y1 + (p0.y < p1.y ? p0.y : p1.y);  // higher of the two points
        a.y2 = baseline;
        if (a.y2 > a.y1) lv_draw_rect(layer, &d, &a);
        p0 = p1;
    }
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
    // Subtle vertical gradient (card -> background) for a touch of depth.
    lv_obj_set_style_bg_grad_color(chart, lv_color_hex(COL_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(chart, LV_GRAD_DIR_VER, LV_PART_MAIN);
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
    // Flat 24h high/low reference lines, dimmed so they read as guides behind
    // the price line. Hidden (POINT_NONE) until live hi/lo arrives.
    series_hi = lv_chart_add_series(chart, lv_color_hex(COL_BORDER),
                                    LV_CHART_AXIS_PRIMARY_Y);
    series_lo = lv_chart_add_series(chart, lv_color_hex(COL_BORDER),
                                    LV_CHART_AXIS_PRIMARY_Y);
    lv_chart_set_all_value(chart, series_hi, LV_CHART_POINT_NONE);
    lv_chart_set_all_value(chart, series_lo, LV_CHART_POINT_NONE);
    lv_obj_set_style_line_width(chart, 2, LV_PART_ITEMS);

    // H / L value labels pinned to the chart corners (spot-space numbers).
    lbl_hi = lv_label_create(chart);
    lv_obj_set_style_text_font(lbl_hi, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_hi, lv_color_hex(COL_GREY), 0);
    lv_obj_align(lbl_hi, LV_ALIGN_TOP_RIGHT, -2, 2);
    lv_label_set_text(lbl_hi, "");

    lbl_lo = lv_label_create(chart);
    lv_obj_set_style_text_font(lbl_lo, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_lo, lv_color_hex(COL_GREY), 0);
    lv_obj_align(lbl_lo, LV_ALIGN_BOTTOM_RIGHT, -2, -2);
    lv_label_set_text(lbl_lo, "");

    // Glow fill under the price line (custom draw in the POST phase).
    lv_obj_add_event_cb(chart, chart_glow_cb, LV_EVENT_DRAW_POST_BEGIN, NULL);

    // Volume histogram pane, Binance-style, beneath the price chart.
    vol_chart = lv_chart_create(scr);
    lv_obj_set_width(vol_chart, lv_pct(100));
    lv_obj_set_height(vol_chart, 44);
    lv_obj_set_style_bg_color(vol_chart, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_bg_opa(vol_chart, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(vol_chart, lv_color_hex(COL_BORDER), 0);
    lv_obj_set_style_border_width(vol_chart, 1, 0);
    lv_obj_set_style_radius(vol_chart, 8, 0);
    lv_obj_set_style_pad_all(vol_chart, 3, 0);
    lv_chart_set_div_line_count(vol_chart, 0, 0);
    lv_chart_set_type(vol_chart, LV_CHART_TYPE_BAR);
    lv_chart_set_update_mode(vol_chart, LV_CHART_UPDATE_MODE_SHIFT);
    lv_chart_set_point_count(vol_chart, MARKET_MAX_CLOSES);
    vol_series = lv_chart_add_series(vol_chart, lv_color_hex(COL_GREY),
                                     LV_CHART_AXIS_PRIMARY_Y);
    lv_obj_set_style_pad_column(vol_chart, 0, LV_PART_MAIN);  // touching bars

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

// Draw the flat 24h high/low reference lines (PAXG/chart space) and refresh the
// H/L corner labels (offset into spot space so they match the headline). Must
// run under the LVGL lock.
static void update_hilo_overlay(void)
{
    float hi, lo;
    if (!market_live_hilo(&hi, &lo)) return;

    uint32_t pc = lv_chart_get_point_count(chart);
    if (hi != s_drawn_hi || lo != s_drawn_lo || pc != s_drawn_pc) {
        for (uint32_t i = 0; i < pc; i++) {
            lv_chart_set_value_by_id(chart, series_hi, i, (int32_t)(hi + 0.5f));
            lv_chart_set_value_by_id(chart, series_lo, i, (int32_t)(lo + 0.5f));
        }
        s_drawn_hi = hi;
        s_drawn_lo = lo;
        s_drawn_pc = pc;
    }

    // Labels in spot space: shift by the live spot/PAXG gap when both are known.
    float delta = 0.0f, spot, paxg, change;
    if (market_spot_get(&spot) && market_live_get(&paxg, &change)) {
        delta = spot - paxg;
    }
    char b[16];
    snprintf(b, sizeof(b), "H %.0f", hi + delta);
    lv_label_set_text(lbl_hi, b);
    snprintf(b, sizeof(b), "L %.0f", lo + delta);
    lv_label_set_text(lbl_lo, b);
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
        // Stretch the range to include the live 24h high/low so the reference
        // lines (and any wick beyond the closes) stay on-screen.
        float hh, ll;
        if (market_live_hilo(&hh, &ll)) {
            if (hh > hi) hi = hh;
            if (ll < lo) lo = ll;
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
        update_hilo_overlay();  // refill flat ref lines at the new point count
        lv_chart_refresh(chart);

        // Color price line + change by direction over the window. The glow fill
        // tracks the same color.
        bool up = d->closes[d->n_closes - 1] >= d->closes[0];
        s_fill_color = up ? COL_GREEN : COL_CYAN;
        lv_chart_set_series_color(chart, series, lv_color_hex(s_fill_color));
        s_last_close = d->closes[d->n_closes - 1];
        s_last_idx   = d->n_closes - 1;

        // Volume histogram: scale to the window's max so bars use the full pane.
        float vmax = 0.0f;
        for (int i = 0; i < d->n_closes; i++) {
            if (d->volumes[i] > vmax) vmax = d->volumes[i];
        }
        if (vmax <= 0.0f) vmax = 1.0f;
        lv_chart_set_range(vol_chart, LV_CHART_AXIS_PRIMARY_Y, 0, (int32_t)(vmax + 0.5f));
        lv_chart_set_point_count(vol_chart, d->n_closes);
        for (int i = 0; i < d->n_closes; i++) {
            lv_chart_set_value_by_id(vol_chart, vol_series, i,
                                     (int32_t)(d->volumes[i] + 0.5f));
        }
        lv_chart_refresh(vol_chart);
    }

    float paxg, change, spot;
    bool have_live = market_live_get(&paxg, &change);
    bool have_spot = market_spot_get(&spot);
    if (have_spot) {
        render_price(spot, change, have_live);
    } else if (have_live) {
        render_price(paxg, change, true);
    }
}

const char *ui_chart_display_mode_name(ui_chart_display_mode_t mode)
{
    switch (mode) {
    case UI_CHART_DISPLAY_AUTO:   return "auto";
    case UI_CHART_DISPLAY_DAY:    return "day";
    case UI_CHART_DISPLAY_DIM:    return "dim";
    case UI_CHART_DISPLAY_OFF:    return "off";
    case UI_CHART_DISPLAY_CUSTOM: return "custom";
    default:                      return "unknown";
    }
}

bool ui_chart_parse_display_mode(const char *mode, ui_chart_display_mode_t *out)
{
    if (!mode || !out) return false;
    if (strcmp(mode, "auto") == 0) {
        *out = UI_CHART_DISPLAY_AUTO;
    } else if (strcmp(mode, "day") == 0) {
        *out = UI_CHART_DISPLAY_DAY;
    } else if (strcmp(mode, "dim") == 0) {
        *out = UI_CHART_DISPLAY_DIM;
    } else if (strcmp(mode, "off") == 0) {
        *out = UI_CHART_DISPLAY_OFF;
    } else {
        return false;
    }
    return true;
}

static uint8_t compute_backlight_target(void)
{
    int64_t now_us = esp_timer_get_time();
    if (s_wake_until_us > now_us) return BACKLIGHT_DAY_PERCENT;

    ui_chart_display_mode_t mode = s_display_mode;
    uint8_t custom = s_configured_percent;
    switch (mode) {
    case UI_CHART_DISPLAY_DAY:
        return BACKLIGHT_DAY_PERCENT;
    case UI_CHART_DISPLAY_DIM:
        return BACKLIGHT_NIGHT_PERCENT;
    case UI_CHART_DISPLAY_OFF:
        return 0;
    case UI_CHART_DISPLAY_CUSTOM:
        return custom;
    case UI_CHART_DISPLAY_AUTO:
    default:
        break;
    }

    time_t now = time(NULL);
    if (now <= 1600000000) return BACKLIGHT_DAY_PERCENT;

    struct tm tm;
    localtime_r(&now, &tm);
    return (tm.tm_hour >= 22 || tm.tm_hour < 7)
        ? BACKLIGHT_NIGHT_PERCENT
        : BACKLIGHT_DAY_PERCENT;
}

static void apply_backlight_policy(void)
{
    xSemaphoreTake(s_state_mtx, portMAX_DELAY);
    uint8_t target = compute_backlight_target();
    if (target != s_backlight_percent) {
        if (lcd_set_backlight_percent(target) == ESP_OK) {
            s_backlight_percent = target;
            ESP_LOGI(TAG, "backlight %u%%", (unsigned)target);
        }
    }
    xSemaphoreGive(s_state_mtx);
}

bool ui_chart_set_display_mode(ui_chart_display_mode_t mode)
{
    if (mode < UI_CHART_DISPLAY_AUTO || mode > UI_CHART_DISPLAY_OFF) return false;
    uint8_t brightness = (mode == UI_CHART_DISPLAY_DAY) ? BACKLIGHT_DAY_PERCENT :
                         (mode == UI_CHART_DISPLAY_DIM) ? BACKLIGHT_NIGHT_PERCENT :
                         (mode == UI_CHART_DISPLAY_OFF) ? 0 :
                         BACKLIGHT_DAY_PERCENT;
    xSemaphoreTake(s_state_mtx, portMAX_DELAY);
    s_display_mode = mode;
    s_configured_percent = brightness;
    s_wake_until_us = 0;
    xSemaphoreGive(s_state_mtx);
    save_display_settings(mode, brightness);
    apply_backlight_policy();
    return true;
}

bool ui_chart_set_custom_brightness(uint8_t percent)
{
    if (percent > 100) return false;
    xSemaphoreTake(s_state_mtx, portMAX_DELAY);
    s_display_mode = UI_CHART_DISPLAY_CUSTOM;
    s_configured_percent = percent;
    s_wake_until_us = 0;
    xSemaphoreGive(s_state_mtx);
    save_display_settings(UI_CHART_DISPLAY_CUSTOM, percent);
    apply_backlight_policy();
    return true;
}

void ui_chart_wake_for(uint32_t seconds)
{
    if (seconds == 0) seconds = 60;
    if (seconds > 3600) seconds = 3600;
    xSemaphoreTake(s_state_mtx, portMAX_DELAY);
    s_wake_until_us = esp_timer_get_time() + (int64_t)seconds * 1000000;
    xSemaphoreGive(s_state_mtx);
    apply_backlight_policy();
}

void ui_chart_force_refresh(void)
{
    if (s_fetch_task_handle) {
        xTaskNotifyGive(s_fetch_task_handle);
    }
}

void ui_chart_status_get(ui_chart_status_t *out)
{
    memset(out, 0, sizeof(*out));
    int64_t now = esp_timer_get_time();
    out->uptime_s = (uint32_t)(now / 1000000);

    xSemaphoreTake(s_state_mtx, portMAX_DELAY);
    out->mode = s_display_mode;
    out->configured_percent = s_configured_percent;
    out->effective_percent = s_backlight_percent;
    if (s_wake_until_us > now) {
        out->wake_remaining_s = (uint32_t)((s_wake_until_us - now + 999999) / 1000000);
    }
    out->history_success_count = s_history_success_count;
    out->history_fail_count = s_history_fail_count;
    out->chart_points = s_chart_points;
    out->history_valid = s_history_valid;
    if (s_last_update_us > 0) {
        out->last_history_age_s = (uint32_t)((now - s_last_update_us) / 1000000);
    }
    xSemaphoreGive(s_state_mtx);
}

// Slow timer: ticks the clock line, refreshes the footer, and applies dimming.
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
    apply_backlight_policy();

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

    // Refresh the 24h high/low reference lines + labels (only redraws on change).
    update_hilo_overlay();
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
        bool ok = market_fetch_history(d);

        lvgl_port_lock(0);
        apply_data(d);
        lvgl_port_unlock();

        if (ok) {
            xSemaphoreTake(s_state_mtx, portMAX_DELAY);
            s_last_update_us = esp_timer_get_time();
            s_history_success_count++;
            s_chart_points = d->n_closes;
            s_history_valid = true;
            xSemaphoreGive(s_state_mtx);
        } else {
            xSemaphoreTake(s_state_mtx, portMAX_DELAY);
            s_history_fail_count++;
            xSemaphoreGive(s_state_mtx);
        }

        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(ok ? HISTORY_REFRESH_PERIOD_MS : HISTORY_RETRY_PERIOD_MS));
    }
}

void ui_chart_start(lcd_t *lcd)
{
    s_state_mtx = xSemaphoreCreateMutex();
    load_display_settings();
    apply_backlight_policy();

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
    lv_timer_create(footer_cb, FOOTER_PERIOD_MS, NULL);
    lv_timer_create(live_cb, LIVE_UI_PERIOD_MS, NULL);
    footer_cb(NULL);
    lvgl_port_unlock();

    xTaskCreate(fetch_task, "market_fetch", 12288, NULL, 4, &s_fetch_task_handle);
    ESP_LOGI(TAG, "chart UI up");
}
