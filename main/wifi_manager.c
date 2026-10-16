/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "wifi_manager.h"

#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAILED_BIT    BIT1
#define WIFI_DISCONNECTED_BIT BIT2

static const char *TAG = "plant_wifi";
static EventGroupHandle_t s_events;
static wifi_manager_state_cb_t s_state_callback;
static void *s_state_user_data;
static bool s_initialized;
static bool s_connected;
static bool s_connecting;
static bool s_reconnect_enabled;
static char s_current_ssid[WIFI_MANAGER_SSID_MAX_LEN + 1];
static esp_err_t s_last_error = ESP_OK;

static void notify_state(bool connected, esp_err_t reason)
{
    if (s_state_callback) {
        s_state_callback(connected, s_current_ssid, reason, s_state_user_data);
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_data;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        const bool was_connected = s_connected;
        s_connected = false;
        xEventGroupSetBits(s_events, WIFI_DISCONNECTED_BIT);
        if (!s_connecting) {
            if (was_connected && s_reconnect_enabled) {
                notify_state(false, ESP_ERR_WIFI_NOT_CONNECT);
            }
            if (s_reconnect_enabled) {
                const esp_err_t reconnect_ret = esp_wifi_connect();
                if (reconnect_ret != ESP_OK) {
                    ESP_LOGW(TAG, "Schedule Wi-Fi reconnect failed: %s",
                             esp_err_to_name(reconnect_ret));
                }
            }
            return;
        }
        s_last_error = ESP_ERR_WIFI_NOT_CONNECT;
        xEventGroupSetBits(s_events, WIFI_FAILED_BIT);
        notify_state(false, s_last_error);
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        s_connected = true;
        s_connecting = false;
        s_reconnect_enabled = true;
        s_last_error = ESP_OK;
        xEventGroupSetBits(s_events, WIFI_CONNECTED_BIT);
        notify_state(true, ESP_OK);
    }
}

esp_err_t wifi_manager_init(wifi_manager_state_cb_t callback, void *user_data)
{
    if (s_initialized) {
        s_state_callback = callback;
        s_state_user_data = user_data;
        return ESP_OK;
    }

    s_events = xEventGroupCreate();
    ESP_RETURN_ON_FALSE(s_events, ESP_ERR_NO_MEM, TAG,
                        "create Wi-Fi event group failed");

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "initialize esp-netif failed");
    esp_err_t ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_RETURN_ON_ERROR(ret, TAG, "create default event loop failed");
    }
    ESP_RETURN_ON_FALSE(esp_netif_create_default_wifi_sta(), ESP_FAIL, TAG,
                        "create station interface failed");

    wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&config), TAG, "initialize Wi-Fi failed");
    ESP_RETURN_ON_ERROR(
        esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                   wifi_event_handler, NULL),
        TAG, "register Wi-Fi event handler failed");
    ESP_RETURN_ON_ERROR(
        esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                   wifi_event_handler, NULL),
        TAG, "register IP event handler failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_FLASH), TAG,
                        "set Wi-Fi storage failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG,
                        "set station mode failed");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "start Wi-Fi failed");

    s_state_callback = callback;
    s_state_user_data = user_data;
    s_initialized = true;
    return ESP_OK;
}

static esp_err_t wait_for_connection(uint32_t timeout_ms)
{
    const EventBits_t bits = xEventGroupWaitBits(
        s_events, WIFI_CONNECTED_BIT | WIFI_FAILED_BIT, pdTRUE, pdFALSE,
        pdMS_TO_TICKS(timeout_ms));

    if (bits & WIFI_CONNECTED_BIT) {
        s_connecting = false;
        return ESP_OK;
    }
    if (bits & WIFI_FAILED_BIT) {
        s_connecting = false;
        return s_last_error;
    }
    s_connecting = false;
    (void)esp_wifi_disconnect();
    return ESP_ERR_TIMEOUT;
}

