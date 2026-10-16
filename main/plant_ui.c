// SPDX-License-Identifier: Apache-2.0

#include "plant_ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_cache.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_jpeg_dec.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "camera_settings.h"
#include "plant_camera.h"
#include "photo_store.h"
#include "voice_tts.h"

#define GSP_BUNDLE_ENABLE_RAW_IDS
#include "bundle_gsp.h"

#define PAGE_HOME 0
#define PAGE_CAMERA 1
#define PAGE_WIFI 2
#define PAGE_RESULT 3
#define PAGE_GALLERY 4
#define PAGE_PASSWORD 5
#define MOCK_NETWORK_COUNT 3
#define SCAN_POLL_MS 200
#define SCAN_TIMEOUT_MS 12000
#define CONNECT_POLL_MS 200
#define CONNECT_TIMEOUT_MS 20000
#define CONNECT_MS 800
#define VOLUME_POLL_MS 200
#define EDGE_ZONE_PX 64
#define BACK_SWIPE_DISTANCE_PX 56
#define BACK_SWIPE_MAX_Y_PX 80
#define GALLERY_SWIPE_PX 40
#define COLOR_ROW 0xFFFFu
#define COLOR_LOCK 0x740Fu
#define COLOR_WIFI_CONNECTED 0x438Cu
#define COLOR_WIFI_DISCONNECTED 0x740Fu
#define COLOR_WIFI_CONNECTING 0xEDEDu
#define PHOTO_WIDTH 480U
#define PHOTO_HEIGHT 480U
#define PHOTO_RGB_BYTES (PHOTO_WIDTH * PHOTO_HEIGHT * sizeof(uint16_t))
#define PHOTO_JPEG_MAX (256U * 1024U)
#define THUMB_SIZE 128U
#define THUMB_COUNT 6U
#define THUMB_ORIGIN_X0 30
#define THUMB_ORIGIN_X1 176
#define THUMB_ORIGIN_X2 322
#define THUMB_ORIGIN_Y0 108
#define THUMB_ORIGIN_Y1 256
#define THUMB_TAP_SLOP_PX 16
#define THUMB_RGB_BYTES (THUMB_SIZE * THUMB_SIZE * sizeof(uint16_t))
#define THUMB_PUSH_RETRY_MS 10U
#define THUMB_PUSH_RETRY_COUNT 100U
#define THUMB_TASK_STACK 4096U
#define THUMB_TASK_PRIORITY 4U

static const char *TAG = "plant_ui";

static const uint16_t s_thumb_surface_binds[THUMB_COUNT] = {
    GSP_PLANT_BIND_THUMB_0_SURFACE, GSP_PLANT_BIND_THUMB_1_SURFACE,
    GSP_PLANT_BIND_THUMB_2_SURFACE, GSP_PLANT_BIND_THUMB_3_SURFACE,
    GSP_PLANT_BIND_THUMB_4_SURFACE, GSP_PLANT_BIND_THUMB_5_SURFACE,
};

typedef struct {
    plant_ui_t *state;
    uint32_t generation;
    uint32_t ready_mask;
} thumbnail_publish_context_t;

#ifndef ESP_PLATFORM
typedef struct {
    const char *ssid;
    bool secure;
} network_sample_t;

static const network_sample_t k_mock_networks[MOCK_NETWORK_COUNT] = {
    {"PlantLab", true},
    {"ESP-Guest", false},
    {"Home-5G", true},
};
#endif

static void enter_camera(plant_ui_t *state);
static void leave_camera(plant_ui_t *state);
static void show_gallery_grid(plant_ui_t *state);
static void show_gallery_detail(plant_ui_t *state);
static void refresh_gallery_thumbs(esp_gsp_handle_t ui, void *ctx);

static const plant_wifi_ap_t *selected_ap(const plant_ui_t *state)
{
    if (state->selected_network >= state->network_total) {
        return NULL;
    }
    return &state->aps[state->selected_network];
}

static void note(plant_ui_t *state, esp_gsp_err_t err)
{
    if (err != ESP_GSP_OK) {
        state->last_error = err;
    }
}

static void text(plant_ui_t *state, uint16_t bind, const char *value)
{
    note(state, esp_gsp_set_text(state->ui, bind, value));
}

static void set_delete_dialog(plant_ui_t *state, bool visible)
{
    state->delete_armed = visible;
    note(state, esp_gsp_set_visible(
                    state->ui, GSP_PLANT_BIND_GALLERY_DELETE_VISIBLE, visible));
}

static void confirm_gallery_delete(plant_ui_t *state)
{
    if (state->gallery_count > 0 &&
        state->gallery_index < state->gallery_count) {
        (void)photo_store_delete(state->gallery_index);
        state->gallery_count = photo_store_get_count();
        if (state->gallery_index >= state->gallery_count &&
            state->gallery_count > 0) {
            state->gallery_index = state->gallery_count - 1;
        }
    }
    set_delete_dialog(state, false);
    if (state->gallery_count == 0) {
        show_gallery_grid(state);
    } else {
        show_gallery_detail(state);
    }
}

static void show(plant_ui_t *state, unsigned page)
{
    static const uint16_t visibility[] = {
        GSP_PLANT_BIND_HOME_VISIBLE, GSP_PLANT_BIND_CAMERA_VISIBLE,
        GSP_PLANT_BIND_WIFI_VISIBLE, GSP_PLANT_BIND_RESULT_VISIBLE,
        GSP_PLANT_BIND_GALLERY_VISIBLE, GSP_PLANT_BIND_PASSWORD_VISIBLE,
    };
    static bool booted;

    if (booted && state->page == page) {
        return;
    }
    if (booted && state->page == PAGE_CAMERA) {
        leave_camera(state);
    }
    if (!booted) {
        for (unsigned i = 0; i < 6; ++i) {
            note(state,
                 esp_gsp_set_visible(state->ui, visibility[i], i == page));
        }
        booted = true;
    } else {
        note(state, esp_gsp_set_visible(state->ui, visibility[state->page], false));
        note(state, esp_gsp_set_visible(state->ui, visibility[page], true));
    }
    state->page = page;
    if (page == PAGE_CAMERA) {
        enter_camera(state);
    }
}

