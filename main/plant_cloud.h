/*
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char common_name[64];
    char scientific_name[96];
    char family[64];
    char summary[384];
    char poem[192];
    char poem_source[96];
    char audio_url[256];
    float confidence;
} plant_result_t;

esp_err_t plant_cloud_identify(const uint8_t *jpeg, size_t jpeg_size,
                               plant_result_t *out_result);

#ifdef __cplusplus
}
#endif
