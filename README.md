# ESP32-C6 XAU/USD Gold Chart

A live gold-price chart on a **Waveshare ESP32-C6-LCD-1.47** (ST7789T, 172x320).
Renders a 24h line chart with the current **spot** price, 24h % change, a London
clock, and a live-connection indicator, on a dark theme driven by LVGL 9.

## What it shows

```
XAU / USD            <- title
gold spot            <- subtitle
Thu 29 May  14:30:21 <- London (GMT/BST) clock, SNTP-synced
$4517.50             <- true spot price (headline)
+1.57% 24h    ● LIVE <- 24h change  +  WebSocket status dot
┌───────────────────┐
│      chart        │ <- 96-point 24h line, tip tracks live
└───────────────────┘
192.168.x.x   -52dBm <- IP + Wi-Fi RSSI
```

## Data sources — and why two of them

Gold has no single free feed that is **true spot + live + keyless + with
history**, so this firmware blends two keyless sources that move together:

| On screen | Source | Why |
| --- | --- | --- |
| **Headline price** | `api.gold-api.com/price/XAU` | True spot XAU/USD — the same ~4500 number TradingView shows (OANDA). Keyless, ~real-time. **Price only**: no history, no change. Polled every 6 s. |
| **24h % change** | Binance `PAXGUSDT` | PAXG (Paxos Gold token) tracks spot 24/7; its 24h move ≈ gold's. |
| **Chart (96 pts)** | Binance `PAXGUSDT` 15m klines | gold-api has no history; PAXG candles give the 24h shape. |
| **Live tick / LIVE dot** | Binance `PAXGUSDT` `@ticker` WebSocket | Pushes a 24h rolling stat **once per second** (not trade-driven), so the chart tip and LIVE indicator stay live. |

PAXG trades as a token and sits **~$5–15 off spot** (liquidity premium + USDT
peg), which is why the *headline* uses gold-api instead. The chart has no Y-axis
labels, so its internal PAXG scale (a few dollars below spot) is invisible.

### Sources that were evaluated and rejected

- **OANDA v20** — true spot, official streaming API, but requires an account.
- **Finnhub** — `OANDA:XAU_USD` is ideal, but forex/metals is a **paid** feature; the free tier returns `"You don't have access to this resource."`
- **Alpaca** — no forex/metals (US equities + crypto only). GLD ETF is a US-hours proxy at best.
- **Twelve Data** — true spot with history, but needs a free signup and the free tier caps refresh at ~1–2 min (no realtime metals WS).

Swapping the source only touches `main/market.c` — the UI consumes plain
getters (`market_spot_get`, `market_live_get`, `market_fetch`) and doesn't care
where the numbers come from.

## Hosts / endpoints

All over HTTPS/WSS with the mbedTLS cert bundle (`esp_crt_bundle`), parsed with
cJSON. Keyless, no geo-block.

- Spot: `GET https://api.gold-api.com/price/XAU` → `{"price":4517.5,...}`
- Klines: `GET https://data-api.binance.vision/api/v3/klines?symbol=PAXGUSDT&interval=15m&limit=96`
- 24h: `GET https://data-api.binance.vision/api/v3/ticker/24hr?symbol=PAXGUSDT`
- Live: `wss://data-stream.binance.vision/ws/paxgusdt@ticker` (fields `c` = last price, `P` = 24h %)

## Architecture

- **`market_fetch()`** (45 s task) — klines + 24h change over HTTPS, fills the chart history.
- **`spot_task`** (6 s) — polls gold-api for the headline spot price.
- **WebSocket** — persistent `@ticker` stream; `market_live_get()` returns the latest 1 Hz price/change, `market_live_connected()` drives the LIVE dot.
- **HTTP control server** — port 80 dashboard + JSON API for status, brightness,
  display mode, manual refresh, and OTA.
- **UI timers** — `live_cb` (500 ms) updates the headline/change/chart tip; `footer_cb` (1 s) ticks the clock and refreshes IP/RSSI + LIVE colour.
- All network I/O runs **outside** the LVGL lock; the UI is mutated under it.
- **Time** — SNTP (`pool.ntp.org`), timezone `Europe/London` (`GMT0BST,M3.5.0/1,M10.5.0`).

## Hardware

Waveshare ESP32-C6-LCD-1.47, 8MB flash, **no PSRAM**. Pinout in `main/board.h`
(SPI2, MOSI=6 SCLK=7 CS=14 DC=15 RST=21 BL=22, ST7789T 172x320, X-offset 34).
The onboard WS2812 RGB LED (GPIO8) is cleared at boot.

## Build & flash

Requires ESP-IDF v5.4.

```sh
. ~/esp/esp-idf-v5.4/export.sh
cd esp32c6-xauusd-chart

# Wi-Fi credentials (gitignored)
cp main/secrets.example.h main/secrets.h
$EDITOR main/secrets.h            # set WIFI_SSID / WIFI_PASS

idf.py set-target esp32c6
idf.py build
idf.py -p /dev/tty.usbmodemXXX flash monitor
```

`main/secrets.h` is gitignored; the committed `main/secrets.example.h` is the
template. No market-data API keys are needed.

The app uses an OTA partition table:

| Partition | Offset | Size | Purpose |
| --- | ---: | ---: | --- |
| `factory` | `0x20000` | `0x200000` | USB-flashed recovery image |
| `ota_0` | `0x220000` | `0x200000` | OTA slot |
| `ota_1` | `0x420000` | `0x200000` | OTA slot |

## Web dashboard and API

Once the board joins Wi-Fi, open its IP in a browser:

