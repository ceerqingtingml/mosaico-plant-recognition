// SPDX-License-Identifier: Apache-2.0

#include "plant_wifi.h"

#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "plant_wifi";

typedef struct {
    SemaphoreHandle_t lock;
    esp_netif_t *netif;
    esp_event_handler_instance_t wifi_events;
    esp_event_handler_instance_t ip_events;
    bool started;
    bool scanning;
    plant_wifi_ap_t aps[PLANT_WIFI_MAX_APS];
    size_t ap_count;
    plant_wifi_conn_state_t conn_state;
    esp_err_t last_error;
    char pending_ssid[PLANT_WIFI_SSID_MAX + 1];
} plant_wifi_t;

static plant_wifi_t s_wifi;

static void set_error_locked(esp_err_t err)
{
    s_wifi.last_error = err;
}

static void ingest_scan_results(void)
{
    uint16_t available = 0;
    if (esp_wifi_scan_get_ap_num(&available) != ESP_OK) {
        xSemaphoreTake(s_wifi.lock, portMAX_DELAY);
        s_wifi.scanning = false;
        set_error_locked(ESP_FAIL);
        xSemaphoreGive(s_wifi.lock);
        return;
    }

    uint16_t count = available;
    if (count > PLANT_WIFI_MAX_APS) {
        count = PLANT_WIFI_MAX_APS;
    }
    wifi_ap_record_t *records = NULL;
    esp_err_t err = ESP_OK;
    if (count > 0) {
        records = calloc(count, sizeof(*records));
        if (!records) {
            xSemaphoreTake(s_wifi.lock, portMAX_DELAY);
            s_wifi.scanning = false;
            set_error_locked(ESP_ERR_NO_MEM);
            xSemaphoreGive(s_wifi.lock);
            return;
        }
        err = esp_wifi_scan_get_ap_records(&count, records);
    }

    xSemaphoreTake(s_wifi.lock, portMAX_DELAY);
    s_wifi.scanning = false;
    s_wifi.ap_count = 0;
    if (err != ESP_OK) {
        set_error_locked(err);
        xSemaphoreGive(s_wifi.lock);
        free(records);
        return;
    }

    for (uint16_t i = 0; i < count && s_wifi.ap_count < PLANT_WIFI_MAX_APS; ++i) {
        if (records[i].ssid[0] == '\0') {
            continue;
        }
        bool duplicate = false;
        for (size_t j = 0; j < s_wifi.ap_count; ++j) {
            if (strncmp(s_wifi.aps[j].ssid, (const char *)records[i].ssid,
                        PLANT_WIFI_SSID_MAX) == 0) {
                if (records[i].rssi > s_wifi.aps[j].rssi) {
                    s_wifi.aps[j].rssi = records[i].rssi;
                    s_wifi.aps[j].secure =
                        records[i].authmode != WIFI_AUTH_OPEN;
                }
                duplicate = true;
                break;
            }
        }
        if (duplicate) {
            continue;
        }
        plant_wifi_ap_t *ap = &s_wifi.aps[s_wifi.ap_count++];
        strlcpy(ap->ssid, (const char *)records[i].ssid, sizeof(ap->ssid));
        ap->rssi = records[i].rssi;
        ap->secure = records[i].authmode != WIFI_AUTH_OPEN;
    }

    /* Strongest first. */
    for (size_t i = 1; i < s_wifi.ap_count; ++i) {
        plant_wifi_ap_t key = s_wifi.aps[i];
        size_t j = i;
        while (j > 0 && s_wifi.aps[j - 1].rssi < key.rssi) {
            s_wifi.aps[j] = s_wifi.aps[j - 1];
            --j;
        }
        s_wifi.aps[j] = key;
    }
    set_error_locked(ESP_OK);
    const size_t unique = s_wifi.ap_count;
    xSemaphoreGive(s_wifi.lock);
    free(records);
    ESP_LOGI(TAG, "scan done: %u unique APs", (unsigned)unique);
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)data;
    if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) {
        ingest_scan_results();
        return;
    }
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        bool reconnect = false;
        xSemaphoreTake(s_wifi.lock, portMAX_DELAY);
        if (s_wifi.conn_state == PLANT_WIFI_CONN_CONNECTED) {
            s_wifi.conn_state = PLANT_WIFI_CONN_CONNECTING;
            reconnect = true;
        } else if (s_wifi.conn_state == PLANT_WIFI_CONN_CONNECTING) {
            s_wifi.conn_state = PLANT_WIFI_CONN_FAILED;
            set_error_locked(ESP_FAIL);
        }
        xSemaphoreGive(s_wifi.lock);
        if (reconnect) {
            (void)esp_wifi_connect();
        }
        return;
    }
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xSemaphoreTake(s_wifi.lock, portMAX_DELAY);
        s_wifi.conn_state = PLANT_WIFI_CONN_CONNECTED;
        set_error_locked(ESP_OK);
        xSemaphoreGive(s_wifi.lock);
        ESP_LOGI(TAG, "connected to %s", s_wifi.pending_ssid);
    }
}

