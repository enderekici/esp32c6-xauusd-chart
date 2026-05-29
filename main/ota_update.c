#include "ota_update.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_crt_bundle.h"
#include "esp_err.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "esp_wifi.h"

static const char *TAG = "ota";

static SemaphoreHandle_t s_mtx;
static ota_update_status_t s_status = {
    .state = OTA_UPDATE_IDLE,
    .message = "idle",
};

const char *ota_update_state_name(ota_update_state_t state)
{
    switch (state) {
    case OTA_UPDATE_IDLE:         return "idle";
    case OTA_UPDATE_RUNNING:      return "running";
    case OTA_UPDATE_OK_REBOOTING: return "ok_rebooting";
    case OTA_UPDATE_FAILED:       return "failed";
    default:                      return "unknown";
    }
}

static uint32_t uptime_s(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000000);
}

static void status_set(ota_update_state_t state, const char *msg)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    s_status.state = state;
    snprintf(s_status.message, sizeof(s_status.message), "%s", msg ? msg : "");
    if (state == OTA_UPDATE_FAILED || state == OTA_UPDATE_OK_REBOOTING) {
        s_status.finished_s = uptime_s();
    }
    s_status.rebooting = (state == OTA_UPDATE_OK_REBOOTING);
    xSemaphoreGive(s_mtx);
}

static void ota_task(void *arg)
{
    char url[192];
    snprintf(url, sizeof(url), "%s", (const char *)arg);
    free(arg);

    ESP_LOGI(TAG, "starting update from %s", url);
    esp_wifi_set_ps(WIFI_PS_NONE);

    esp_http_client_config_t http_cfg = {
        .url = url,
        .timeout_ms = 15000,
        .keep_alive_enable = true,
        .crt_bundle_attach = esp_crt_bundle_attach,
        // LAN HTTP is intentionally allowed for the first OTA implementation.
        .skip_cert_common_name_check = true,
    };
    esp_https_ota_config_t ota_cfg = {
        .http_config = &http_cfg,
    };

    esp_err_t err = esp_https_ota(&ota_cfg);
    if (err == ESP_OK) {
        status_set(OTA_UPDATE_OK_REBOOTING, "update ok; rebooting");
        ESP_LOGI(TAG, "update ok; rebooting");
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
    }

    ESP_LOGE(TAG, "update failed: %s", esp_err_to_name(err));
    char msg[96];
    snprintf(msg, sizeof(msg), "failed: %s", esp_err_to_name(err));
    status_set(OTA_UPDATE_FAILED, msg);
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    vTaskDelete(NULL);
}

bool ota_update_start(const char *url)
{
    if (!s_mtx) {
        s_mtx = xSemaphoreCreateMutex();
    }
    if (!url || (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0)) {
        status_set(OTA_UPDATE_FAILED, "only http:// or https:// URLs are enabled");
        return false;
    }
    if (strlen(url) >= sizeof(s_status.url)) {
        status_set(OTA_UPDATE_FAILED, "url too long");
        return false;
    }

    xSemaphoreTake(s_mtx, portMAX_DELAY);
    bool busy = s_status.state == OTA_UPDATE_RUNNING || s_status.state == OTA_UPDATE_OK_REBOOTING;
    if (!busy) {
        memset(&s_status, 0, sizeof(s_status));
        s_status.state = OTA_UPDATE_RUNNING;
        s_status.started_s = uptime_s();
        snprintf(s_status.url, sizeof(s_status.url), "%s", url);
        snprintf(s_status.message, sizeof(s_status.message), "downloading");
    }
    xSemaphoreGive(s_mtx);
    if (busy) return false;

    char *task_url = strdup(url);
    if (!task_url) {
        status_set(OTA_UPDATE_FAILED, "url alloc failed");
        return false;
    }
    if (xTaskCreate(ota_task, "ota_update", 8192, task_url, 5, NULL) != pdPASS) {
        free(task_url);
        status_set(OTA_UPDATE_FAILED, "task create failed");
        return false;
    }
    return true;
}

void ota_update_status_get(ota_update_status_t *out)
{
    if (!s_mtx) {
        s_mtx = xSemaphoreCreateMutex();
    }
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    *out = s_status;
    xSemaphoreGive(s_mtx);
}
