// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef ESP_PLATFORM
#include "esp_err.h"
#else
typedef int esp_err_t;
#ifndef ESP_OK
#define ESP_OK 0
#endif
#ifndef ESP_FAIL
#define ESP_FAIL -1
#endif
#endif

#define PLANT_WIFI_MAX_APS 24
#define PLANT_WIFI_SSID_MAX 32
#define PLANT_WIFI_PASS_MAX 64

typedef struct {
    char ssid[PLANT_WIFI_SSID_MAX + 1];
    bool secure;
    int8_t rssi;
} plant_wifi_ap_t;

typedef enum {
    PLANT_WIFI_CONN_IDLE = 0,
    PLANT_WIFI_CONN_CONNECTING,
    PLANT_WIFI_CONN_CONNECTED,
    PLANT_WIFI_CONN_FAILED,
} plant_wifi_conn_state_t;

#ifdef ESP_PLATFORM

esp_err_t plant_wifi_ensure_started(void);
esp_err_t plant_wifi_request_scan(void);
void plant_wifi_abort_scan(void);
bool plant_wifi_is_scanning(void);
size_t plant_wifi_copy_results(plant_wifi_ap_t *out, size_t capacity);
esp_err_t plant_wifi_connect(const char *ssid, const char *password);
plant_wifi_conn_state_t plant_wifi_conn_state(void);
esp_err_t plant_wifi_last_error(void);
esp_err_t plant_wifi_current_ssid(char *out, size_t capacity);

#else

/* Host simulator keeps the UI mock path in plant_ui.c. */

static inline esp_err_t plant_wifi_ensure_started(void)
{
    return ESP_OK;
}

static inline esp_err_t plant_wifi_request_scan(void)
{
    return ESP_FAIL;
}

static inline void plant_wifi_abort_scan(void) {}

static inline bool plant_wifi_is_scanning(void)
{
    return false;
}

static inline size_t plant_wifi_copy_results(plant_wifi_ap_t *out, size_t capacity)
{
    (void)out;
    (void)capacity;
    return 0;
}

static inline esp_err_t plant_wifi_connect(const char *ssid, const char *password)
{
    (void)ssid;
    (void)password;
    return ESP_FAIL;
}

static inline plant_wifi_conn_state_t plant_wifi_conn_state(void)
{
    return PLANT_WIFI_CONN_IDLE;
}

static inline esp_err_t plant_wifi_last_error(void)
{
    return ESP_FAIL;
}

static inline esp_err_t plant_wifi_current_ssid(char *out, size_t capacity)
{
    if (out && capacity) {
        out[0] = '\0';
    }
    return ESP_FAIL;
}

#endif