esp_err_t plant_wifi_ensure_started(void)
{
    if (!s_wifi.lock) {
        s_wifi.lock = xSemaphoreCreateMutex();
        ESP_RETURN_ON_FALSE(s_wifi.lock, ESP_ERR_NO_MEM, TAG, "mutex");
    }

    xSemaphoreTake(s_wifi.lock, portMAX_DELAY);
    const bool started = s_wifi.started;
    xSemaphoreGive(s_wifi.lock);
    if (started) {
        return ESP_OK;
    }

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    if (!s_wifi.netif) {
        s_wifi.netif = esp_netif_create_default_wifi_sta();
        ESP_RETURN_ON_FALSE(s_wifi.netif, ESP_ERR_NO_MEM, TAG, "sta netif");
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK && err != ESP_ERR_WIFI_INIT_STATE) {
        return err;
    }

    if (!s_wifi.wifi_events) {
        ESP_RETURN_ON_ERROR(
            esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                on_wifi_event, NULL,
                                                &s_wifi.wifi_events),
            TAG, "wifi events");
    }
    if (!s_wifi.ip_events) {
        ESP_RETURN_ON_ERROR(
            esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                on_wifi_event, NULL,
                                                &s_wifi.ip_events),
            TAG, "ip events");
    }

    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_FLASH), TAG, "storage");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "mode");
    err = esp_wifi_start();
    if (err != ESP_OK) {
        /* esp_wifi_start is idempotent once the driver is running. */
        wifi_mode_t mode = WIFI_MODE_NULL;
        if (esp_wifi_get_mode(&mode) != ESP_OK || mode == WIFI_MODE_NULL) {
            ESP_RETURN_ON_ERROR(err, TAG, "wifi start");
        }
    }

    xSemaphoreTake(s_wifi.lock, portMAX_DELAY);
    s_wifi.started = true;
    s_wifi.conn_state = PLANT_WIFI_CONN_IDLE;
    set_error_locked(ESP_OK);
    xSemaphoreGive(s_wifi.lock);

    wifi_config_t saved = {0};
    if (esp_wifi_get_config(WIFI_IF_STA, &saved) == ESP_OK &&
        saved.sta.ssid[0] != '\0') {
        xSemaphoreTake(s_wifi.lock, portMAX_DELAY);
        strlcpy(s_wifi.pending_ssid, (const char *)saved.sta.ssid,
                sizeof(s_wifi.pending_ssid));
        s_wifi.conn_state = PLANT_WIFI_CONN_CONNECTING;
        xSemaphoreGive(s_wifi.lock);
        if (esp_wifi_connect() != ESP_OK) {
            xSemaphoreTake(s_wifi.lock, portMAX_DELAY);
            s_wifi.conn_state = PLANT_WIFI_CONN_FAILED;
            xSemaphoreGive(s_wifi.lock);
        }
    }
    ESP_LOGI(TAG, "Wi-Fi station ready");
    return ESP_OK;
}

esp_err_t plant_wifi_request_scan(void)
{
    ESP_RETURN_ON_ERROR(plant_wifi_ensure_started(), TAG, "start");

    xSemaphoreTake(s_wifi.lock, portMAX_DELAY);
    if (s_wifi.scanning) {
        xSemaphoreGive(s_wifi.lock);
        return ESP_OK;
    }
    s_wifi.scanning = true;
    s_wifi.ap_count = 0;
    set_error_locked(ESP_OK);
    xSemaphoreGive(s_wifi.lock);

    wifi_scan_config_t scan = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active.min = 100,
        .scan_time.active.max = 300,
    };
    const esp_err_t err = esp_wifi_scan_start(&scan, false);
    if (err != ESP_OK) {
        xSemaphoreTake(s_wifi.lock, portMAX_DELAY);
        s_wifi.scanning = false;
        set_error_locked(err);
        xSemaphoreGive(s_wifi.lock);
        ESP_LOGE(TAG, "scan_start failed: %s", esp_err_to_name(err));
    }
    return err;
}

