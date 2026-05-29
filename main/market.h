#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MARKET_MAX_CLOSES 96  // 96 x 15m candles = 24h

typedef struct {
    float price;                       // current spot price (USD)
    float change_pct;                  // 24h change in percent
    float closes[MARKET_MAX_CLOSES];   // candle close prices, oldest -> newest
    int   n_closes;                    // valid entries in closes[]
    bool  ok;                          // true if all fetches+parses succeeded
} market_data_t;

// Blocking: fetches spot price, 24h change and 15m klines for PAXGUSDT over
// HTTPS (Binance public market-data host). Fills *out. Returns true on full
// success; on partial failure out->ok is false but populated fields are valid.
// Do NOT call under the LVGL lock — this blocks on the network.
bool market_fetch(market_data_t *out);

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

#ifdef __cplusplus
}
#endif