static void show_wifi_label(plant_ui_t *state)
{
    char status[64];
    uint32_t color = COLOR_WIFI_DISCONNECTED;
    bool connected = false;
    bool connecting = state->wifi_connecting;
#ifdef ESP_PLATFORM
    const plant_wifi_conn_state_t connection = plant_wifi_conn_state();
    connected = connection == PLANT_WIFI_CONN_CONNECTED;
    connecting = connection == PLANT_WIFI_CONN_CONNECTING;
#else
    connected = state->ssid[0] != '\0' && !connecting;
#endif

    /* Network state belongs on the Wi-Fi page, not under the home title. */
    text(state, GSP_PLANT_BIND_WIFI_LABEL, " ");
    if (connected) {
        color = COLOR_WIFI_CONNECTED;
        if (state->ssid[0]) {
            snprintf(status, sizeof(status), "已连接 · %s", state->ssid);
        } else {
            strlcpy(status, "已连接", sizeof(status));
        }
    } else if (connecting) {
        color = COLOR_WIFI_CONNECTING;
        strlcpy(status, "正在连接…", sizeof(status));
    } else {
        strlcpy(status, "未连接", sizeof(status));
    }
    note(state, esp_gsp_set_color(state->ui, GSP_PLANT_BIND_WIFI_INDICATOR,
                                  color));
    text(state, GSP_PLANT_BIND_WIFI_CONNECTION, status);
}

static void show_camera_labels(plant_ui_t *state)
{
    char rotation[24];
    snprintf(rotation, sizeof(rotation), "画面 %u°",
             (unsigned)state->rotation_degrees);
    text(state, GSP_PLANT_BIND_FLASH_LABEL,
         state->flash_on ? "闪光灯开" : "闪光灯关");
    text(state, GSP_PLANT_BIND_ROTATE_LABEL, rotation);
    text(state, GSP_PLANT_BIND_PREVIEW_NOTE, "让植物完整出现在框内");
}

static void enter_camera(plant_ui_t *state)
{
    show_camera_labels(state);
#ifdef ESP_PLATFORM
#ifdef GSP_PLANT_BIND_PREVIEW_SURFACE
    if (plant_camera_start_preview(state->ui, GSP_PLANT_BIND_PREVIEW_SURFACE) !=
        ESP_OK) {
        note(state, esp_gsp_set_visible(state->ui, GSP_PLANT_BIND_PREVIEW_NOTE,
                                        true));
        text(state, GSP_PLANT_BIND_PREVIEW_NOTE, "相机打开失败，请检查模组");
    } else {
        /* Keep the tip/error label out of the live preview area. */
        note(state, esp_gsp_set_visible(state->ui, GSP_PLANT_BIND_PREVIEW_NOTE,
                                        false));
    }
#else
    text(state, GSP_PLANT_BIND_PREVIEW_NOTE, "界面缺少预览画布");
#endif
#else
    text(state, GSP_PLANT_BIND_PREVIEW_NOTE, "请在设备上查看实时预览");
#endif
}

static void leave_camera(plant_ui_t *state)
{
    plant_camera_stop_preview();
    note(state, esp_gsp_set_visible(state->ui, GSP_PLANT_BIND_PREVIEW_NOTE, true));
}

