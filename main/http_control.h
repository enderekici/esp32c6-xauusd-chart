#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Starts the local status/control HTTP server on port 80. Call after Wi-Fi,
// time, and UI startup; handlers tolerate Wi-Fi not having an IP yet.
void http_control_start(void);

#ifdef __cplusplus
}
#endif
