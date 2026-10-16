// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "esp_err.h"
#include "esp_gsp.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t plant_input_register(esp_gsp_handle_t ui);

#ifdef __cplusplus
}
#endif
