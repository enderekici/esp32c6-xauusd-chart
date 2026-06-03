#include "market.h"

#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_http_client.h"
#include "esp_websocket_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"

static const char *TAG = "market";

#define MARKET_HOST "https://data-api.binance.vision"
#define URL_KLINES  MARKET_HOST "/api/v3/klines?symbol=PAXGUSDT&interval=15m&limit=96"

// Market-data-only WebSocket host (mirror of stream.binance.com, no key/geo
// block). The @ticker stream pushes a 24h rolling stat once per second.
#define WS_URI "wss://data-stream.binance.vision/ws/paxgusdt@ticker"

// Keyless true-spot XAU/USD (matches the OANDA print on TradingView). Price
// only, no history/change — polled for the headline number.
#define URL_SPOT "https://api.gold-api.com/price/XAU"
#define SPOT_POLL_MS 60000

// klines for 96 candles is several KB; ticker responses are tiny. Use one
// generous heap buffer for all three requests.
#define RESP_CAP (16 * 1024)

typedef struct {
    char  *buf;
    int    len;
    int    cap;
} resp_ctx_t;

static esp_err_t http_event_cb(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        resp_ctx_t *ctx = (resp_ctx_t *)evt->user_data;
        if (ctx && ctx->buf) {
            int copy = evt->data_len;
            if (ctx->len + copy > ctx->cap - 1) {
                copy = ctx->cap - 1 - ctx->len;
            }
            if (copy > 0) {
                memcpy(ctx->buf + ctx->len, evt->data, copy);
                ctx->len += copy;
            }
        }
    }
    return ESP_OK;
}

// GET url into buf (capacity cap). Returns bytes read, or -1 on error.
static int http_get(const char *url, char *buf, int cap)
{
    resp_ctx_t ctx = { .buf = buf, .len = 0, .cap = cap };
    esp_http_client_config_t cfg = {
        .url               = url,
        .event_handler     = http_event_cb,
        .user_data         = &ctx,
        .timeout_ms        = 8000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size       = 2048,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return -1;

    int out = -1;
    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        int status = esp_http_client_get_status_code(client);
        if (status == 200) {
            buf[ctx.len] = 0;
            out = ctx.len;
        } else {
            ESP_LOGW(TAG, "%s -> HTTP %d", url, status);
        }
    } else {
        ESP_LOGW(TAG, "%s -> %s", url, esp_err_to_name(err));
    }
    esp_http_client_cleanup(client);
    return out;
}

// klines is a JSON array of arrays; index 4 is the close, index 5 the base-asset
// volume (both strings).
static int parse_klines(const char *json, float *closes, float *volumes, int max)
{
    cJSON *root = cJSON_Parse(json);
    if (!root || !cJSON_IsArray(root)) {
        if (root) cJSON_Delete(root);
        return 0;
    }
    int n = 0;
    cJSON *candle = NULL;
    cJSON_ArrayForEach(candle, root) {
        if (n >= max) break;
        if (!cJSON_IsArray(candle)) continue;
        cJSON *close = cJSON_GetArrayItem(candle, 4);
        if (cJSON_IsString(close) && close->valuestring) {
            cJSON *vol = cJSON_GetArrayItem(candle, 5);
            volumes[n] = (cJSON_IsString(vol) && vol->valuestring)
                       ? (float)atof(vol->valuestring) : 0.0f;
            closes[n++] = (float)atof(close->valuestring);
        }
    }
    cJSON_Delete(root);
    return n;
}

bool market_fetch_history(market_data_t *out)
{
    memset(out, 0, sizeof(*out));

    char *buf = malloc(RESP_CAP);
    if (!buf) {
        ESP_LOGE(TAG, "buf alloc failed");
        return false;
    }

    bool ok = false;
    int n = http_get(URL_KLINES, buf, RESP_CAP);
    if (n > 0) {
        out->n_closes = parse_klines(buf, out->closes, out->volumes, MARKET_MAX_CLOSES);
        if (out->n_closes > 0) {
            ok = true;
        } else {
            ESP_LOGW(TAG, "klines parse yielded 0 points");
        }
    } else {
        ESP_LOGW(TAG, "klines fetch failed");
    }

    free(buf);
    out->ok = ok;
    ESP_LOGI(TAG, "history fetch ok=%d pts=%d", ok, out->n_closes);
    return ok;
}

