/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "plant_cloud.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "mbedtls/base64.h"

#define JPEG_MAX_SIZE      (256U * 1024U)
#define RESPONSE_MAX_SIZE  (12U * 1024U)

static const char *TAG = "plant_cloud";

static const char REQUEST_PREFIX_FORMAT[] =
    "{\"model\":\"%s\",\"messages\":[{\"role\":\"user\",\"content\":["
    "{\"type\":\"image_url\",\"image_url\":{\"url\":"
    "\"data:image/jpeg;base64,";

static const char REQUEST_SUFFIX[] =
    "\"}},{\"type\":\"text\",\"text\":\""
    "请识别图片主体。如果主体是植物，请返回可靠的中文常用名、拉丁学名、科名、"
    "置信度、简短科普，以及一首确实存在且与该植物相关的中国古诗句和出处。"
    "如果不能确认诗句出处，poem 和 poem_source 都返回空字符串，严禁编造。"
    "如果图片主体不是植物或无法可靠识别，common_name 返回‘未识别到植物’，"
    "scientific_name、family、poem、poem_source 返回空字符串，并在 summary "
    "中简要说明图片内容。只返回一个紧凑 JSON 对象，不要 Markdown，不要额外文字。"
    "字段必须为 common_name、scientific_name、family、confidence、summary、"
    "poem、poem_source；confidence 是 0 到 1 的数字。summary 不超过 120 个"
    "汉字，poem 不超过 50 个汉字，poem_source 不超过 30 个汉字。"
    "\"}]}],\"temperature\":0.1,\"max_tokens\":600,"
    "\"response_format\":{\"type\":\"json_object\"}}";

static esp_err_t write_all(esp_http_client_handle_t client,
                           const uint8_t *data, size_t size)
{
    size_t written = 0;
    while (written < size) {
        const int chunk = esp_http_client_write(
            client, (const char *)data + written, (int)(size - written));
        if (chunk <= 0) {
            return ESP_FAIL;
        }
        written += (size_t)chunk;
    }
    return ESP_OK;
}

static esp_err_t copy_json_string(const cJSON *object, const char *key,
                                  char *destination, size_t destination_size,
                                  bool required)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsString(item) || !item->valuestring) {
        if (required) {
            ESP_LOGE(TAG, "Response is missing string field \"%s\"", key);
            return ESP_ERR_INVALID_RESPONSE;
        }
        destination[0] = '\0';
        return ESP_OK;
    }
    strlcpy(destination, item->valuestring, destination_size);
    return ESP_OK;
}

static esp_err_t parse_response(const char *json, plant_result_t *result)
{
    cJSON *root = cJSON_Parse(json);
    ESP_RETURN_ON_FALSE(root, ESP_ERR_INVALID_RESPONSE, TAG,
                        "invalid Qwen JSON response");

    const cJSON *choices = cJSON_GetObjectItemCaseSensitive(root, "choices");
    const cJSON *choice = cJSON_IsArray(choices)
                              ? cJSON_GetArrayItem(choices, 0)
                              : NULL;
    const cJSON *message = cJSON_IsObject(choice)
                               ? cJSON_GetObjectItemCaseSensitive(
                                     choice, "message")
                               : NULL;
    const cJSON *content = cJSON_IsObject(message)
                               ? cJSON_GetObjectItemCaseSensitive(
                                     message, "content")
                               : NULL;
    if (!cJSON_IsString(content) || !content->valuestring) {
        cJSON_Delete(root);
        ESP_RETURN_ON_FALSE(false, ESP_ERR_INVALID_RESPONSE, TAG,
                            "Qwen response has no message content");
    }

    const char *object_start = strchr(content->valuestring, '{');
    const char *object_end = strrchr(content->valuestring, '}');
    if (!object_start || !object_end || object_end < object_start) {
        cJSON_Delete(root);
        ESP_RETURN_ON_FALSE(false, ESP_ERR_INVALID_RESPONSE, TAG,
                            "Qwen content has no JSON object");
    }

    const size_t object_size = (size_t)(object_end - object_start) + 1U;
    char *object_json = heap_caps_malloc(
        object_size + 1U, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!object_json) {
        object_json = malloc(object_size + 1U);
    }
    if (!object_json) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    memcpy(object_json, object_start, object_size);
    object_json[object_size] = '\0';

    cJSON *plant = cJSON_Parse(object_json);
    free(object_json);
    if (!cJSON_IsObject(plant)) {
        cJSON_Delete(root);
        ESP_RETURN_ON_FALSE(false, ESP_ERR_INVALID_RESPONSE, TAG,
                            "Qwen content is not valid plant JSON");
    }

    esp_err_t ret = copy_json_string(
        plant, "common_name", result->common_name,
        sizeof(result->common_name), true);
    if (ret == ESP_OK) {
        ret = copy_json_string(
            plant, "scientific_name", result->scientific_name,
            sizeof(result->scientific_name), true);
    }
    if (ret == ESP_OK) {
        ret = copy_json_string(plant, "family", result->family,
                               sizeof(result->family), false);
    }
    if (ret == ESP_OK) {
        ret = copy_json_string(plant, "summary", result->summary,
                               sizeof(result->summary), false);
    }
    if (ret == ESP_OK) {
        ret = copy_json_string(plant, "poem", result->poem,
                               sizeof(result->poem), false);
    }
    if (ret == ESP_OK) {
        ret = copy_json_string(plant, "poem_source", result->poem_source,
                               sizeof(result->poem_source), false);
    }
    result->audio_url[0] = '\0';

    const cJSON *confidence =
        cJSON_GetObjectItemCaseSensitive(plant, "confidence");
    result->confidence =
        cJSON_IsNumber(confidence) ? (float)confidence->valuedouble : 0.0f;
    if (result->confidence < 0.0f) {
        result->confidence = 0.0f;
    } else if (result->confidence > 1.0f) {
        result->confidence = 1.0f;
    }
    cJSON_Delete(plant);
    cJSON_Delete(root);
    return ret;
}