static esp_err_t decode_gallery_image(plant_ui_t *state, uint32_t index)
{
    if (!state->gallery_jpeg) {
        state->gallery_jpeg = heap_caps_malloc(
            PHOTO_JPEG_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (!state->gallery_pixels) {
        state->gallery_pixels = heap_caps_aligned_calloc(
            128, 1, PHOTO_RGB_BYTES,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    }
    if (!state->gallery_jpeg || !state->gallery_pixels) {
        return ESP_ERR_NO_MEM;
    }

    size_t jpeg_size = 0;
    ESP_RETURN_ON_ERROR(
        photo_store_load_jpeg(index, state->gallery_jpeg, PHOTO_JPEG_MAX,
                              &jpeg_size),
        TAG, "load gallery JPEG");

    jpeg_dec_config_t config = DEFAULT_JPEG_DEC_CONFIG();
    config.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;
    jpeg_dec_handle_t decoder = NULL;
    jpeg_error_t jpeg_ret = jpeg_dec_open(&config, &decoder);
    ESP_RETURN_ON_FALSE(jpeg_ret == JPEG_ERR_OK, ESP_FAIL, TAG,
                        "open JPEG decoder: %d", jpeg_ret);

    jpeg_dec_io_t io = {
        .inbuf = state->gallery_jpeg,
        .inbuf_len = (int)jpeg_size,
        .outbuf = (uint8_t *)state->gallery_pixels,
    };
    jpeg_dec_header_info_t header = {0};
    jpeg_ret = jpeg_dec_parse_header(decoder, &io, &header);
    if (jpeg_ret != JPEG_ERR_OK || header.width != PHOTO_WIDTH ||
        header.height != PHOTO_HEIGHT) {
        jpeg_dec_close(decoder);
        return ESP_ERR_INVALID_SIZE;
    }
    int output_size = 0;
    jpeg_ret = jpeg_dec_get_outbuf_len(decoder, &output_size);
    if (jpeg_ret != JPEG_ERR_OK || output_size > (int)PHOTO_RGB_BYTES) {
        jpeg_dec_close(decoder);
        return ESP_ERR_INVALID_SIZE;
    }
    jpeg_ret = jpeg_dec_process(decoder, &io);
    jpeg_dec_close(decoder);
    ESP_RETURN_ON_FALSE(jpeg_ret == JPEG_ERR_OK, ESP_FAIL, TAG,
                        "decode gallery JPEG: %d", jpeg_ret);
    /*
     * esp_new_jpeg decodes on the CPU. The canvas and thumbnail path then
     * read this buffer through DMA, so the new pixels have to reach memory.
     * Invalidating here would discard the tail still sitting in cache, which
     * is the striped band at the bottom of an opened photo.
     */
    ESP_RETURN_ON_ERROR(
        esp_cache_msync(state->gallery_pixels, PHOTO_RGB_BYTES,
                        ESP_CACHE_MSYNC_FLAG_DIR_C2M),
        TAG, "write back decoded gallery image");

    return ESP_OK;
}

static void show_gallery_detail(plant_ui_t *state)
{
    char title[32];
    state->gallery_count =
        state->storage_available ? photo_store_get_count() : 0;
    if (state->gallery_count == 0) {
        text(state, GSP_PLANT_BIND_GALLERY_TITLE, "相册是空的");
        text(state, GSP_PLANT_BIND_GALLERY_CAPTION, "识别后点保存才会出现在这里");
        return;
    }
    if (state->gallery_index >= state->gallery_count) {
        state->gallery_index = state->gallery_count - 1;
    }
    snprintf(title, sizeof(title), "相册 %u/%u", state->gallery_index + 1,
             state->gallery_count);
    text(state, GSP_PLANT_BIND_GALLERY_TITLE, title);
    photo_store_info_t info = {0};
    if (photo_store_get_info(state->gallery_index, &info) == ESP_OK &&
        decode_gallery_image(state, state->gallery_index) == ESP_OK &&
        esp_gsp_canvas_try_push(
            state->ui, GSP_PLANT_BIND_GALLERY_PHOTO_SURFACE,
            state->gallery_pixels, PHOTO_WIDTH * sizeof(uint16_t), NULL,
            NULL) == ESP_GSP_OK) {
        char caption[48];
        snprintf(caption, sizeof(caption), "%s · %u KB", info.filename,
                 (unsigned)((info.size_bytes + 1023U) / 1024U));
        text(state, GSP_PLANT_BIND_GALLERY_CAPTION, caption);
    } else {
        text(state, GSP_PLANT_BIND_GALLERY_CAPTION, "照片读取失败");
    }
}

static void thumbnail_frame_released(void *ctx)
{
    volatile bool *borrowed = ctx;
    if (borrowed) {
        *borrowed = false;
    }
}

static void thumbnail_publish_task(void *arg)
{
    thumbnail_publish_context_t *ctx = arg;
    plant_ui_t *state = ctx->state;

    for (uint32_t slot = 0; slot < THUMB_COUNT; ++slot) {
        if (ctx->generation != state->thumbnail_generation ||
            !(ctx->ready_mask & (1U << slot))) {
            continue;
        }

        /*
         * A still frame stays borrowed until that canvas is replaced or
         * stopped. Each cell has its own buffer, so publish the rest without
         * waiting for this release.
         */
        state->thumbnail_borrowed[slot] = true;
        esp_gsp_err_t push = ESP_GSP_ERR_TIMEOUT;
        for (uint32_t retry = 0; retry < THUMB_PUSH_RETRY_COUNT; ++retry) {
            push = esp_gsp_canvas_try_push(
                state->ui, s_thumb_surface_binds[slot],
                state->thumbnail_pixels[slot],
                THUMB_SIZE * sizeof(uint16_t), thumbnail_frame_released,
                (void *)&state->thumbnail_borrowed[slot]);
            if (push == ESP_GSP_OK) {
                break;
            }
            if (push != ESP_GSP_ERR_TIMEOUT ||
                ctx->generation != state->thumbnail_generation) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(THUMB_PUSH_RETRY_MS));
        }

        if (push != ESP_GSP_OK) {
            state->thumbnail_borrowed[slot] = false;
            ESP_LOGW(TAG, "thumbnail %u push failed: 0x%x",
                     (unsigned)slot, (unsigned)push);
        }
    }

    state->thumbnail_workers--;
    free(ctx);
    vTaskDelete(NULL);
}

static uint32_t gallery_page_count(const plant_ui_t *state)
{
    if (state->gallery_count == 0) {
        return 1;
    }
    return (state->gallery_count + THUMB_COUNT - 1U) / THUMB_COUNT;
}

static void show_gallery_page_label(plant_ui_t *state)
{
    char hint[32];
    const uint32_t pages = gallery_page_count(state);

    if (state->gallery_page >= pages) {
        state->gallery_page = pages - 1U;
    }
    if (state->gallery_count == 0 || pages <= 1U) {
        text(state, GSP_PLANT_BIND_GALLERY_PAGE, "点击照片查看");
        return;
    }
    snprintf(hint, sizeof(hint),
             state->gallery_page + 1U < pages ? "%u/%u 上滑" : "%u/%u 下滑",
             (unsigned)(state->gallery_page + 1U), (unsigned)pages);
    text(state, GSP_PLANT_BIND_GALLERY_PAGE, hint);
}

static void show_gallery_grid(plant_ui_t *state)
{
    static const uint16_t visible_binds[THUMB_COUNT] = {
        GSP_PLANT_BIND_THUMB_0_VISIBLE, GSP_PLANT_BIND_THUMB_1_VISIBLE,
        GSP_PLANT_BIND_THUMB_2_VISIBLE, GSP_PLANT_BIND_THUMB_3_VISIBLE,
        GSP_PLANT_BIND_THUMB_4_VISIBLE, GSP_PLANT_BIND_THUMB_5_VISIBLE,
    };
    state->gallery_count =
        state->storage_available ? photo_store_get_count() : 0;
    state->gallery_detail = false;
    set_delete_dialog(state, false);
    show_gallery_page_label(state);
    note(state, esp_gsp_set_visible(
                    state->ui, GSP_PLANT_BIND_GALLERY_GRID_VISIBLE, true));
    note(state, esp_gsp_set_visible(
                    state->ui, GSP_PLANT_BIND_GALLERY_DETAIL_VISIBLE, false));
    text(state, GSP_PLANT_BIND_GALLERY_EMPTY_STATUS,
         state->gallery_count ? "" : "识别后点保存才会出现在这里");

    if (state->thumbnail_workers != 0) {
        /*
         * The previous page is still publishing. Drop that work and try
         * again from the timer instead of ignoring the page change.
         */
        state->gallery_thumbs_stale = true;
        state->thumbnail_generation++;
        if (!state->gallery_timer) {
            state->gallery_timer = esp_gsp_timer_create(
                state->ui, 100, refresh_gallery_thumbs, state);
        }
        return;
    }
    state->gallery_thumbs_stale = false;
    /*
     * Drop canvases that are not on this grid. Each pushed image keeps a
     * canvas target until it is stopped, and the gallery needs one per cell.
     */
    (void)esp_gsp_canvas_stop(state->ui, GSP_PLANT_BIND_PREVIEW_SURFACE);
    (void)esp_gsp_canvas_stop(state->ui, GSP_PLANT_BIND_RESULT_PHOTO_SURFACE);
    (void)esp_gsp_canvas_stop(state->ui, GSP_PLANT_BIND_GALLERY_PHOTO_SURFACE);
    for (uint32_t slot = 0; slot < THUMB_COUNT; ++slot) {
        if (!state->thumbnail_borrowed[slot]) {
            continue;
        }
        (void)esp_gsp_canvas_stop(state->ui, s_thumb_surface_binds[slot]);
        for (uint32_t retry = 0; retry < THUMB_PUSH_RETRY_COUNT; ++retry) {
            if (!state->thumbnail_borrowed[slot]) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(THUMB_PUSH_RETRY_MS));
        }
    }

    uint32_t ready_mask = 0;
    for (uint32_t slot = 0; slot < THUMB_COUNT; ++slot) {
        const uint32_t offset = state->gallery_page * THUMB_COUNT + slot;
        const bool used = offset < state->gallery_count;
        note(state, esp_gsp_set_visible(state->ui, visible_binds[slot], used));
        if (!used) {
            continue;
        }
        const uint32_t index = state->gallery_count - 1U - offset;
        state->thumbnail_indices[slot] = index;
        if (!state->thumbnail_pixels[slot]) {
            state->thumbnail_pixels[slot] = heap_caps_aligned_calloc(
                128, 1, THUMB_RGB_BYTES,
                MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
        }
        if (state->thumbnail_borrowed[slot] || !state->thumbnail_pixels[slot] ||
            decode_gallery_image(state, index) != ESP_OK) {
            continue;
        }
        for (uint32_t y = 0; y < THUMB_SIZE; ++y) {
            const uint32_t source_y = y * PHOTO_HEIGHT / THUMB_SIZE;
            for (uint32_t x = 0; x < THUMB_SIZE; ++x) {
                const uint32_t source_x = x * PHOTO_WIDTH / THUMB_SIZE;
                state->thumbnail_pixels[slot][y * THUMB_SIZE + x] =
                    state->gallery_pixels[source_y * PHOTO_WIDTH + source_x];
            }
        }
        /*
         * GSP borrows this buffer through DMA. Write the CPU-side thumbnail
         * back to memory before publishing it.
         */
        if (esp_cache_msync(state->thumbnail_pixels[slot], THUMB_RGB_BYTES,
                            ESP_CACHE_MSYNC_FLAG_DIR_C2M) != ESP_OK) {
            continue;
        }
        ready_mask |= 1U << slot;
    }

    if (ready_mask == 0) {
        return;
    }
    thumbnail_publish_context_t *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        ESP_LOGW(TAG, "allocate thumbnail publisher failed");
        return;
    }
    ctx->state = state;
    ctx->generation = ++state->thumbnail_generation;
    ctx->ready_mask = ready_mask;
    state->thumbnail_workers++;
    if (xTaskCreate(thumbnail_publish_task, "plant_thumbs", THUMB_TASK_STACK,
                    ctx, THUMB_TASK_PRIORITY, NULL) != pdPASS) {
        state->thumbnail_workers--;
        free(ctx);
        ESP_LOGW(TAG, "start thumbnail publisher failed");
    }
}

static void open_gallery_detail(plant_ui_t *state, uint32_t index)
{
    if (index >= state->gallery_count) {
        return;
    }
    state->gallery_index = index;
    state->gallery_detail = true;
    set_delete_dialog(state, false);
    note(state, esp_gsp_set_visible(
                    state->ui, GSP_PLANT_BIND_GALLERY_GRID_VISIBLE, false));
    note(state, esp_gsp_set_visible(
                    state->ui, GSP_PLANT_BIND_GALLERY_DETAIL_VISIBLE, true));
    show_gallery_detail(state);
}

static void show_volume(plant_ui_t *state)
{
    char value[12];
    snprintf(value, sizeof(value), "%u", state->volume);
    text(state, GSP_PLANT_BIND_VOLUME_VALUE, value);
#ifdef GSP_PLANT_BIND_VOLUME_LEVEL
    note(state, esp_gsp_set_value(state->ui, GSP_PLANT_BIND_VOLUME_LEVEL,
                                  state->volume));
#endif
}

static void poll_volume(esp_gsp_handle_t ui, void *ctx)
{
    plant_ui_t *state = ctx;
    int32_t value = (int32_t)state->volume;
    (void)ui;
#ifdef GSP_PLANT_BIND_VOLUME_LEVEL
    if (esp_gsp_get_value(state->ui, GSP_PLANT_BIND_VOLUME_LEVEL, &value) ==
            ESP_GSP_OK &&
        (unsigned)value != state->volume) {
        if (value < 0) {
            value = 0;
        } else if (value > 100) {
            value = 100;
        }
        state->volume = (unsigned)value;
        (void)voice_tts_set_volume((int)state->volume);
        char label[12];
        snprintf(label, sizeof(label), "%u", state->volume);
        text(state, GSP_PLANT_BIND_VOLUME_VALUE, label);
    }
#else
    (void)value;
#endif
}

static void show_identify_error(plant_ui_t *state, const char *message)
{
    state->identifying = false;
    text(state, GSP_PLANT_BIND_RESULT_NAME, "未识别");
    text(state, GSP_PLANT_BIND_RESULT_CONFIDENCE, " ");
    text(state, GSP_PLANT_BIND_RESULT_LATIN, " ");
    text(state, GSP_PLANT_BIND_RESULT_SUMMARY, message);
    text(state, GSP_PLANT_BIND_RESULT_POEM, " ");
    text(state, GSP_PLANT_BIND_RESULT_STATUS, "本次照片未保存");
    state->saved = false;
}

static void identify_task(void *ctx)
{
    plant_ui_t *state = ctx;
    uint8_t *jpeg = NULL;
    size_t jpeg_size = 0;
    uint32_t width = 0;
    uint32_t height = 0;

    esp_err_t err = plant_camera_capture_jpeg(
        &jpeg, &jpeg_size, &width, &height);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "capture failed: %s", esp_err_to_name(err));
        state->identifying = false;
        note(state, esp_gsp_set_visible(
                        state->ui, GSP_PLANT_BIND_PREVIEW_NOTE, true));
        text(state, GSP_PLANT_BIND_PREVIEW_NOTE, "拍照失败，请重试");
        state->identify_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    free(state->capture_jpeg);
    state->capture_jpeg = jpeg;
    state->capture_jpeg_size = jpeg_size;

    show(state, PAGE_RESULT);
    /*
     * show() asks the preview task to exit. That task restores the preview
     * placeholder on its way out. Wait until that restore has finished, then
     * publish the captured frame. capture_pixels stays unchanged here.
     */
    if (plant_camera_wait_preview_stopped(500) != ESP_OK) {
        ESP_LOGW(TAG, "preview canvas still stopping");
    }
    esp_err_t shown = ESP_FAIL;
    for (int attempt = 0; attempt < 10; ++attempt) {
        shown = plant_camera_push_snapshot(
            state->ui, GSP_PLANT_BIND_RESULT_PHOTO_SURFACE);
        if (shown == ESP_OK) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (shown != ESP_OK) {
        ESP_LOGW(TAG, "show captured photo failed");
    }
    text(state, GSP_PLANT_BIND_RESULT_NAME, "正在翻阅自然图鉴");
    text(state, GSP_PLANT_BIND_RESULT_CONFIDENCE, " ");
    text(state, GSP_PLANT_BIND_RESULT_LATIN, " ");
    text(state, GSP_PLANT_BIND_RESULT_SUMMARY,
         "正在观察叶片、花瓣与生长形态");
    text(state, GSP_PLANT_BIND_RESULT_POEM, " ");
    text(state, GSP_PLANT_BIND_RESULT_STATUS, "照片默认不保存");

    memset(&state->result, 0, sizeof(state->result));
    err = plant_cloud_identify(
        state->capture_jpeg, state->capture_jpeg_size, &state->result);
    if (state->page != PAGE_RESULT) {
        state->identifying = false;
        state->identify_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "recognition failed: %s", esp_err_to_name(err));
        show_identify_error(
            state,
            err == ESP_ERR_INVALID_STATE
                ? "请先配置千问 API Key。"
                : "云端识别暂时不可用，请检查网络后重试。");
    } else {
        char confidence[32];
        char latin[176];
        char poem[320];
        snprintf(confidence, sizeof(confidence), "匹配度 %.0f%%",
                 state->result.confidence * 100.0f);
        snprintf(latin, sizeof(latin), "%.63s%s%.95s",
                 state->result.family,
                 state->result.family[0] &&
                         state->result.scientific_name[0]
                     ? " · "
                     : "",
                 state->result.scientific_name);
        snprintf(poem, sizeof(poem), "%.191s%s%.95s",
                 state->result.poem,
                 state->result.poem[0] &&
                         state->result.poem_source[0]
                     ? "\n— "
                     : "",
                 state->result.poem_source);
        text(state, GSP_PLANT_BIND_RESULT_NAME,
             state->result.common_name[0] ? state->result.common_name
                                          : "识别结果");
        text(state, GSP_PLANT_BIND_RESULT_CONFIDENCE, confidence);
        text(state, GSP_PLANT_BIND_RESULT_LATIN, latin);
        text(state, GSP_PLANT_BIND_RESULT_SUMMARY,
             state->result.summary[0] ? state->result.summary
                                      : "暂无详细科普信息。");
        text(state, GSP_PLANT_BIND_RESULT_POEM, poem);
        text(state, GSP_PLANT_BIND_RESULT_STATUS, "照片默认不保存");
        state->identifying = false;
    }
    state->identify_task = NULL;
    vTaskDelete(NULL);
}

