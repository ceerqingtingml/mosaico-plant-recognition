/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "voice_tts.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "bsp/esp_mosaico.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_tts.h"
#include "esp_tts_voice_template.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "plant_tts";
static esp_partition_mmap_handle_t s_voice_mmap;
static esp_tts_voice_t *s_voice;
static esp_tts_handle_t s_tts;
static esp_codec_dev_handle_t s_speaker;
static SemaphoreHandle_t s_lock;
static volatile bool s_stop_requested;
static volatile bool s_busy;
static int s_volume = 60;

static void speak_task(void *arg)
{
    char *text = arg;
    if (xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
        if (esp_tts_parse_chinese(s_tts, text)) {
            int samples = 0;
            while (!s_stop_requested) {
                short *pcm = esp_tts_stream_play(s_tts, &samples, 3);
                if (samples <= 0) {
                    break;
                }
                (void)esp_codec_dev_write(
                    s_speaker, pcm, samples * (int)sizeof(short));
            }
            esp_tts_stream_reset(s_tts);
        } else {
            ESP_LOGW(TAG, "Unable to parse TTS text");
        }
        xSemaphoreGive(s_lock);
    }
    s_busy = false;
    free(text);
    vTaskDelete(NULL);
}

esp_err_t voice_tts_init(void)
{
    s_speaker = bsp_audio_codec_speaker_init();
    ESP_RETURN_ON_FALSE(s_speaker, ESP_FAIL, TAG,
                        "initialize speaker codec failed");

    esp_codec_dev_sample_info_t sample_info = {
        .sample_rate = 16000,
        .bits_per_sample = 16,
        .channel = 1,
        .channel_mask = 0,
    };
    ESP_RETURN_ON_FALSE(
        esp_codec_dev_open(s_speaker, &sample_info) == ESP_CODEC_DEV_OK,
        ESP_FAIL, TAG, "open speaker failed");
    (void)esp_codec_dev_set_out_vol(s_speaker, s_volume);

    const esp_partition_t *partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "voice_data");
    ESP_RETURN_ON_FALSE(partition, ESP_ERR_NOT_FOUND, TAG,
                        "voice_data partition not found");

    const void *voice_data = NULL;
    ESP_RETURN_ON_ERROR(
        esp_partition_mmap(partition, 0, partition->size,
                           ESP_PARTITION_MMAP_DATA, &voice_data, &s_voice_mmap),
        TAG, "map voice data failed");

    s_voice =
        esp_tts_voice_set_init(&esp_tts_voice_template, (void *)voice_data);
    ESP_RETURN_ON_FALSE(s_voice, ESP_FAIL, TAG,
                        "initialize Xiaole voice failed");
    s_tts = esp_tts_create(s_voice);
    ESP_RETURN_ON_FALSE(s_tts, ESP_FAIL, TAG, "create TTS engine failed");

    s_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_NO_MEM, TAG,
                        "create TTS lock failed");
    return ESP_OK;
}

esp_err_t voice_tts_set_volume(int volume)
{
    if (volume < 0 || volume > 100) {
        return ESP_ERR_INVALID_ARG;
    }
    s_volume = volume;
    if (!s_speaker) {
        return ESP_OK;
    }
    return esp_codec_dev_set_out_vol(s_speaker, volume) == ESP_CODEC_DEV_OK
               ? ESP_OK
               : ESP_FAIL;
}

int voice_tts_get_volume(void)
{
    return s_volume;
}

esp_err_t voice_tts_speak_async(const char *text)
{
    ESP_RETURN_ON_FALSE(s_tts && s_speaker && text && text[0],
                        ESP_ERR_INVALID_STATE, TAG, "TTS is not ready");
    ESP_RETURN_ON_FALSE(!s_busy, ESP_ERR_INVALID_STATE, TAG,
                        "TTS is already speaking");

    char *copy = strdup(text);
    ESP_RETURN_ON_FALSE(copy, ESP_ERR_NO_MEM, TAG,
                        "allocate TTS text failed");
    s_stop_requested = false;
    s_busy = true;
    if (xTaskCreate(speak_task, "plant_tts", 6144, copy, 4, NULL) != pdPASS) {
        s_busy = false;
        free(copy);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void voice_tts_stop(void)
{
    s_stop_requested = true;
}