esp_err_t wifi_manager_try_saved(uint32_t timeout_ms)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG,
                        "Wi-Fi manager is not initialized");

    wifi_config_t config = {0};
    ESP_RETURN_ON_ERROR(esp_wifi_get_config(WIFI_IF_STA, &config), TAG,
                        "read saved Wi-Fi configuration failed");
    if (config.sta.ssid[0] == '\0') {
        return ESP_ERR_NOT_FOUND;
    }

    strlcpy(s_current_ssid, (const char *)config.sta.ssid,
            sizeof(s_current_ssid));
    ESP_LOGI(TAG, "Connecting to saved network \"%s\"", s_current_ssid);
    xEventGroupClearBits(s_events, WIFI_CONNECTED_BIT | WIFI_FAILED_BIT);
    s_reconnect_enabled = true;
    s_connecting = true;
    ESP_RETURN_ON_ERROR(esp_wifi_connect(), TAG,
                        "start saved Wi-Fi connection failed");
    return wait_for_connection(timeout_ms);
}

esp_err_t wifi_manager_scan(wifi_manager_ap_t *aps, size_t capacity,
                            size_t *out_count)
{
    ESP_RETURN_ON_FALSE(s_initialized && aps && out_count && capacity > 0,
                        ESP_ERR_INVALID_ARG, TAG, "invalid scan arguments");

    wifi_scan_config_t scan_config = {
        .show_hidden = true,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
    };
    ESP_RETURN_ON_ERROR(esp_wifi_scan_start(&scan_config, true), TAG,
                        "Wi-Fi scan failed");

    uint16_t count = capacity > UINT16_MAX ? UINT16_MAX : (uint16_t)capacity;
    wifi_ap_record_t records[WIFI_MANAGER_MAX_APS] = {0};
    if (count > WIFI_MANAGER_MAX_APS) {
        count = WIFI_MANAGER_MAX_APS;
    }
    ESP_RETURN_ON_ERROR(esp_wifi_scan_get_ap_records(&count, records), TAG,
                        "read Wi-Fi scan results failed");

    size_t written = 0;
    for (uint16_t i = 0; i < count && written < capacity; ++i) {
        if (records[i].ssid[0] == '\0') {
            continue;
        }
        strlcpy(aps[written].ssid, (const char *)records[i].ssid,
                sizeof(aps[written].ssid));
        aps[written].rssi = records[i].rssi;
        aps[written].secure = records[i].authmode != WIFI_AUTH_OPEN;
        ++written;
    }
    *out_count = written;
    return ESP_OK;
}

esp_err_t wifi_manager_connect(const char *ssid, const char *password,
                               uint32_t timeout_ms)
{
    ESP_RETURN_ON_FALSE(s_initialized && ssid && ssid[0] != '\0',
                        ESP_ERR_INVALID_ARG, TAG,
                        "invalid Wi-Fi connection arguments");

    wifi_config_t config = {0};
    strlcpy((char *)config.sta.ssid, ssid, sizeof(config.sta.ssid));
    strlcpy((char *)config.sta.password, password ? password : "",
            sizeof(config.sta.password));
    config.sta.threshold.authmode =
        password && password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    config.sta.pmf_cfg.capable = true;
    config.sta.pmf_cfg.required = false;

    s_reconnect_enabled = false;
    s_connecting = false;
    xEventGroupClearBits(s_events, WIFI_DISCONNECTED_BIT);
    (void)esp_wifi_disconnect();
    (void)xEventGroupWaitBits(s_events, WIFI_DISCONNECTED_BIT, pdTRUE, pdFALSE,
                              pdMS_TO_TICKS(1000));
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &config), TAG,
                        "save Wi-Fi configuration failed");
    strlcpy(s_current_ssid, ssid, sizeof(s_current_ssid));
    s_connected = false;
    s_last_error = ESP_OK;
    xEventGroupClearBits(s_events, WIFI_CONNECTED_BIT | WIFI_FAILED_BIT);
    s_reconnect_enabled = true;
    s_connecting = true;
    ESP_RETURN_ON_ERROR(esp_wifi_connect(), TAG,
                        "start Wi-Fi connection failed");

    const esp_err_t ret = wait_for_connection(timeout_ms);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Connection to \"%s\" failed: %s", ssid,
                 esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t wifi_manager_forget(void)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG,
                        "Wi-Fi manager is not initialized");
    s_reconnect_enabled = false;
    s_connecting = false;
    (void)esp_wifi_disconnect();
    wifi_config_t empty = {0};
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &empty), TAG,
                        "clear Wi-Fi configuration failed");
    s_connected = false;
    s_current_ssid[0] = '\0';
    return ESP_OK;
}

bool wifi_manager_is_connected(void)
{
    return s_connected;
}

const char *wifi_manager_current_ssid(void)
{
    return s_current_ssid;
}
