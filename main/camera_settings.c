/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */

#include "camera_settings.h"

#include "esp_check.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "camera_settings";

#define NVS_NAMESPACE              "camera_app"
#define NVS_KEY_PREVIEW_ROTATE_180 "prev_rot180"
#define NVS_KEY_ROTATION           "rotation_v4"
#define DEFAULT_ROTATION_DEGREES   180U

esp_err_t camera_settings_init(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "erase NVS failed");
        ret = nvs_flash_init();
    } else if (ret == ESP_ERR_INVALID_STATE) {
        ret = ESP_OK;
    }
    ESP_RETURN_ON_ERROR(ret, TAG, "initialize NVS failed");
    return ESP_OK;
}

esp_err_t camera_settings_load_preview_rotate_180(bool *rotated)
{
    ESP_RETURN_ON_FALSE(rotated, ESP_ERR_INVALID_ARG, TAG, "output pointer is null");

    nvs_handle_t handle = 0;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        *rotated = false;
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(ret, TAG, "open NVS namespace failed");

    uint8_t value = 0;
    ret = nvs_get_u8(handle, NVS_KEY_PREVIEW_ROTATE_180, &value);
    nvs_close(handle);

    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        *rotated = false;
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(ret, TAG, "read preview rotation setting failed");

    *rotated = value != 0;
    return ESP_OK;
}

esp_err_t camera_settings_save_preview_rotate_180(bool rotated)
{
    nvs_handle_t handle = 0;
    ESP_RETURN_ON_ERROR(
        nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle),
        TAG, "open NVS namespace failed");
    ESP_RETURN_ON_ERROR(
        nvs_set_u8(handle, NVS_KEY_PREVIEW_ROTATE_180, rotated ? 1U : 0U),
        TAG, "write preview rotation setting failed");
    ESP_RETURN_ON_ERROR(nvs_commit(handle), TAG, "commit preview rotation setting failed");
    nvs_close(handle);
    return ESP_OK;
}

esp_err_t camera_settings_load_rotation(uint16_t *degrees)
{
    ESP_RETURN_ON_FALSE(degrees, ESP_ERR_INVALID_ARG, TAG,
                        "output pointer is null");
    nvs_handle_t handle = 0;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        bool legacy = false;
        ESP_RETURN_ON_ERROR(camera_settings_load_preview_rotate_180(&legacy),
                            TAG, "load legacy rotation");
        *degrees = legacy ? 180U : DEFAULT_ROTATION_DEGREES;
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(ret, TAG, "open NVS namespace failed");
    uint16_t value = 0;
    ret = nvs_get_u16(handle, NVS_KEY_ROTATION, &value);
    nvs_close(handle);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        bool legacy = false;
        ESP_RETURN_ON_ERROR(camera_settings_load_preview_rotate_180(&legacy),
                            TAG, "load legacy rotation");
        *degrees = legacy ? 180U : DEFAULT_ROTATION_DEGREES;
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(ret, TAG, "read rotation setting failed");
    *degrees = (value == 90U || value == 180U || value == 270U) ? value : 0U;
    return ESP_OK;
}

esp_err_t camera_settings_save_rotation(uint16_t degrees)
{
    ESP_RETURN_ON_FALSE(degrees == 0U || degrees == 90U || degrees == 180U ||
                            degrees == 270U,
                        ESP_ERR_INVALID_ARG, TAG, "invalid rotation");
    nvs_handle_t handle = 0;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle), TAG,
                        "open NVS namespace failed");
    esp_err_t ret = nvs_set_u16(handle, NVS_KEY_ROTATION, degrees);
    if (ret == ESP_OK) {
        ret = nvs_commit(handle);
    }
    nvs_close(handle);
    return ret;
}