void plant_wifi_abort_scan(void)
{
    if (!s_wifi.lock) {
        return;
    }
    (void)esp_wifi_scan_stop();
    xSemaphoreTake(s_wifi.lock, portMAX_DELAY);
    s_wifi.scanning = false;
    xSemaphoreGive(s_wifi.lock);
}

bool plant_wifi_is_scanning(void)
{
    if (!s_wifi.lock) {
        return false;
    }
    xSemaphoreTake(s_wifi.lock, portMAX_DELAY);
    const bool scanning = s_wifi.scanning;
    xSemaphoreGive(s_wifi.lock);
    return scanning;
}

size_t plant_wifi_copy_results(plant_wifi_ap_t *out, size_t capacity)
{
    if (!out || capacity == 0 || !s_wifi.lock) {
        return 0;
    }
    xSemaphoreTake(s_wifi.lock, portMAX_DELAY);
    const size_t n = s_wifi.ap_count < capacity ? s_wifi.ap_count : capacity;
    memcpy(out, s_wifi.aps, n * sizeof(plant_wifi_ap_t));
    xSemaphoreGive(s_wifi.lock);
    return n;
}

esp_err_t plant_wifi_connect(const char *ssid, const char *password)
{
    ESP_RETURN_ON_FALSE(ssid && ssid[0], ESP_ERR_INVALID_ARG, TAG, "ssid");
    ESP_RETURN_ON_ERROR(plant_wifi_ensure_started(), TAG, "start");

    wifi_config_t cfg = {0};
    strlcpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid));
    if (password) {
        strlcpy((char *)cfg.sta.password, password, sizeof(cfg.sta.password));
    }
    cfg.sta.threshold.authmode =
        (password && password[0]) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    xSemaphoreTake(s_wifi.lock, portMAX_DELAY);
    strlcpy(s_wifi.pending_ssid, ssid, sizeof(s_wifi.pending_ssid));
    s_wifi.conn_state = PLANT_WIFI_CONN_CONNECTING;
    set_error_locked(ESP_OK);
    xSemaphoreGive(s_wifi.lock);

    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &cfg), TAG, "config");
    (void)esp_wifi_disconnect();
    const esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        xSemaphoreTake(s_wifi.lock, portMAX_DELAY);
        s_wifi.conn_state = PLANT_WIFI_CONN_FAILED;
        set_error_locked(err);
        xSemaphoreGive(s_wifi.lock);
    }
    return err;
}

plant_wifi_conn_state_t plant_wifi_conn_state(void)
{
    if (!s_wifi.lock) {
        return PLANT_WIFI_CONN_IDLE;
    }
    xSemaphoreTake(s_wifi.lock, portMAX_DELAY);
    const plant_wifi_conn_state_t state = s_wifi.conn_state;
    xSemaphoreGive(s_wifi.lock);
    return state;
}

esp_err_t plant_wifi_last_error(void)
{
    if (!s_wifi.lock) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_wifi.lock, portMAX_DELAY);
    const esp_err_t err = s_wifi.last_error;
    xSemaphoreGive(s_wifi.lock);
    return err;
}

esp_err_t plant_wifi_current_ssid(char *out, size_t capacity)
{
    ESP_RETURN_ON_FALSE(out && capacity > 0, ESP_ERR_INVALID_ARG, TAG, "args");
    out[0] = '\0';
    ESP_RETURN_ON_FALSE(s_wifi.lock, ESP_ERR_INVALID_STATE, TAG, "not started");
    xSemaphoreTake(s_wifi.lock, portMAX_DELAY);
    if (s_wifi.conn_state == PLANT_WIFI_CONN_CONNECTED) {
        strlcpy(out, s_wifi.pending_ssid, capacity);
    }
    xSemaphoreGive(s_wifi.lock);
    return out[0] ? ESP_OK : ESP_ERR_NOT_FOUND;
}
