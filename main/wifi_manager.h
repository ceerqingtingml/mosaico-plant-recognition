/*
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WIFI_MANAGER_MAX_APS 16
#define WIFI_MANAGER_SSID_MAX_LEN 32

typedef struct {
    char ssid[WIFI_MANAGER_SSID_MAX_LEN + 1];
    int8_t rssi;
    bool secure;
} wifi_manager_ap_t;

typedef void (*wifi_manager_state_cb_t)(bool connected, const char *ssid,
                                        esp_err_t reason, void *user_data);

esp_err_t wifi_manager_init(wifi_manager_state_cb_t callback, void *user_data);
esp_err_t wifi_manager_try_saved(uint32_t timeout_ms);
esp_err_t wifi_manager_scan(wifi_manager_ap_t *aps, size_t capacity,
                            size_t *out_count);
esp_err_t wifi_manager_connect(const char *ssid, const char *password,
                               uint32_t timeout_ms);
esp_err_t wifi_manager_forget(void);
bool wifi_manager_is_connected(void);
const char *wifi_manager_current_ssid(void);

#ifdef __cplusplus
}
#endif
