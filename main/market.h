#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MARKET_MAX_CLOSES 96  // 96 x 15m candles = 24h

typedef struct {
    float price;                       // reserved for one-shot price fetches
    float change_pct;                  // reserved for one-shot change fetches
    float closes[MARKET_MAX_CLOSES];   // candle close prices, oldest -> newest
    int   n_closes;                    // valid entries in closes[]
    bool  ok;                          // true if chart history loaded
} market_data_t;

typedef struct {
    bool  live_connected;
    bool  live_valid;
    bool  spot_valid;
    float live_price;
    float live_change_pct;
    float spot_price;
    uint32_t live_age_s;
    uint32_t spot_age_s;
    uint32_t ws_connect_count;
    uint32_t ws_disconnect_count;
    uint32_t spot_fail_count;
} market_status_t;

// Blocking: fetches 15m klines for PAXGUSDT over HTTPS (Binance public
// market-data host). Fills *out. Returns true when chart history was loaded.
// Do NOT call under the LVGL lock — this blocks on the network.
bool market_fetch_history(market_data_t *out);

// Opens a persistent WebSocket to the Binance <symbol>@ticker stream, which
// pushes a 24h rolling-window stat (last price + 24h change %) once per second.
// Spawns its own client task; safe to call once after Wi-Fi is up.
void market_live_start(void);

// Latest values received on the live stream. Returns true once at least one
// ticker message has been parsed (price/change_pct then valid). Thread-safe.
bool market_live_get(float *price, float *change_pct);

// True while the live WebSocket is connected (false before first connect or
// during a reconnect). Thread-safe.
bool market_live_connected(void);

// Latest true-spot XAU/USD price from gold-api (the number matching
// TradingView). Returns true once a price has been fetched. Thread-safe.
bool market_spot_get(float *price);

// Snapshot of live/spot source health and latest values. Thread-safe.
void market_status_get(market_status_t *out);

#ifdef __cplusplus
}
#endif
