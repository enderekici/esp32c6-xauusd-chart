#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    OTA_UPDATE_IDLE = 0,
    OTA_UPDATE_RUNNING,
    OTA_UPDATE_OK_REBOOTING,
    OTA_UPDATE_FAILED,
} ota_update_state_t;

typedef struct {
    ota_update_state_t state;
    char url[192];
    char message[96];
    uint32_t started_s;
    uint32_t finished_s;
    bool rebooting;
} ota_update_status_t;

bool ota_update_start(const char *url);
void ota_update_status_get(ota_update_status_t *out);
const char *ota_update_state_name(ota_update_state_t state);

#ifdef __cplusplus
}
#endif
