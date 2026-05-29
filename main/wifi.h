#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Bring up netif + esp_event + esp_wifi as a plain STA and start connecting.
// Auto-reconnects on disconnect. Call once at boot after nvs_flash_init().
void wifi_start(void);

// True once an IP has been acquired.
bool wifi_is_connected(void);

// Last measured AP RSSI in dBm (0 if unknown / disconnected).
int8_t wifi_rssi(void);

// Dotted-quad IP string ("" until connected). Stable pointer.
const char *wifi_ip(void);

#ifdef __cplusplus
}
#endif