static void start_identify(plant_ui_t *state)
{
    if (state->identifying || state->identify_task) {
        return;
    }
    state->identifying = true;
    state->saved = false;
    note(state, esp_gsp_set_visible(
                    state->ui, GSP_PLANT_BIND_PREVIEW_NOTE, true));
    text(state, GSP_PLANT_BIND_PREVIEW_NOTE, "正在拍照…");
    TaskHandle_t task = NULL;
    if (xTaskCreate(identify_task, "plant_identify", 12288, state, 5,
                    &task) != pdPASS) {
        state->identifying = false;
        text(state, GSP_PLANT_BIND_PREVIEW_NOTE, "识别任务启动失败");
        return;
    }
    state->identify_task = task;
}

static int thumb_slot_at(int32_t x, int32_t y)
{
    const int32_t origins_x[] = {
        THUMB_ORIGIN_X0, THUMB_ORIGIN_X1, THUMB_ORIGIN_X2,
    };
    const int32_t origins_y[] = {THUMB_ORIGIN_Y0, THUMB_ORIGIN_Y1};

    for (int32_t row = 0; row < 2; ++row) {
        for (int32_t col = 0; col < 3; ++col) {
            const int32_t left = origins_x[col];
            const int32_t top = origins_y[row];
            if (x >= left && x < left + (int32_t)THUMB_SIZE &&
                y >= top && y < top + (int32_t)THUMB_SIZE) {
                return (int)(row * 3 + col);
            }
        }
    }
    return -1;
}

