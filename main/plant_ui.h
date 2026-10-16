// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stddef.h>

#include "esp_gsp.h"
#include "plant_cloud.h"
#include "plant_wifi.h"

typedef struct {
    esp_gsp_handle_t ui;
    void *timer;
    unsigned page;
    unsigned volume;
    unsigned plant_index;
    unsigned gallery_index;
    unsigned gallery_count;
    unsigned gallery_page;
    bool gallery_thumbs_stale;
    void *gallery_timer;
    uint8_t *gallery_jpeg;
    uint16_t *gallery_pixels;
    uint16_t *thumbnail_pixels[6];
    uint32_t thumbnail_indices[6];
    volatile bool thumbnail_borrowed[6];
    volatile uint32_t thumbnail_generation;
    volatile uint32_t thumbnail_workers;
    bool gallery_detail;
    bool delete_armed;
    bool identifying;
    bool saved;
    bool storage_available;
    bool flash_on;
    uint16_t rotation_degrees;
    uint8_t *capture_jpeg;
    size_t capture_jpeg_size;
    plant_result_t result;
    void *identify_task;
    char ssid[PLANT_WIFI_SSID_MAX + 1];
    uint16_t networks;
    unsigned network_total;
    unsigned selected_network;
    plant_wifi_ap_t aps[PLANT_WIFI_MAX_APS];
    void *wifi_timer;
    bool wifi_connecting;
    bool wifi_scanning;
    uint32_t wifi_wait_ms;
    void *volume_timer;
    int16_t gesture_x;
    int16_t gesture_y;
    bool gesture_tracking;
    bool gesture_from_left;
    bool gesture_from_edge;
    esp_gsp_err_t last_error;
} plant_ui_t;

esp_gsp_err_t plant_ui_init(esp_gsp_handle_t ui, plant_ui_t *state);
void plant_ui_deinit(esp_gsp_handle_t ui, plant_ui_t *state);