// ---- Live WebSocket ticker -------------------------------------------------

static SemaphoreHandle_t   s_live_mtx;
static float               s_live_price;
static float               s_live_change;
static float               s_live_high;
static float               s_live_low;
static bool                s_live_valid;
static bool                s_hilo_valid;
static float               s_spot_price;
static bool                s_spot_valid;
static volatile bool       s_live_connected;
static int64_t             s_last_live_us;
static int64_t             s_last_spot_us;
static uint32_t            s_ws_connect_count;
static uint32_t            s_ws_disconnect_count;
static uint32_t            s_spot_fail_count;
static esp_websocket_client_handle_t s_ws;

// Parse one @ticker frame: "c" = last price, "P" = 24h change %, "h"/"l" = 24h
// high/low. All fields are JSON strings.
static void parse_ticker(const char *json, int len)
{
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!root) return;
    cJSON *c = cJSON_GetObjectItemCaseSensitive(root, "c");
    cJSON *p = cJSON_GetObjectItemCaseSensitive(root, "P");
    if (cJSON_IsString(c) && c->valuestring && cJSON_IsString(p) && p->valuestring) {
        float price  = (float)atof(c->valuestring);
        float change = (float)atof(p->valuestring);
        cJSON *h = cJSON_GetObjectItemCaseSensitive(root, "h");
        cJSON *l = cJSON_GetObjectItemCaseSensitive(root, "l");
        bool have_hilo = cJSON_IsString(h) && h->valuestring &&
                         cJSON_IsString(l) && l->valuestring;
        xSemaphoreTake(s_live_mtx, portMAX_DELAY);
        bool first = !s_live_valid;
        s_live_price  = price;
        s_live_change = change;
        s_live_valid  = true;
        if (have_hilo) {
            s_live_high  = (float)atof(h->valuestring);
            s_live_low   = (float)atof(l->valuestring);
            s_hilo_valid = true;
        }
        s_last_live_us = esp_timer_get_time();
        xSemaphoreGive(s_live_mtx);
        if (first) {
            ESP_LOGI(TAG, "live tick: price=%.2f chg=%.2f%%", price, change);
        }
    }
    cJSON_Delete(root);
}

// gold-api returns {"price":4511.10,...} — price is a JSON number, not string.
static bool parse_spot(const char *json, float *out)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) return false;
    bool ok = false;
    cJSON *p = cJSON_GetObjectItemCaseSensitive(root, "price");
    if (cJSON_IsNumber(p)) {
        *out = (float)p->valuedouble;
        ok = true;
    }
    cJSON_Delete(root);
    return ok;
}

static void spot_task(void *arg)
{
    (void)arg;
    char *buf = malloc(1024);
    if (!buf) {
        ESP_LOGE(TAG, "spot buf alloc failed");
        vTaskDelete(NULL);
        return;
    }
    while (true) {
        int n = http_get(URL_SPOT, buf, 1024);
        float price;
        if (n > 0 && parse_spot(buf, &price)) {
            xSemaphoreTake(s_live_mtx, portMAX_DELAY);
            bool first = !s_spot_valid;
            s_spot_price = price;
            s_spot_valid = true;
            s_last_spot_us = esp_timer_get_time();
            xSemaphoreGive(s_live_mtx);
            if (first) ESP_LOGI(TAG, "spot: XAU=%.2f", price);
        } else {
            xSemaphoreTake(s_live_mtx, portMAX_DELAY);
            s_spot_fail_count++;
            xSemaphoreGive(s_live_mtx);
        }
        vTaskDelay(pdMS_TO_TICKS(SPOT_POLL_MS));
    }
}

