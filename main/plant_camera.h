// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_gsp.h"

#ifdef ESP_PLATFORM
#include "esp_err.h"

esp_err_t plant_camera_start_preview(esp_gsp_handle_t ui, uint16_t preview_bind);
void plant_camera_stop_preview(void);
bool plant_camera_preview_running(void);
esp_err_t plant_camera_wait_preview_stopped(uint32_t timeout_ms);
esp_err_t plant_camera_capture_jpeg(uint8_t **out_jpeg, size_t *out_size,
                                    uint32_t *out_width, uint32_t *out_height);
esp_err_t plant_camera_push_snapshot(esp_gsp_handle_t ui,
                                     uint16_t snapshot_bind);
uint16_t plant_camera_rotate_clockwise(void);
uint16_t plant_camera_rotation_degrees(void);
esp_err_t plant_camera_set_rotation(uint16_t degrees);
void plant_camera_set_flash_enabled(bool enabled);

#else

typedef int esp_err_t;
#ifndef ESP_OK
#define ESP_OK 0
#endif
#ifndef ESP_FAIL
#define ESP_FAIL -1
#endif

static inline esp_err_t plant_camera_start_preview(esp_gsp_handle_t ui,
                                                   uint16_t preview_bind)
{
    (void)ui;
    (void)preview_bind;
    return ESP_FAIL;
}

static inline void plant_camera_stop_preview(void) {}

static inline esp_err_t plant_camera_wait_preview_stopped(uint32_t timeout_ms)
{
    (void)timeout_ms;
    return ESP_OK;
}

static inline bool plant_camera_preview_running(void)
{
    return false;
}

static inline esp_err_t plant_camera_capture_jpeg(uint8_t **out_jpeg,
                                                  size_t *out_size,
                                                  uint32_t *out_width,
                                                  uint32_t *out_height)
{
    (void)out_jpeg;
    (void)out_size;
    (void)out_width;
    (void)out_height;
    return ESP_FAIL;
}

static inline esp_err_t plant_camera_push_snapshot(esp_gsp_handle_t ui,
                                                   uint16_t snapshot_bind)
{
    (void)ui;
    (void)snapshot_bind;
    return ESP_FAIL;
}

static inline uint16_t plant_camera_rotate_clockwise(void)
{
    return 0;
}

static inline uint16_t plant_camera_rotation_degrees(void)
{
    return 0;
}

static inline esp_err_t plant_camera_set_rotation(uint16_t degrees)
{
    (void)degrees;
    return ESP_FAIL;
}

static inline void plant_camera_set_flash_enabled(bool enabled)
{
    (void)enabled;
}

#endif
