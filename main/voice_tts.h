/*
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t voice_tts_init(void);
esp_err_t voice_tts_speak_async(const char *text);
esp_err_t voice_tts_set_volume(int volume);
int voice_tts_get_volume(void);
void voice_tts_stop(void);

#ifdef __cplusplus
}
#endif
