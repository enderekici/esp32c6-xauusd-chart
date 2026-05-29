#include "market.h"

#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"

static const char *TAG = "market";

#define MARKET_HOST "https://data-api.binance.vision"
#define URL_KLINES  MARKET_HOST "/api/v3/klines?symbol=PAXGUSDT&interval=15m&limit=96"
#define URL_PRICE   MARKET_HOST "/api/v3/ticker/price?symbol=PAXGUSDT"
#define URL_24HR    MARKET_HOST "/api/v3/ticker/24hr?symbol=PAXGUSDT"

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

static bool parse_price(const char *json, float *out)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) return false;
    bool ok = false;
    cJSON *p = cJSON_GetObjectItemCaseSensitive(root, "price");
    if (cJSON_IsString(p) && p->valuestring) {
        *out = (float)atof(p->valuestring);
        ok = true;
    }
    cJSON_Delete(root);
    return ok;
}

static bool parse_change(const char *json, float *out)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) return false;
    bool ok = false;
    cJSON *c = cJSON_GetObjectItemCaseSensitive(root, "priceChangePercent");
    if (cJSON_IsString(c) && c->valuestring) {
        *out = (float)atof(c->valuestring);
        ok = true;
    }
    cJSON_Delete(root);
    return ok;
}

// klines is a JSON array of arrays; element index 4 is the close (string).
static int parse_klines(const char *json, float *closes, int max)
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
            closes[n++] = (float)atof(close->valuestring);
        }
    }
    cJSON_Delete(root);
    return n;
}

bool market_fetch(market_data_t *out)
{
    memset(out, 0, sizeof(*out));

    char *buf = malloc(RESP_CAP);
    if (!buf) {
        ESP_LOGE(TAG, "buf alloc failed");
        return false;
    }

    bool ok = true;

    // Spot price.
    int n = http_get(URL_PRICE, buf, RESP_CAP);
    if (n > 0 && parse_price(buf, &out->price)) {
        // ok
    } else {
        ESP_LOGW(TAG, "price fetch/parse failed");
        ok = false;
    }

    // 24h change percent.
    n = http_get(URL_24HR, buf, RESP_CAP);
    if (n > 0 && parse_change(buf, &out->change_pct)) {
        // ok
    } else {
        ESP_LOGW(TAG, "24hr fetch/parse failed");
        ok = false;
    }

    // Candles.
    n = http_get(URL_KLINES, buf, RESP_CAP);
    if (n > 0) {
        out->n_closes = parse_klines(buf, out->closes, MARKET_MAX_CLOSES);
        if (out->n_closes == 0) {
            ESP_LOGW(TAG, "klines parse yielded 0 points");
            ok = false;
        }
    } else {
        ESP_LOGW(TAG, "klines fetch failed");
        ok = false;
    }

    free(buf);
    out->ok = ok;
    ESP_LOGI(TAG, "fetch ok=%d price=%.2f chg=%.2f%% pts=%d",
             ok, out->price, out->change_pct, out->n_closes);
    return ok;
}
