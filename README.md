# ESP32-C6 PAXG/USD Gold Chart

A live gold-price chart on a **Waveshare ESP32-C6-LCD-1.47** (ST7789T, 172x320).
Renders a 24h candle-close line chart, current price, and 24h % change with a
dark theme, driven by LVGL 9.

## "Gold proxy" — why PAXG?

Spot gold (XAUUSD) trades on a 5-day FX week and most free spot feeds are
auth-gated or geo-blocked. This firmware instead tracks **PAXGUSDT** on Binance:
Paxos Gold (PAXG) is a token redeemable 1:1 for a troy ounce of LBMA-certified
gold, so its USD price closely tracks spot and trades **24/7**. It is a proxy,
not an exact spot quote.

Swapping to a true XAUUSD feed (e.g. Twelve Data, OANDA) only requires changing
the fetch URLs and JSON parsing in `main/market.c` — the UI consumes a plain
`market_data_t` (price, 24h %, close[] series) and doesn't care about the source.

## Data source

Public Binance market-data host **`data-api.binance.vision`** (no API key, no
geo-block issues):

- Candles: `GET /api/v3/klines?symbol=PAXGUSDT&interval=15m&limit=96` (96 x 15m = 24h)
- Spot price: `GET /api/v3/ticker/price?symbol=PAXGUSDT`
- 24h change: `GET /api/v3/ticker/24hr?symbol=PAXGUSDT`

Fetched over HTTPS via `esp_http_client` with the mbedTLS cert bundle, parsed
with cJSON. The UI refreshes every ~45s; the network fetch runs in a background
task outside the LVGL lock.

## Hardware

Waveshare ESP32-C6-LCD-1.47, 8MB flash, no PSRAM. Pinout in `main/board.h`
(SPI2, MOSI=6 SCLK=7 CS=14 DC=15 RST=21 BL=22, ST7789T 172x320, X-offset 34).

## Build & flash

Requires ESP-IDF v5.4.

```sh
. ~/esp/esp-idf-v5.4/export.sh
cd esp32c6-xauusd-chart

# 1. Wi-Fi credentials
cp main/secrets.example.h main/secrets.h
$EDITOR main/secrets.h            # set WIFI_SSID / WIFI_PASS

idf.py set-target esp32c6
idf.py build
idf.py -p /dev/tty.usbmodemXXX flash monitor
```

`main/secrets.h` is gitignored. The committed `main/secrets.example.h` is the
template.

## Layout

| File | Role |
| --- | --- |
| `main/main.c` | boot: nvs -> led off -> lcd -> wifi -> chart UI |
| `main/lcd.c/.h`, `main/Vernon_ST7789T.c/.h`, `main/board.h` | ST7789T panel driver + pinout (reused from sibling project) |
| `main/wifi.c/.h` | plain Wi-Fi STA with auto-reconnect |
| `main/market.c/.h` | HTTPS fetch + JSON parse of PAXGUSDT data |
| `main/ui_chart.c/.h` | LVGL dark-theme chart UI + refresh task |