```text
http://192.168.x.x/
```

The dashboard shows live price, 24h change, firmware version, Wi-Fi status,
display state, source freshness, heap, OTA state, and raw JSON status. It also
provides controls for display mode, brightness, a 60 second wake, manual data
refresh, and firmware update by URL.

API endpoints:

| Method | Path | Purpose |
| --- | --- | --- |
| `GET` | `/` | Browser dashboard |
| `GET` | `/api/status` | Full JSON status, including embedded app version |
| `POST` | `/api/display?mode=auto|day|dim|off` | Change display mode |
| `POST` | `/api/brightness?percent=0..100` | Set configured brightness |
| `POST` | `/api/wake?seconds=60` | Temporarily wake/brighten the display |
| `POST` | `/api/refresh` | Force a market-data refresh |
| `POST` | `/api/ota?url=<encoded-url>` | Start OTA from an HTTP/HTTPS firmware URL |

Example status check:

```sh
curl http://192.168.x.x/api/status
```

The response includes:

```json
{
  "app": {
    "project": "xauusd_chart",
    "version": "build-<run>-<commit>",
    "date": "<compile-date>",
    "time": "<compile-time>",
    "idf": "v5.4.2"
  },
  "ota": {
    "state": "idle",
    "message": "idle"
  }
}
```

## OTA from GitHub Actions

The workflow at `.github/workflows/firmware.yml` builds the ESP-IDF app on every
push to `main`, uploads the firmware as a workflow artifact, and publishes OTA
files to the `gh-pages` branch:

- `xauusd_chart.bin` — app image used by OTA
- `version.json` — version, commit, SHA-256, size, and URL metadata
- `bootloader.bin` and `partition-table.bin` — reference files for USB recovery

The firmware version is generated by CI as:

```text
build-<github-run-number>-<short-commit>
```

The workflow passes that value as `APP_VERSION`; the top-level `CMakeLists.txt`
maps it to ESP-IDF `PROJECT_VER`. That means the GitHub Pages `version.json`,
the embedded ESP-IDF app descriptor, `/api/status`, and the dashboard Firmware
card all report the same build version.

Repository setup:

1. Add GitHub repository secrets `WIFI_SSID` and `WIFI_PASS`.
2. Enable GitHub Pages from the `gh-pages` branch, root folder.
3. After the first successful workflow run, the OTA URL is:

```text
https://enderekici.github.io/esp32c6-xauusd-chart/xauusd_chart.bin
```

Check the published metadata:

```sh
curl https://enderekici.github.io/esp32c6-xauusd-chart/version.json
```

The board can install the published firmware from the dashboard OTA box or with:

```sh
curl -X POST 'http://192.168.x.x/api/ota?url=https%3A%2F%2Fenderekici.github.io%2Fesp32c6-xauusd-chart%2Fxauusd_chart.bin'
```

GitHub Pages caches assets for a short time. When testing a just-published
build, append a cache-buster query string:

```sh
curl -X POST 'http://192.168.x.x/api/ota?url=https%3A%2F%2Fenderekici.github.io%2Fesp32c6-xauusd-chart%2Fxauusd_chart.bin%3Fnocache%3D<short-commit>'
```

Real OTA verification flow:

```sh
# 1. Confirm Pages is serving the expected commit/version.
curl 'https://enderekici.github.io/esp32c6-xauusd-chart/version.json?nocache=<short-commit>'

# 2. Trigger OTA through the same backend endpoint used by the web UI.
curl -X POST 'http://192.168.x.x/api/ota?url=https%3A%2F%2Fenderekici.github.io%2Fesp32c6-xauusd-chart%2Fxauusd_chart.bin%3Fnocache%3D<short-commit>'

# 3. Poll until the board reboots and reports the new embedded version.
curl http://192.168.x.x/api/status
```

USB flashing is still the recovery path if a future firmware breaks Wi-Fi or
OTA.

## LVGL display gotchas (ST7789T + esp_lvgl_port)

Two non-obvious flags in `ui_chart_start()` — both required:

- **`rotation.mirror_x = true`** — `esp_lvgl_port` re-applies the panel mirror
  from the rotation flags during `lvgl_port_add_disp`, overwriting the driver's
  `mirror(true)`. With it `false` the whole UI renders mirrored.
- **`flags.swap_bytes = false`** — `true` double-swaps the RGB565 byte order
  (navy background turns magenta, borders turn green).

Also: LVGL's `lv_snprintf`/`lv_label_set_text_fmt` **drop `%f`** unless
`LV_SPRINTF_USE_FLOAT` is set — float labels are formatted with libc `snprintf`
then `lv_label_set_text`, or they render as a literal `f` (`$f`, `+f%`).

## Layout

| File | Role |
| --- | --- |
| `main/main.c` | boot: nvs → LED off → lcd → wifi → SNTP → chart UI |
| `main/lcd.c/.h`, `main/Vernon_ST7789T.c/.h`, `main/board.h` | ST7789T panel driver + pinout |
| `main/wifi.c/.h` | Wi-Fi STA with auto-reconnect |
| `main/market.c/.h` | gold-api spot poll + Binance HTTPS fetch + `@ticker` WebSocket |
| `main/ui_chart.c/.h` | LVGL dark-theme chart UI + refresh/live timers |
| `main/http_control.c/.h` | port 80 dashboard + JSON control API |
| `main/ota_update.c/.h` | HTTP/HTTPS OTA task + OTA status tracking |
| `partitions.csv` | factory + two OTA app slots |
| `.github/workflows/firmware.yml` | CI build, version metadata, GitHub Pages OTA publish |