static void step_gallery_page(plant_ui_t *state, int direction)
{
    const uint32_t pages = gallery_page_count(state);

    if (pages <= 1U) {
        return;
    }
    if (direction > 0) {
        if (state->gallery_page + 1U >= pages) {
            return;
        }
        state->gallery_page++;
    } else if (state->gallery_page == 0) {
        return;
    } else {
        state->gallery_page--;
    }
    show_gallery_grid(state);
}

static void refresh_gallery_thumbs(esp_gsp_handle_t ui, void *ctx)
{
    plant_ui_t *state = ctx;

    (void)ui;
    if (!state->gallery_thumbs_stale || state->page != PAGE_GALLERY ||
        state->gallery_detail) {
        if (state->gallery_timer) {
            note(state, esp_gsp_timer_delete(state->ui, state->gallery_timer));
            state->gallery_timer = NULL;
        }
        return;
    }
    if (state->thumbnail_workers != 0) {
        return;
    }
    if (state->gallery_timer) {
        note(state, esp_gsp_timer_delete(state->ui, state->gallery_timer));
        state->gallery_timer = NULL;
    }
    show_gallery_grid(state);
}

static void step_gallery(plant_ui_t *state, int direction)
{
    if (state->gallery_count == 0) {
        show_gallery_grid(state);
        return;
    }
    if (direction > 0) {
        state->gallery_index = (state->gallery_index + 1) % state->gallery_count;
    } else {
        state->gallery_index =
            (state->gallery_index + state->gallery_count - 1) %
            state->gallery_count;
    }
    set_delete_dialog(state, false);
    show_gallery_detail(state);
}

static gsp_err_t network_row(esp_gsp_handle_t ui, esp_gsp_row_t row, uint32_t index,
                             void *ctx)
{
    plant_ui_t *state = ctx;
    if (!state || index >= state->network_total || index >= PLANT_WIFI_MAX_APS) {
        return GSP_ERR_INVALID_ARG;
    }
    const uint32_t lock = state->aps[index].secure ? COLOR_LOCK : COLOR_ROW;
    esp_gsp_err_t err = gsp_plant_networks_row_row_set_networks_title_text(
        ui, row, state->aps[index].ssid);
    if (err == ESP_GSP_OK) {
        err = gsp_plant_networks_row_row_set_lock_shackle_color(ui, row, lock);
    }
    if (err == ESP_GSP_OK) {
        err = gsp_plant_networks_row_row_set_lock_body_color(ui, row, lock);
    }
    return err == ESP_GSP_OK ? GSP_OK : GSP_ERR_INVALID_STATE;
}

static void stop_wifi_timer(plant_ui_t *state)
{
    if (state->wifi_timer) {
        note(state, esp_gsp_timer_delete(state->ui, state->wifi_timer));
        state->wifi_timer = NULL;
    }
    state->wifi_scanning = false;
}

static void apply_scan_results(plant_ui_t *state, size_t count)
{
    state->network_total = (unsigned)count;
    note(state, esp_gsp_list_set_total(state->ui, state->networks, (uint32_t)count));
    note(state, esp_gsp_list_refresh(state->ui, state->networks));
    if (count == 0) {
        text(state, GSP_PLANT_BIND_WIFI_STATUS, "未发现网络，可重新扫描");
    } else {
        char status[48];
        snprintf(status, sizeof(status), "发现 %u 个网络，点选连接",
                 (unsigned)count);
        text(state, GSP_PLANT_BIND_WIFI_STATUS, status);
    }
}

