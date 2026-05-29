#include "http_control.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_http_server.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "market.h"
#include "ota_update.h"
#include "ui_chart.h"
#include "wifi.h"

static const char *TAG = "http";
static httpd_handle_t s_server;

static void common_headers(httpd_req_t *req, const char *type)
{
    httpd_resp_set_type(req, type);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET,POST,OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
}

static bool query_value(httpd_req_t *req, const char *key, char *out, size_t out_len)
{
    char query[128];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        return false;
    }
    return httpd_query_key_value(query, key, out, out_len) == ESP_OK;
}

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool url_decode(const char *in, char *out, size_t out_len)
{
    size_t o = 0;
    for (size_t i = 0; in[i]; i++) {
        if (o + 1 >= out_len) return false;
        if (in[i] == '%' && in[i + 1] && in[i + 2]) {
            int hi = hex_val(in[i + 1]);
            int lo = hex_val(in[i + 2]);
            if (hi < 0 || lo < 0) return false;
            out[o++] = (char)((hi << 4) | lo);
            i += 2;
        } else if (in[i] == '+') {
            out[o++] = ' ';
        } else {
            out[o++] = in[i];
        }
    }
    out[o] = 0;
    return true;
}

static esp_err_t root_handler(httpd_req_t *req)
{
    common_headers(req, "text/html");
    static const char html[] =
        "<!doctype html><html><head><meta name=viewport content='width=device-width,initial-scale=1'>"
        "<title>XAU/USD ESP32-C6</title><style>"
        ":root{color-scheme:dark}*{box-sizing:border-box}body{margin:0;background:#0b0e14;color:#d6deeb;font-family:-apple-system,BlinkMacSystemFont,Segoe UI,sans-serif}"
        "main{max-width:760px;margin:auto;padding:18px}.top{display:flex;justify-content:space-between;gap:12px;align-items:flex-start}"
        "h1{font-size:20px;margin:0;color:#5ccfe6}.sub{color:#7a88a0;font-size:13px;margin-top:3px}.price{font-size:42px;font-weight:700;margin:20px 0 2px}"
        ".change{font-size:18px}.ok{color:#7ee787}.bad{color:#ff8a8a}.muted{color:#7a88a0}.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(170px,1fr));gap:10px;margin-top:16px}"
        ".card{background:#131722;border:1px solid #222838;border-radius:8px;padding:12px}.label{font-size:12px;color:#7a88a0}.value{font-size:17px;margin-top:4px}"
        ".controls{display:flex;flex-wrap:wrap;gap:8px;margin-top:14px}button{padding:9px 12px;background:#1b2230;color:#d6deeb;border:1px solid #334155;border-radius:6px}"
        "button.active{border-color:#5ccfe6;color:#5ccfe6}input[type=range]{width:100%;margin-top:12px}pre{white-space:pre-wrap;overflow:auto;font-size:12px}"
        "</style></head><body><main><div class=top><div><h1>XAU / USD</h1><div class=sub>ESP32-C6 live chart control</div></div><div id=live class=muted>LIVE</div></div>"
        "<div id=price class=price>--</div><div id=change class=change>--</div>"
        "<div class=grid>"
        "<div class=card><div class=label>Wi-Fi</div><div id=wifi class=value>--</div></div>"
        "<div class=card><div class=label>Display</div><div id=display class=value>--</div></div>"
        "<div class=card><div class=label>Sources</div><div id=sources class=value>--</div></div>"
        "<div class=card><div class=label>Memory</div><div id=heap class=value>--</div></div>"
        "<div class=card><div class=label>OTA</div><div id=ota class=value>--</div></div>"
        "</div><div class=card style='margin-top:10px'><div class=label>Brightness</div><input id=b type=range min=0 max=100 value=30 oninput='bv.textContent=this.value+\"%\"'><div id=bv class=value>30%</div></div>"
        "<div class=controls id=modes>"
        "<button data-mode=auto>Auto</button><button data-mode=day>Day</button><button data-mode=dim>Dim</button><button data-mode=off>Off</button>"
        "<button id=setb>Set Brightness</button><button id=wake>Wake 60s</button><button id=refresh>Refresh</button>"
        "</div><div class=card style='margin-top:10px'><div class=label>OTA URL</div><input id=otau style='width:100%;margin-top:8px;padding:9px;background:#0b0e14;color:#d6deeb;border:1px solid #334155;border-radius:6px' placeholder='https://enderekici.github.io/esp32c6-xauusd-chart/xauusd_chart.bin'><div class=controls><button id=otab>Update Firmware</button></div></div>"
        "<div class=card style='margin-top:10px'><div class=label>Raw status</div><pre id=raw>loading...</pre></div>"
        "</main><script>"
        "const $=id=>document.getElementById(id);let last={};"
        "async function post(u){await fetch(u,{method:'POST'});await load()}"
        "function age(v){return v?`${v}s`:'--'}"
        "async function load(){let r=await fetch('/api/status');let s=await r.json();last=s;"
        "$('price').textContent=s.market.spot_valid?'$'+s.market.spot_price.toFixed(2):(s.market.live_valid?'$'+s.market.live_price.toFixed(2):'--');"
        "let pos=s.market.live_change_pct>=0;$('change').textContent=(s.market.live_valid?`${pos?'+':''}${s.market.live_change_pct.toFixed(2)}% 24h`:'--');$('change').className='change '+(pos?'ok':'bad');"
        "$('live').textContent=s.market.ws_connected?'● LIVE':'● OFF';$('live').className=s.market.ws_connected?'ok':'muted';"
        "$('wifi').textContent=s.wifi.connected?`${s.wifi.ip}  ${s.wifi.rssi}dBm`:'down';"
        "$('display').textContent=`${s.display.mode}  ${s.display.effective_percent}%${s.display.wake_remaining_s?' wake '+s.display.wake_remaining_s+'s':''}`;"
        "$('sources').textContent=`WS ${age(s.market.live_age_s)}  Spot ${age(s.market.spot_age_s)}  Hist ${age(s.history.age_s)}`;"
        "$('heap').textContent=`${Math.round(s.heap.free/1024)}K free / ${Math.round(s.heap.largest_block/1024)}K block`;"
        "$('ota').textContent=`${s.ota.state} ${s.ota.message||''}`;"
        "$('b').value=s.display.configured_percent;$('bv').textContent=s.display.configured_percent+'%';"
        "document.querySelectorAll('[data-mode]').forEach(x=>x.classList.toggle('active',x.dataset.mode==s.display.mode));$('raw').textContent=JSON.stringify(s,null,2)}"
        "document.querySelectorAll('[data-mode]').forEach(x=>x.onclick=()=>post('/api/display?mode='+x.dataset.mode));"
        "$('setb').onclick=()=>post('/api/brightness?percent='+$('b').value);$('wake').onclick=()=>post('/api/wake?seconds=60');$('refresh').onclick=()=>post('/api/refresh');"
        "$('otab').onclick=()=>{let u=$('otau').value.trim();if(u&&confirm('Install firmware from '+u+' ?'))post('/api/ota?url='+encodeURIComponent(u))};"
        "load();setInterval(load,3000)"
        "</script></body></html>";
    return httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t status_handler(httpd_req_t *req)
{
    ui_chart_status_t ui;
    market_status_t market;
    ota_update_status_t ota;
    ui_chart_status_get(&ui);
    market_status_get(&market);
    ota_update_status_get(&ota);

    char *json = malloc(2048);
    if (!json) {
        common_headers(req, "application/json");
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"oom\"}");
    }

    int n = snprintf(json, 2048,
        "{"
        "\"uptime_s\":%lu,"
        "\"wifi\":{\"connected\":%s,\"ip\":\"%s\",\"rssi\":%d},"
        "\"heap\":{\"free\":%lu,\"min_free\":%lu,\"largest_block\":%lu},"
        "\"display\":{\"mode\":\"%s\",\"configured_percent\":%u,\"effective_percent\":%u,\"wake_remaining_s\":%lu},"
        "\"history\":{\"valid\":%s,\"age_s\":%lu,\"points\":%d,\"success_count\":%lu,\"fail_count\":%lu},"
        "\"market\":{\"ws_connected\":%s,\"live_valid\":%s,\"spot_valid\":%s,"
        "\"live_price\":%.2f,\"live_change_pct\":%.2f,\"spot_price\":%.2f,"
        "\"live_age_s\":%lu,\"spot_age_s\":%lu,"
        "\"ws_connect_count\":%lu,\"ws_disconnect_count\":%lu,\"spot_fail_count\":%lu},"
        "\"ota\":{\"state\":\"%s\",\"message\":\"%s\",\"url\":\"%s\",\"started_s\":%lu,\"finished_s\":%lu,\"rebooting\":%s}"
        "}",
        (unsigned long)ui.uptime_s,
        wifi_is_connected() ? "true" : "false", wifi_ip(), wifi_rssi(),
        (unsigned long)esp_get_free_heap_size(),
        (unsigned long)esp_get_minimum_free_heap_size(),
        (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
        ui_chart_display_mode_name(ui.mode),
        (unsigned)ui.configured_percent,
        (unsigned)ui.effective_percent,
        (unsigned long)ui.wake_remaining_s,
        ui.history_valid ? "true" : "false",
        (unsigned long)ui.last_history_age_s,
        ui.chart_points,
        (unsigned long)ui.history_success_count,
        (unsigned long)ui.history_fail_count,
        market.live_connected ? "true" : "false",
        market.live_valid ? "true" : "false",
        market.spot_valid ? "true" : "false",
        market.live_price,
        market.live_change_pct,
        market.spot_price,
        (unsigned long)market.live_age_s,
        (unsigned long)market.spot_age_s,
        (unsigned long)market.ws_connect_count,
        (unsigned long)market.ws_disconnect_count,
        (unsigned long)market.spot_fail_count,
        ota_update_state_name(ota.state),
        ota.message,
        ota.url,
        (unsigned long)ota.started_s,
        (unsigned long)ota.finished_s,
        ota.rebooting ? "true" : "false");

    common_headers(req, "application/json");
    esp_err_t err = httpd_resp_send(req, json, n);
    free(json);
    return err;
}

static esp_err_t display_handler(httpd_req_t *req)
{
    char mode_s[16];
    ui_chart_display_mode_t mode;
    common_headers(req, "application/json");
    if (!query_value(req, "mode", mode_s, sizeof(mode_s)) ||
        !ui_chart_parse_display_mode(mode_s, &mode) ||
        !ui_chart_set_display_mode(mode)) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"bad mode\"}");
    }
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t brightness_handler(httpd_req_t *req)
{
    char value[8];
    char *end = NULL;
    common_headers(req, "application/json");
    if (!query_value(req, "percent", value, sizeof(value))) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"missing percent\"}");
    }
    long percent = strtol(value, &end, 10);
    if (!end || *end != 0 || percent < 0 || percent > 100 ||
        !ui_chart_set_custom_brightness((uint8_t)percent)) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"bad percent\"}");
    }
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t wake_handler(httpd_req_t *req)
{
    char value[12];
    uint32_t seconds = 60;
    common_headers(req, "application/json");
    if (query_value(req, "seconds", value, sizeof(value))) {
        char *end = NULL;
        long parsed = strtol(value, &end, 10);
        if (end && *end == 0 && parsed > 0) {
            seconds = (uint32_t)parsed;
        }
    }
    ui_chart_wake_for(seconds);
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t refresh_handler(httpd_req_t *req)
{
    common_headers(req, "application/json");
    ui_chart_force_refresh();
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t ota_handler(httpd_req_t *req)
{
    char url[192];
    common_headers(req, "application/json");
    if (!query_value(req, "url", url, sizeof(url))) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"missing url\"}");
    }

    char decoded[192];
    if (!url_decode(url, decoded, sizeof(decoded))) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"bad url encoding\"}");
    }

    if (!ota_update_start(decoded)) {
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"ota not started\"}");
    }
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

