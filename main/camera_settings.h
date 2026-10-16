/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t camera_settings_init(void);

esp_err_t camera_settings_load_preview_rotate_180(bool *rotated);
esp_err_t camera_settings_save_preview_rotate_180(bool rotated);
esp_err_t camera_settings_load_rotation(uint16_t *degrees);
esp_err_t camera_settings_save_rotation(uint16_t degrees);

#ifdef __cplusplus
}
#endif