#ifdef ESP_PLATFORM
static void poll_scan(esp_gsp_handle_t ui, void *ctx)
{
    plant_ui_t *state = ctx;
    (void)ui;
    if (!state->wifi_scanning) {
        return;
    }
    state->wifi_wait_ms += SCAN_POLL_MS;
    if (plant_wifi_is_scanning() && state->wifi_wait_ms < SCAN_TIMEOUT_MS) {
        return;
    }
    if (plant_wifi_is_scanning()) {
        plant_wifi_abort_scan();
        stop_wifi_timer(state);
        state->wifi_wait_ms = 0;
        text(state, GSP_PLANT_BIND_WIFI_STATUS, "扫描超时，请重试");
        return;
    }
    stop_wifi_timer(state);
    state->wifi_wait_ms = 0;
    apply_scan_results(state,
                       plant_wifi_copy_results(state->aps, PLANT_WIFI_MAX_APS));
}

static void poll_connect(esp_gsp_handle_t ui, void *ctx)
{
    plant_ui_t *state = ctx;
    const plant_wifi_conn_state_t conn = plant_wifi_conn_state();
    (void)ui;
    if (!state->wifi_connecting) {
        return;
    }
    state->wifi_wait_ms += CONNECT_POLL_MS;
    if (conn == PLANT_WIFI_CONN_CONNECTING &&
        state->wifi_wait_ms < CONNECT_TIMEOUT_MS) {
        return;
    }
    stop_wifi_timer(state);
    state->wifi_connecting = false;
    state->wifi_wait_ms = 0;
    if (conn == PLANT_WIFI_CONN_CONNECTED) {
        const plant_wifi_ap_t *ap = selected_ap(state);
        if (ap) {
            const size_t length =
                strnlen(ap->ssid, sizeof(state->ssid) - 1U);
            memmove(state->ssid, ap->ssid, length);
            state->ssid[length] = '\0';
        } else {
            (void)plant_wifi_current_ssid(state->ssid, sizeof(state->ssid));
        }
        text(state, GSP_PLANT_BIND_WIFI_STATUS, "连接成功");
        show(state, PAGE_HOME);
        show_wifi_label(state);
        return;
    }
    text(state,
         state->page == PAGE_PASSWORD ? GSP_PLANT_BIND_PASSWORD_STATUS
                                      : GSP_PLANT_BIND_WIFI_STATUS,
         "连接失败，请检查密码后重试");
    show_wifi_label(state);
}
#else
static void finish_mock_scan(esp_gsp_handle_t ui, void *ctx)
{
    plant_ui_t *state = ctx;
    (void)ui;
    stop_wifi_timer(state);
    apply_scan_results(state, MOCK_NETWORK_COUNT);
}

static void finish_mock_connect(esp_gsp_handle_t ui, void *ctx)
{
    plant_ui_t *state = ctx;
    const plant_wifi_ap_t *ap = selected_ap(state);
    (void)ui;
    stop_wifi_timer(state);
    state->wifi_connecting = false;
    snprintf(state->ssid, sizeof(state->ssid), "%s", ap ? ap->ssid : "");
    text(state, GSP_PLANT_BIND_WIFI_STATUS, "连接成功");
    show(state, PAGE_HOME);
    show_wifi_label(state);
}
#endif

static void start_scan(plant_ui_t *state)
{
    if (state->wifi_timer || state->wifi_connecting) {
        return;
    }
    state->network_total = 0;
    state->wifi_wait_ms = 0;
    memset(state->aps, 0, sizeof(state->aps));
    note(state, esp_gsp_list_set_total(state->ui, state->networks, 0));
    note(state, esp_gsp_list_refresh(state->ui, state->networks));
    text(state, GSP_PLANT_BIND_WIFI_STATUS, "正在寻找附近网络…");

#ifdef ESP_PLATFORM
    if (plant_wifi_ensure_started() != ESP_OK ||
        plant_wifi_request_scan() != ESP_OK) {
        text(state, GSP_PLANT_BIND_WIFI_STATUS, "Wi-Fi 扫描启动失败");
        return;
    }
    state->wifi_scanning = true;
    state->wifi_timer =
        esp_gsp_timer_create(state->ui, SCAN_POLL_MS, poll_scan, state);
    if (!state->wifi_timer) {
        state->wifi_scanning = false;
        note(state, ESP_GSP_ERR_NO_MEM);
    }
#else
    for (unsigned i = 0; i < MOCK_NETWORK_COUNT; ++i) {
        strlcpy(state->aps[i].ssid, k_mock_networks[i].ssid,
                sizeof(state->aps[i].ssid));
        state->aps[i].secure = k_mock_networks[i].secure;
        state->aps[i].rssi = (int8_t)(-40 - (int)i * 8);
    }
    state->wifi_timer =
        esp_gsp_timer_create(state->ui, 600, finish_mock_scan, state);
    if (!state->wifi_timer) {
        note(state, ESP_GSP_ERR_NO_MEM);
    }
#endif
}

static void start_connect(plant_ui_t *state)
{
    const plant_wifi_ap_t *ap = selected_ap(state);
    char password[PLANT_WIFI_PASS_MAX + 1] = {0};

    if (state->wifi_connecting || !ap) {
        return;
    }
    stop_wifi_timer(state);
    state->wifi_connecting = true;
    state->wifi_wait_ms = 0;
    show_wifi_label(state);
    text(state,
         ap->secure ? GSP_PLANT_BIND_PASSWORD_STATUS : GSP_PLANT_BIND_WIFI_STATUS,
         "正在连接…");

#ifdef ESP_PLATFORM
    if (ap->secure) {
        note(state, esp_gsp_keyboard_text(state->ui, password, sizeof(password)));
    }
    if (plant_wifi_connect(ap->ssid, password) != ESP_OK) {
        state->wifi_connecting = false;
        show_wifi_label(state);
        text(state,
             ap->secure ? GSP_PLANT_BIND_PASSWORD_STATUS
                        : GSP_PLANT_BIND_WIFI_STATUS,
             "连接请求失败");
        return;
    }
    state->wifi_timer =
        esp_gsp_timer_create(state->ui, CONNECT_POLL_MS, poll_connect, state);
#else
    (void)password;
    state->wifi_timer =
        esp_gsp_timer_create(state->ui, CONNECT_MS, finish_mock_connect, state);
#endif
    if (!state->wifi_timer) {
        state->wifi_connecting = false;
        show_wifi_label(state);
        note(state, ESP_GSP_ERR_NO_MEM);
    }
}