void http_control_start(void)
{
    if (s_server) return;

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 80;
    cfg.lru_purge_enable = true;
    cfg.stack_size = 8192;

    esp_err_t err = httpd_start(&s_server, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "start failed: %s", esp_err_to_name(err));
        return;
    }

    httpd_uri_t root = { .uri = "/", .method = HTTP_GET, .handler = root_handler };
    httpd_uri_t status = { .uri = "/api/status", .method = HTTP_GET, .handler = status_handler };
    httpd_uri_t display = { .uri = "/api/display", .method = HTTP_POST, .handler = display_handler };
    httpd_uri_t brightness = { .uri = "/api/brightness", .method = HTTP_POST, .handler = brightness_handler };
    httpd_uri_t wake = { .uri = "/api/wake", .method = HTTP_POST, .handler = wake_handler };
    httpd_uri_t refresh = { .uri = "/api/refresh", .method = HTTP_POST, .handler = refresh_handler };
    httpd_uri_t ota = { .uri = "/api/ota", .method = HTTP_POST, .handler = ota_handler };

    ESP_ERROR_CHECK(httpd_register_uri_handler(s_server, &root));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_server, &status));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_server, &display));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_server, &brightness));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_server, &wake));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_server, &refresh));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_server, &ota));
    ESP_LOGI(TAG, "control server started on port 80");
}