static void ws_event_cb(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base;
    esp_websocket_event_data_t *e = (esp_websocket_event_data_t *)data;
    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED:
        s_live_connected = true;
        if (s_live_mtx) {
            xSemaphoreTake(s_live_mtx, portMAX_DELAY);
            s_ws_connect_count++;
            xSemaphoreGive(s_live_mtx);
        }
        ESP_LOGI(TAG, "ws connected");
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
        s_live_connected = false;
        if (s_live_mtx) {
            xSemaphoreTake(s_live_mtx, portMAX_DELAY);
            s_ws_disconnect_count++;
            xSemaphoreGive(s_live_mtx);
        }
        ESP_LOGW(TAG, "ws disconnected");
        break;
    case WEBSOCKET_EVENT_DATA:
        // op_code 1 = text. A @ticker frame is small and arrives whole; ignore
        // pings/pongs and any zero-length control frames.
        if (e->op_code == 0x01 && e->data_len > 0 &&
            e->payload_offset == 0 && e->data_len == e->payload_len) {
            parse_ticker(e->data_ptr, e->data_len);
        }
        break;
    default:
        break;
    }
}

void market_live_start(void)
{
    if (s_ws) return;  // already running
    s_live_mtx = xSemaphoreCreateMutex();

    esp_websocket_client_config_t cfg = {
        .uri                  = WS_URI,
        .crt_bundle_attach    = esp_crt_bundle_attach,
        .reconnect_timeout_ms = 5000,
        .network_timeout_ms   = 10000,
        .buffer_size          = 2048,
        .task_stack           = 5120,
    };
    s_ws = esp_websocket_client_init(&cfg);
    if (!s_ws) {
        ESP_LOGE(TAG, "ws init failed");
        return;
    }
    esp_websocket_register_events(s_ws, WEBSOCKET_EVENT_ANY, ws_event_cb, NULL);
    esp_err_t err = esp_websocket_client_start(s_ws);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ws start: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "live ticker started (%s)", WS_URI);

    xTaskCreate(spot_task, "spot", 5120, NULL, 4, NULL);
}

bool market_live_get(float *price, float *change_pct)
{
    if (!s_live_mtx) return false;
    xSemaphoreTake(s_live_mtx, portMAX_DELAY);
    bool valid = s_live_valid;
    if (valid) {
        if (price)      *price = s_live_price;
        if (change_pct) *change_pct = s_live_change;
    }
    xSemaphoreGive(s_live_mtx);
    return valid;
}

bool market_live_connected(void)
{
    return s_live_connected;
}

bool market_live_hilo(float *high, float *low)
{
    if (!s_live_mtx) return false;
    xSemaphoreTake(s_live_mtx, portMAX_DELAY);
    bool valid = s_hilo_valid;
    if (valid) {
        if (high) *high = s_live_high;
        if (low)  *low  = s_live_low;
    }
    xSemaphoreGive(s_live_mtx);
    return valid;
}

bool market_spot_get(float *price)
{
    if (!s_live_mtx) return false;
    xSemaphoreTake(s_live_mtx, portMAX_DELAY);
    bool valid = s_spot_valid;
    if (valid && price) *price = s_spot_price;
    xSemaphoreGive(s_live_mtx);
    return valid;
}

void market_status_get(market_status_t *out)
{
    memset(out, 0, sizeof(*out));
    out->live_connected = s_live_connected;
    if (!s_live_mtx) return;

    int64_t now = esp_timer_get_time();
    xSemaphoreTake(s_live_mtx, portMAX_DELAY);
    out->live_valid = s_live_valid;
    out->spot_valid = s_spot_valid;
    out->live_price = s_live_price;
    out->live_change_pct = s_live_change;
    out->spot_price = s_spot_price;
    out->ws_connect_count = s_ws_connect_count;
    out->ws_disconnect_count = s_ws_disconnect_count;
    out->spot_fail_count = s_spot_fail_count;
    if (s_last_live_us > 0) {
        out->live_age_s = (uint32_t)((now - s_last_live_us) / 1000000);
    }
    if (s_last_spot_us > 0) {
        out->spot_age_s = (uint32_t)((now - s_last_spot_us) / 1000000);
    }
    xSemaphoreGive(s_live_mtx);
}