static void open_password(plant_ui_t *state)
{
    char title[48];
    const plant_wifi_ap_t *ap = selected_ap(state);
    snprintf(title, sizeof(title), "连接 %s", ap ? ap->ssid : "");
    note(state, esp_gsp_keyboard_attach(state->ui, ESP_GSP_KEYBOARD_NONE,
                                        GSP_PLANT_BIND_PASSWORD_INPUT));
    note(state, esp_gsp_keyboard_attach(state->ui,
                                        GSP_PLANT_ACT_ID_PASSWORD_KEYBOARD_KEY,
                                        GSP_PLANT_BIND_PASSWORD_INPUT));
    text(state, GSP_PLANT_BIND_PASSWORD_INPUT, "");
    text(state, GSP_PLANT_BIND_PASSWORD_TITLE, title);
    text(state, GSP_PLANT_BIND_PASSWORD_STATUS, "输入密码后点 OK 或连接");
    show(state, PAGE_PASSWORD);
}

static void go_back(plant_ui_t *state)
{
    switch (state->page) {
    case PAGE_CAMERA:
        show(state, PAGE_HOME);
        show_wifi_label(state);
        break;
    case PAGE_WIFI:
        if (state->wifi_connecting) {
            return;
        }
        stop_wifi_timer(state);
        show(state, PAGE_HOME);
        show_wifi_label(state);
        break;
    case PAGE_PASSWORD:
        if (!state->wifi_connecting) {
            show(state, PAGE_WIFI);
        }
        break;
    case PAGE_RESULT:
        if (state->timer) {
            note(state, esp_gsp_timer_delete(state->ui, state->timer));
            state->timer = NULL;
        }
        state->identifying = false;
        show(state, PAGE_CAMERA);
        break;
    case PAGE_GALLERY:
        if (state->gallery_detail) {
            if (state->delete_armed) {
                set_delete_dialog(state, false);
            } else {
                show_gallery_grid(state);
            }
        } else {
            show(state, PAGE_CAMERA);
        }
        break;
    default:
        break;
    }
}

static void on_pointer(esp_gsp_handle_t ui, int32_t x, int32_t y, bool pressed,
                       void *ctx)
{
    plant_ui_t *state = ctx;
    int32_t dx;
    int32_t dy;
    (void)ui;
    if (pressed) {
        if (!state->gesture_tracking) {
            state->gesture_tracking = true;
            state->gesture_x = (int16_t)x;
            state->gesture_y = (int16_t)y;
            state->gesture_from_left = x < EDGE_ZONE_PX;
            state->gesture_from_edge =
                state->gesture_from_left || x >= 480 - EDGE_ZONE_PX;
        }
        return;
    }
    if (!state->gesture_tracking) {
        return;
    }
    state->gesture_tracking = false;
    dx = x - state->gesture_x;
    dy = y - state->gesture_y;
    if (state->delete_armed && state->page == PAGE_GALLERY &&
        state->gallery_detail && !state->gesture_from_edge) {
        if (abs((int)dx) <= THUMB_TAP_SLOP_PX &&
            abs((int)dy) <= THUMB_TAP_SLOP_PX &&
            (state->gesture_x < 40 || state->gesture_x >= 440 ||
             state->gesture_y < 140 || state->gesture_y >= 340)) {
            set_delete_dialog(state, false);
        }
        return;
    }
    if (state->gesture_from_edge) {
        const int32_t inward = state->gesture_from_left ? dx : -dx;
        if (inward >= BACK_SWIPE_DISTANCE_PX &&
            abs((int)dy) <= BACK_SWIPE_MAX_Y_PX && inward > abs((int)dy)) {
            go_back(state);
        }
        return;
    }
    if (state->page == PAGE_GALLERY && !state->gallery_detail &&
        abs((int)dy) >= GALLERY_SWIPE_PX && abs((int)dy) > abs((int)dx)) {
        /* Finger moving up reveals the older page below this one. */
        ESP_LOGI(TAG, "gallery page swipe dy=%ld workers=%u page=%u",
                 (long)dy, (unsigned)state->thumbnail_workers,
                 state->gallery_page);
        step_gallery_page(state, dy < 0 ? 1 : -1);
        return;
    }
    if (state->page == PAGE_GALLERY && !state->gallery_detail &&
        abs((int)dx) <= THUMB_TAP_SLOP_PX && abs((int)dy) <= THUMB_TAP_SLOP_PX) {
        const int slot = thumb_slot_at(state->gesture_x, state->gesture_y);
        const uint32_t offset =
            slot < 0 ? state->gallery_count
                     : state->gallery_page * THUMB_COUNT + (uint32_t)slot;
        if (slot >= 0 && offset < state->gallery_count) {
            open_gallery_detail(state, state->thumbnail_indices[slot]);
        }
        return;
    }
    if (state->page == PAGE_GALLERY && state->gallery_detail &&
        abs((int)dx) >= GALLERY_SWIPE_PX &&
        abs((int)dx) > abs((int)dy)) {
        step_gallery(state, dx < 0 ? 1 : -1);
    }
}

