// SPDX-License-Identifier: Apache-2.0

#include "plant_input.h"

#include <string.h>

#include "bsp/esp_mosaico.h"
#include "esp_check.h"
#include "esp_gsp_debug.h"
#include "esp_iris.h"
#include "esp_iris_service_profiles.h"
#include "esp_log.h"

static const char *TAG = "plant_input";
static esp_gsp_handle_t s_ui;

static esp_err_t pointer_rpc(const esp_iris_rpc_request_t *request,
                             uint8_t *response, size_t response_capacity,
                             size_t *response_size, void *user_ctx)
{
    (void)user_ctx;
    if (s_ui == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (request == NULL || request->payload == NULL ||
        request->payload_size != ESP_IRIS_POINTER_MESSAGE_SIZE ||
        response == NULL || response_size == NULL ||
        response_capacity < ESP_IRIS_POINTER_MESSAGE_SIZE) {
        return ESP_ERR_INVALID_SIZE;
    }

    const uint8_t *message = request->payload;
    const int16_t x =
        (int16_t)(message[2] | ((uint16_t)message[3] << 8));
    const int16_t y =
        (int16_t)(message[4] | ((uint16_t)message[5] << 8));
    if (message[0] > 2 || message[1] != 0 || x < 0 ||
        x >= BSP_LCD_H_RES || y < 0 || y >= BSP_LCD_V_RES) {
        return ESP_ERR_INVALID_ARG;
    }

    const esp_gsp_err_t gsp_err =
        esp_gsp_inject_touch(s_ui, x, y, message[0] != 2);
    if (gsp_err != ESP_GSP_OK) {
        return ESP_FAIL;
    }
    memcpy(response, message, ESP_IRIS_POINTER_MESSAGE_SIZE);
    *response_size = ESP_IRIS_POINTER_MESSAGE_SIZE;
    return ESP_OK;
}

esp_err_t plant_input_register(esp_gsp_handle_t ui)
{
    ESP_RETURN_ON_FALSE(ui, ESP_ERR_INVALID_ARG, TAG, "ui");
    s_ui = ui;
    return esp_iris_rpc_register(ESP_IRIS_POINTER_SERVICE_ID,
                                 ESP_IRIS_POINTER_METHOD_ID, pointer_rpc,
                                 NULL);
}