esp_err_t plant_cloud_identify(const uint8_t *jpeg, size_t jpeg_size,
                               plant_result_t *out_result)
{
    ESP_RETURN_ON_FALSE(jpeg && jpeg_size > 0 && out_result,
                        ESP_ERR_INVALID_ARG, TAG,
                        "invalid recognition arguments");
    ESP_RETURN_ON_FALSE(jpeg_size <= JPEG_MAX_SIZE, ESP_ERR_INVALID_SIZE, TAG,
                        "JPEG is too large: %u bytes (maximum %u)",
                        (unsigned)jpeg_size, (unsigned)JPEG_MAX_SIZE);
    ESP_RETURN_ON_FALSE(CONFIG_QWEN_API_KEY[0] != '\0',
                        ESP_ERR_INVALID_STATE, TAG,
                        "Qwen API key is not configured");

    memset(out_result, 0, sizeof(*out_result));

    const size_t base64_capacity = 4U * ((jpeg_size + 2U) / 3U) + 1U;
    unsigned char *base64 = heap_caps_malloc(
        base64_capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_RETURN_ON_FALSE(base64, ESP_ERR_NO_MEM, TAG,
                        "allocate JPEG Base64 buffer failed");

    size_t base64_size = 0;
    int base64_ret = mbedtls_base64_encode(
        base64, base64_capacity, &base64_size, jpeg, jpeg_size);
    if (base64_ret != 0) {
        ESP_LOGE(TAG, "encode JPEG as Base64 failed: -0x%04x", -base64_ret);
        free(base64);
        return ESP_FAIL;
    }

    const int prefix_length = snprintf(
        NULL, 0, REQUEST_PREFIX_FORMAT, CONFIG_QWEN_MODEL);
    if (prefix_length <= 0) {
        free(base64);
        return ESP_FAIL;
    }
    char *prefix = malloc((size_t)prefix_length + 1U);
    if (!prefix) {
        free(base64);
        return ESP_ERR_NO_MEM;
    }
    snprintf(prefix, (size_t)prefix_length + 1U, REQUEST_PREFIX_FORMAT,
             CONFIG_QWEN_MODEL);

    esp_http_client_config_t config = {
        .url = CONFIG_QWEN_API_URL,
        .timeout_ms = CONFIG_QWEN_API_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 4096,
        .buffer_size_tx = 2048,
        .keep_alive_enable = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        free(prefix);
        free(base64);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = esp_http_client_set_method(client, HTTP_METHOD_POST);
    if (ret == ESP_OK) {
        ret = esp_http_client_set_header(
            client, "Content-Type", "application/json");
    }
    static const char authorization[] = "Bearer " CONFIG_QWEN_API_KEY;
    if (ret == ESP_OK) {
        ret = esp_http_client_set_header(
            client, "Authorization", authorization);
    }

    const size_t content_length =
        (size_t)prefix_length + base64_size + sizeof(REQUEST_SUFFIX) - 1U;
    if (ret == ESP_OK) {
        ret = esp_http_client_open(client, (int)content_length);
    }
    if (ret == ESP_OK) {
        ret = write_all(client, (const uint8_t *)prefix,
                        (size_t)prefix_length);
    }
    if (ret == ESP_OK) {
        ret = write_all(client, base64, base64_size);
    }
    if (ret == ESP_OK) {
        ret = write_all(client, (const uint8_t *)REQUEST_SUFFIX,
                        sizeof(REQUEST_SUFFIX) - 1U);
    }
    free(prefix);
    free(base64);

    int64_t content_length_response = -1;
    if (ret == ESP_OK) {
        content_length_response = esp_http_client_fetch_headers(client);
        if (content_length_response < 0) {
            ret = ESP_FAIL;
        }
    }

    char *response = NULL;
    if (ret == ESP_OK) {
        response = heap_caps_calloc(
            1, RESPONSE_MAX_SIZE + 1U,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!response) {
            response = calloc(1, RESPONSE_MAX_SIZE + 1U);
        }
        if (!response) {
            ret = ESP_ERR_NO_MEM;
        }
    }

    size_t total_read = 0;
    while (ret == ESP_OK && total_read < RESPONSE_MAX_SIZE) {
        const int read = esp_http_client_read(
            client, response + total_read,
            (int)(RESPONSE_MAX_SIZE - total_read));
        if (read < 0) {
            ret = ESP_FAIL;
            break;
        }
        if (read == 0) {
            break;
        }
        total_read += (size_t)read;
    }

    if (ret == ESP_OK &&
        ((content_length_response > RESPONSE_MAX_SIZE) ||
         !esp_http_client_is_complete_data_received(client))) {
        ESP_LOGE(TAG, "Recognition response is incomplete or too large");
        ret = ESP_ERR_INVALID_SIZE;
    }

    if (ret == ESP_OK) {
        response[total_read] = '\0';
        const int status = esp_http_client_get_status_code(client);
        if (status < 200 || status >= 300) {
            ESP_LOGE(TAG, "Qwen returned HTTP %d: %.240s", status, response);
            ret = ESP_ERR_INVALID_RESPONSE;
        } else {
            ret = parse_response(response, out_result);
        }
    }

    free(response);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return ret;
}