static void on_event(esp_gsp_handle_t ui, const esp_gsp_event_t *event, void *ctx)
{
    plant_ui_t *state = ctx;
    (void)ui;
    if (!event || event->type != ESP_GSP_EVENT_CALL) {
        return;
    }
    switch (event->action_id) {
    case GSP_PLANT_ACT_ID_OPEN_WIFI:
        show(state, PAGE_WIFI);
        show_wifi_label(state);
        start_scan(state);
        break;
    case GSP_PLANT_ACT_ID_RESCAN:
        if (!state->wifi_connecting) {
            start_scan(state);
        }
        break;
    case GSP_PLANT_ACT_ID_PASSWORD_BACK:
        if (!state->wifi_connecting) {
            show(state, PAGE_WIFI);
        }
        break;
    case GSP_PLANT_ACT_ID_CONNECT:
    case GSP_PLANT_ACT_ID_PASSWORD_KEYBOARD_KEY:
        if (state->page == PAGE_PASSWORD && !state->wifi_connecting) {
            start_connect(state);
        }
        break;
    case GSP_PLANT_ACT_ID_START_SCAN:
        show(state, PAGE_CAMERA);
        break;
    case GSP_PLANT_ACT_ID_BACK_HOME:
        if (state->wifi_connecting) {
            break;
        }
        stop_wifi_timer(state);
        show(state, PAGE_HOME);
        show_wifi_label(state);
        break;
    case GSP_PLANT_ACT_ID_TOGGLE_FLASH:
        state->flash_on = !state->flash_on;
        plant_camera_set_flash_enabled(state->flash_on);
        note(state, esp_gsp_set_visible(state->ui, GSP_PLANT_BIND_PREVIEW_NOTE,
                                        true));
        text(state, GSP_PLANT_BIND_PREVIEW_NOTE,
             "保持光线充足，主体清晰完整");
        break;
    case GSP_PLANT_ACT_ID_TOGGLE_ROTATE:
        state->rotation_degrees = plant_camera_rotate_clockwise();
        (void)camera_settings_save_rotation(state->rotation_degrees);
        show_camera_labels(state);
        break;
    case GSP_PLANT_ACT_ID_CAPTURE:
        start_identify(state);
        break;
    case GSP_PLANT_ACT_ID_OPEN_GALLERY:
        state->gallery_page = 0;
        show(state, PAGE_GALLERY);
        show_gallery_grid(state);
        break;
    case GSP_PLANT_ACT_ID_SPEAK:
        if (!state->identifying && state->result.common_name[0]) {
            char narration[768];
            snprintf(narration, sizeof(narration), "%s。%s。%s。%s",
                     state->result.common_name, state->result.summary,
                     state->result.poem, state->result.poem_source);
            const esp_err_t speak = voice_tts_speak_async(narration);
            text(state, GSP_PLANT_BIND_RESULT_STATUS,
                 speak == ESP_OK ? "正在语音讲解"
                                 : "语音讲解启动失败");
        }
        break;
    case GSP_PLANT_ACT_ID_SAVE_PHOTO:
        if (!state->identifying && !state->saved &&
            state->capture_jpeg && state->capture_jpeg_size > 0) {
            if (!state->storage_available) {
                text(state, GSP_PLANT_BIND_RESULT_STATUS,
                     "板载 NAND 不可用");
                break;
            }
            photo_store_info_t saved = {0};
            const esp_err_t save = photo_store_save_jpeg(
                state->capture_jpeg, state->capture_jpeg_size, &saved);
            if (save == ESP_OK) {
                state->saved = true;
                state->gallery_count = photo_store_get_count();
                text(state, GSP_PLANT_BIND_RESULT_STATUS,
                     "已保存到相册");
            } else {
                text(state, GSP_PLANT_BIND_RESULT_STATUS,
                     save == ESP_ERR_NO_MEM ? "存储空间不足"
                                            : "保存失败，请重试");
            }
        }
        break;
    case GSP_PLANT_ACT_ID_GALLERY_DELETE:
        if (state->page == PAGE_GALLERY && state->gallery_detail &&
            state->gallery_count > 0 &&
            state->gallery_index < state->gallery_count) {
            set_delete_dialog(state, true);
        }
        break;
    case GSP_PLANT_ACT_ID_GALLERY_DELETE_CANCEL:
        set_delete_dialog(state, false);
        break;
    case GSP_PLANT_ACT_ID_GALLERY_DELETE_CONFIRM:
        if (state->delete_armed) {
            confirm_gallery_delete(state);
        }
        break;
    case GSP_PLANT_ACT_ID_SELECT_NETWORKS:
        if (state->page != PAGE_WIFI || state->wifi_connecting ||
            event->list != state->networks ||
            event->item >= state->network_total) {
            break;
        }
        state->selected_network = event->item;
        if (state->aps[event->item].secure) {
            open_password(state);
        } else {
            start_connect(state);
        }
        break;
    default:
        break;
    }
}

esp_gsp_err_t plant_ui_init(esp_gsp_handle_t ui, plant_ui_t *state)
{
    if (!ui || !state) {
        return ESP_GSP_ERR_INVALID_ARG;
    }
    memset(state, 0, sizeof(*state));
    state->ui = ui;
    state->volume = 60;
    (void)camera_settings_init();
    if (camera_settings_load_rotation(&state->rotation_degrees) != ESP_OK) {
        state->rotation_degrees = 180;
    }
    (void)plant_camera_set_rotation(state->rotation_degrees);
    state->storage_available = photo_store_init() == ESP_OK;
    const esp_err_t tts = voice_tts_init();
    if (tts == ESP_OK) {
        state->volume = (unsigned)voice_tts_get_volume();
    } else {
        ESP_LOGW(TAG, "TTS unavailable: %s", esp_err_to_name(tts));
    }
    state->networks = esp_gsp_list_bind_component(
        ui, GSP_PLANT_OBJ_KEY_NETWORKS, network_row, state);
    if (state->networks == ESP_GSP_LIST_NONE) {
        return ESP_GSP_ERR_NO_MEM;
    }
    note(state, esp_gsp_on_event(ui, on_event, state));
    {
        const esp_gsp_err_t pointer =
            esp_gsp_set_pointer_observer(ui, on_pointer, state);
        if (pointer != ESP_GSP_OK && pointer != ESP_GSP_ERR_NOT_SUPPORTED) {
            note(state, pointer);
        }
    }
    note(state, esp_gsp_list_set_total(ui, state->networks, 0));
#ifdef ESP_PLATFORM
    if (plant_wifi_ensure_started() == ESP_OK) {
        if (plant_wifi_current_ssid(state->ssid, sizeof(state->ssid)) !=
            ESP_OK &&
            plant_wifi_conn_state() == PLANT_WIFI_CONN_CONNECTING) {
            state->wifi_connecting = true;
            state->wifi_timer =
                esp_gsp_timer_create(ui, CONNECT_POLL_MS, poll_connect, state);
        }
    }
#endif
    show(state, PAGE_HOME);
    show_wifi_label(state);
    show_volume(state);
    state->volume_timer =
        esp_gsp_timer_create(ui, VOLUME_POLL_MS, poll_volume, state);
    if (!state->volume_timer) {
        note(state, ESP_GSP_ERR_NO_MEM);
    }
    return state->last_error;
}

void plant_ui_deinit(esp_gsp_handle_t ui, plant_ui_t *state)
{
    if (!ui || !state) {
        return;
    }
    leave_camera(state);
    if (state->timer) {
        note(state, esp_gsp_timer_delete(ui, state->timer));
        state->timer = NULL;
    }
    if (state->volume_timer) {
        note(state, esp_gsp_timer_delete(ui, state->volume_timer));
        state->volume_timer = NULL;
    }
    if (state->gallery_timer) {
        note(state, esp_gsp_timer_delete(ui, state->gallery_timer));
        state->gallery_timer = NULL;
    }
    stop_wifi_timer(state);
    voice_tts_stop();
    ++state->thumbnail_generation;
    while (state->thumbnail_workers != 0) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    free(state->capture_jpeg);
    free(state->gallery_jpeg);
    free(state->gallery_pixels);
    for (uint32_t i = 0; i < THUMB_COUNT; ++i) {
        free(state->thumbnail_pixels[i]);
        state->thumbnail_pixels[i] = NULL;
    }
    state->capture_jpeg = NULL;
    state->gallery_jpeg = NULL;
    state->gallery_pixels = NULL;
    note(state, esp_gsp_keyboard_attach(ui, ESP_GSP_KEYBOARD_NONE, 0));
    note(state, esp_gsp_set_pointer_observer(ui, NULL, NULL));
    note(state, esp_gsp_on_event(ui, NULL, NULL));
}
