#include "wifi.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"

#include "secrets.h"

static const char *TAG = "wifi";

static SemaphoreHandle_t s_mtx;
static bool   s_connected;
static int8_t s_rssi;
static char   s_ip[16];

bool wifi_is_connected(void)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    bool c = s_connected;
    xSemaphoreGive(s_mtx);
    return c;
}

int8_t wifi_rssi(void)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    int8_t r = s_rssi;
    xSemaphoreGive(s_mtx);
    return r;
}

const char *wifi_ip(void)
{
    return s_ip;  // written under lock, read is a snapshot of a C-string
}

static void event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *d = data;
        ESP_LOGW(TAG, "disconnected (reason %d), reconnecting", d ? d->reason : -1);
        xSemaphoreTake(s_mtx, portMAX_DELAY);
        s_connected = false;
        s_rssi = 0;
        s_ip[0] = 0;
        xSemaphoreGive(s_mtx);
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *e = data;
        xSemaphoreTake(s_mtx, portMAX_DELAY);
        s_connected = true;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&e->ip_info.ip));
        xSemaphoreGive(s_mtx);
        ESP_LOGI(TAG, "got IP %s", s_ip);
    }
}

static void rssi_task(void *arg)
{
    while (true) {
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            xSemaphoreTake(s_mtx, portMAX_DELAY);
            s_rssi = ap.rssi;
            xSemaphoreGive(s_mtx);
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

void wifi_start(void)
{
    s_mtx = xSemaphoreCreateMutex();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, event_handler, NULL, NULL));

    wifi_config_t wc = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = { .capable = true, .required = false },
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_MIN_MODEM));

    xTaskCreate(rssi_task, "wifi_rssi", 3072, NULL, 3, NULL);
    ESP_LOGI(TAG, "STA starting, connecting to '%s' (min modem-sleep)", WIFI_SSID);
}
