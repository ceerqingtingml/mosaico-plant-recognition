// SPDX-License-Identifier: Apache-2.0
/*
 * Live preview path aligned with
 * submodule/esp-mosaico-bsp/examples/camera_lcd_preview:
 *   UYVY center-crop -> PPA RGB565 (rotation selected for mounting) -> GSP canvas.
 */

#include "plant_camera.h"

#include <stdlib.h>
#include <string.h>

#include "driver/ppa.h"
#include "esp_cache.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_jpeg_enc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "linux/videodev2.h"
#include "mosaico_module_camera.h"

static const char *TAG = "plant_camera";

#define PREVIEW_W 480
#define PREVIEW_H 480
#define PREVIEW_BUF_COUNT 3
#define PREVIEW_BUF_ALIGN 128
#define PREVIEW_TASK_STACK 8192
#define PREVIEW_TASK_PRIO 12
#define CAMERA_BUFFER_COUNT 2
#define CAPTURE_FAILURE_LIMIT 2
#define CAMERA_RETRY_DELAY_MS 1000
#define CAPTURE_TIMEOUT_MS 2500
#define JPEG_QUALITY 80
#define MAX_JPEG_SIZE (256U * 1024U)
#define FLASH_EXPOSURE_SETTLE_FRAMES 2
#define FLASH_PIPELINE_FLUSH_FRAMES 1

typedef struct {
    uint16_t *pixels;
    bool in_flight;
} preview_slot_t;

typedef struct {
    mosaico_camera_handle_t cam;
    ppa_client_handle_t ppa;
    esp_gsp_handle_t ui;
    uint16_t bind;
    TaskHandle_t task;
    SemaphoreHandle_t lock;
    SemaphoreHandle_t capture_done;
    preview_slot_t slots[PREVIEW_BUF_COUNT];
    uint16_t *capture_pixels;
    uint8_t *capture_jpeg;
    size_t slot_bytes;
    size_t capture_jpeg_size;
    esp_err_t capture_err;
    uint32_t push_ok;
    uint32_t push_fail;
    uint32_t frames_in;
    uint32_t ppa_fail;
    uint32_t drop_no_slot;
    uint8_t rotation_quarters;
    bool flash_enabled;
    bool running;
    bool opened;
    bool logged_format;
    bool capture_requested;
} plant_camera_t;

static plant_camera_t s_cam;

static size_t align_up(size_t value, size_t alignment)
{
    return (value + alignment - 1U) & ~(alignment - 1U);
}

static uint32_t align_down_even(uint32_t value)
{
    return value & ~1U;
}

static void slot_released(void *ctx)
{
    preview_slot_t *slot = ctx;
    if (!slot || !s_cam.lock) {
        return;
    }
    xSemaphoreTake(s_cam.lock, portMAX_DELAY);
    slot->in_flight = false;
    xSemaphoreGive(s_cam.lock);
}

static esp_err_t ensure_ppa(void)
{
    if (s_cam.ppa) {
        return ESP_OK;
    }
    const ppa_client_config_t ppa_config = {
        .oper_type = PPA_OPERATION_SRM,
        .max_pending_trans_num = 1,
    };
    return ppa_register_client(&ppa_config, &s_cam.ppa);
}

/* Same convert as camera_lcd_preview: center square crop + PPA SRM. */
static esp_err_t convert_uyvy_ppa(const mosaico_camera_frame_t *frame,
                                  uint16_t *dst)
{
    ESP_RETURN_ON_FALSE(s_cam.ppa && frame && frame->data && dst,
                        ESP_ERR_INVALID_ARG, TAG, "ppa args");
    ESP_RETURN_ON_FALSE(
        frame->pixel_format == V4L2_PIX_FMT_UYVY ||
            frame->pixel_format == MOSAICO_CAMERA_PIXEL_FORMAT_UYVY,
        ESP_ERR_NOT_SUPPORTED, TAG, "need UYVY");

    const uint32_t crop_size =
        align_down_even(frame->width < frame->height ? frame->width
                                                     : frame->height);
    ESP_RETURN_ON_FALSE(crop_size > 0, ESP_ERR_INVALID_SIZE, TAG, "crop");

    const uint32_t bytes_per_line =
        frame->bytes_per_line ? frame->bytes_per_line : frame->width * 2U;
    ESP_RETURN_ON_FALSE((bytes_per_line % 2U) == 0, ESP_ERR_INVALID_SIZE, TAG,
                        "stride");

    ESP_RETURN_ON_ERROR(
        esp_cache_msync((void *)frame->data, frame->size,
                        ESP_CACHE_MSYNC_FLAG_DIR_M2C |
                            ESP_CACHE_MSYNC_FLAG_INVALIDATE),
        TAG, "invalidate camera frame");
    ESP_RETURN_ON_ERROR(
        esp_cache_msync(dst, s_cam.slot_bytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M),
        TAG, "clean preview slot");

    xSemaphoreTake(s_cam.lock, portMAX_DELAY);
    const uint8_t rotation = s_cam.rotation_quarters;
    xSemaphoreGive(s_cam.lock);
    static const ppa_srm_rotation_angle_t angles[] = {
        PPA_SRM_ROTATION_ANGLE_270,
        PPA_SRM_ROTATION_ANGLE_0,
        PPA_SRM_ROTATION_ANGLE_90,
        PPA_SRM_ROTATION_ANGLE_180,
    };
    const ppa_srm_oper_config_t operation = {
        .in =
            {
                .buffer = frame->data,
                .pic_w = bytes_per_line / 2U,
                .pic_h = frame->height,
                .block_w = crop_size,
                .block_h = crop_size,
                .block_offset_x =
                    align_down_even((frame->width - crop_size) / 2U),
                .block_offset_y =
                    align_down_even((frame->height - crop_size) / 2U),
                .srm_cm = PPA_SRM_COLOR_MODE_YUV422_UYVY,
                .yuv_range = PPA_COLOR_RANGE_LIMIT,
                .yuv_std = PPA_COLOR_CONV_STD_RGB_YUV_BT601,
            },
        .out =
            {
                .buffer = dst,
                .buffer_size = (uint32_t)s_cam.slot_bytes,
                .pic_w = PREVIEW_W,
                .pic_h = PREVIEW_H,
                .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
            },
        /* Correct the LEFT-slot rotation without mirroring the rear camera. */
        .rotation_angle = angles[rotation & 3U],
        .scale_x = (float)PREVIEW_W / (float)crop_size,
        .scale_y = (float)PREVIEW_H / (float)crop_size,
        .mirror_y = false,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };

    ESP_RETURN_ON_ERROR(ppa_do_scale_rotate_mirror(s_cam.ppa, &operation), TAG,
                        "ppa convert");
    /* PPA wrote through DMA. Invalidate CPU cache before GSP/JPEG reads it. */
    ESP_RETURN_ON_ERROR(
        esp_cache_msync(dst, s_cam.slot_bytes,
                        ESP_CACHE_MSYNC_FLAG_DIR_M2C |
                            ESP_CACHE_MSYNC_FLAG_INVALIDATE),
        TAG, "invalidate PPA output");
    return ESP_OK;
}

static esp_err_t encode_rgb565_jpeg(const uint16_t *pixels, size_t *out_size)
{
    ESP_RETURN_ON_FALSE(pixels && out_size && s_cam.capture_jpeg,
                        ESP_ERR_INVALID_ARG, TAG, "jpeg args");

    jpeg_enc_config_t config = DEFAULT_JPEG_ENC_CONFIG();
    config.width = PREVIEW_W;
    config.height = PREVIEW_H;
    config.src_type = JPEG_PIXEL_FORMAT_RGB565_LE;
    config.subsampling = JPEG_SUBSAMPLE_420;
    config.quality = JPEG_QUALITY;

    jpeg_enc_handle_t encoder = NULL;
    jpeg_error_t ret = jpeg_enc_open(&config, &encoder);
    ESP_RETURN_ON_FALSE(ret == JPEG_ERR_OK, ESP_FAIL, TAG,
                        "jpeg open: %d", ret);

    int encoded = 0;
    ret = jpeg_enc_process(
        encoder, (uint8_t *)pixels,
        PREVIEW_W * PREVIEW_H * (int)sizeof(uint16_t),
        s_cam.capture_jpeg, MAX_JPEG_SIZE, &encoded);
    jpeg_enc_close(encoder);
    ESP_RETURN_ON_FALSE(ret == JPEG_ERR_OK && encoded > 0, ESP_FAIL, TAG,
                        "jpeg encode: %d", ret);
    *out_size = (size_t)encoded;
    return ESP_OK;
}

static esp_err_t close_camera(void)
{
    if (!s_cam.cam) {
        s_cam.opened = false;
        return ESP_OK;
    }

    /*
     * Match camera_lcd_preview: do not discard the handle unless every
     * teardown step succeeds. mosaico_camera_del() deliberately retains its
     * context when DVP/GDMA cleanup is incomplete.
     */
    esp_err_t err = mosaico_camera_stop_stream(s_cam.cam);
    if (err == ESP_OK) {
        err = mosaico_camera_close(s_cam.cam);
    }
    if (err == ESP_OK) {
        err = mosaico_camera_del(s_cam.cam);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "camera cleanup incomplete: %s", esp_err_to_name(err));
        return err;
    }

    s_cam.cam = NULL;
    s_cam.opened = false;
    return ESP_OK;
}

static esp_err_t ensure_buffers(void)
{
    s_cam.slot_bytes =
        align_up((size_t)PREVIEW_W * PREVIEW_H * sizeof(uint16_t),
                 PREVIEW_BUF_ALIGN);
    const uint32_t caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA | MALLOC_CAP_8BIT;
    for (int i = 0; i < PREVIEW_BUF_COUNT; ++i) {
        if (s_cam.slots[i].pixels) {
            continue;
        }
        s_cam.slots[i].pixels = heap_caps_aligned_calloc(
            PREVIEW_BUF_ALIGN, 1, s_cam.slot_bytes, caps);
        if (!s_cam.slots[i].pixels) {
            s_cam.slots[i].pixels = heap_caps_aligned_calloc(
                PREVIEW_BUF_ALIGN, 1, s_cam.slot_bytes,
                MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
        }
        ESP_RETURN_ON_FALSE(s_cam.slots[i].pixels, ESP_ERR_NO_MEM, TAG,
                            "preview slot");
        s_cam.slots[i].in_flight = false;
    }
    if (!s_cam.capture_pixels) {
        s_cam.capture_pixels = heap_caps_aligned_calloc(
            PREVIEW_BUF_ALIGN, 1, s_cam.slot_bytes, caps);
        ESP_RETURN_ON_FALSE(s_cam.capture_pixels, ESP_ERR_NO_MEM, TAG,
                            "capture pixels");
    }
    if (!s_cam.capture_jpeg) {
        s_cam.capture_jpeg =
            heap_caps_malloc(MAX_JPEG_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        ESP_RETURN_ON_FALSE(s_cam.capture_jpeg, ESP_ERR_NO_MEM, TAG,
                            "capture jpeg");
    }
    if (!s_cam.capture_done) {
        s_cam.capture_done = xSemaphoreCreateBinary();
        ESP_RETURN_ON_FALSE(s_cam.capture_done, ESP_ERR_NO_MEM, TAG,
                            "capture semaphore");
    }
    return ESP_OK;
}

static preview_slot_t *acquire_slot(void)
{
    xSemaphoreTake(s_cam.lock, portMAX_DELAY);
    preview_slot_t *slot = NULL;
    for (int i = 0; i < PREVIEW_BUF_COUNT; ++i) {
        if (!s_cam.slots[i].in_flight && s_cam.slots[i].pixels) {
            s_cam.slots[i].in_flight = true;
            slot = &s_cam.slots[i];
            break;
        }
    }
    xSemaphoreGive(s_cam.lock);
    return slot;
}

static esp_err_t ensure_open(void)
{
    if (s_cam.opened && s_cam.cam) {
        return ESP_OK;
    }

    mosaico_camera_config_t config = MOSAICO_CAMERA_DEFAULT_CONFIG();
    config.pixel_format = MOSAICO_CAMERA_PIXEL_FORMAT_UYVY;
    config.buffer_count = CAMERA_BUFFER_COUNT;
        config.frame_timeout_ms = 400;
    config.allow_unidentified = true;

    esp_err_t err = mosaico_camera_new(&config, &s_cam.cam);
    if (err == ESP_OK) {
        err = mosaico_camera_open(s_cam.cam);
    }
    if (err == ESP_OK) {
        err = mosaico_camera_start_stream(s_cam.cam);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "camera open failed: %s", esp_err_to_name(err));
        const esp_err_t cleanup_err = close_camera();
        return cleanup_err == ESP_OK ? err : cleanup_err;
    }

    mosaico_camera_info_t info = {0};
    if (mosaico_camera_get_info(s_cam.cam, &info) == ESP_OK) {
        ESP_LOGI(TAG, "camera open ok %ux%u fourcc=0x%08x",
                 (unsigned)info.width, (unsigned)info.height,
                 (unsigned)info.pixel_format);
    }
    s_cam.opened = true;
    s_cam.logged_format = false;
    return ESP_OK;
}

static void finish_capture_error(esp_err_t err)
{
    xSemaphoreTake(s_cam.lock, portMAX_DELAY);
    const bool pending = s_cam.capture_requested;
    s_cam.capture_requested = false;
    s_cam.capture_err = err;
    s_cam.capture_jpeg_size = 0;
    xSemaphoreGive(s_cam.lock);
    if (pending) {
        xSemaphoreGive(s_cam.capture_done);
    }
}

static void preview_task(void *arg)
{
    (void)arg;
    int64_t last_stat_us = esp_timer_get_time();
    uint32_t capture_failures = 0;

    while (true) {
        xSemaphoreTake(s_cam.lock, portMAX_DELAY);
        const bool running = s_cam.running;
        esp_gsp_handle_t ui = s_cam.ui;
        const uint16_t bind = s_cam.bind;
        mosaico_camera_handle_t cam = s_cam.cam;
        xSemaphoreGive(s_cam.lock);

        if (!running || !ui) {
            break;
        }
        if (!cam) {
            const esp_err_t open_err = ensure_open();
            if (open_err != ESP_OK) {
                ESP_LOGW(TAG, "camera unavailable, retrying: %s",
                         esp_err_to_name(open_err));
                vTaskDelay(pdMS_TO_TICKS(CAMERA_RETRY_DELAY_MS));
                continue;
            }
            xSemaphoreTake(s_cam.lock, portMAX_DELAY);
            cam = s_cam.cam;
            xSemaphoreGive(s_cam.lock);
            if (!cam) {
                vTaskDelay(pdMS_TO_TICKS(CAMERA_RETRY_DELAY_MS));
                continue;
            }
        }

        bool capture_pending = false;
        bool use_flash = false;
        xSemaphoreTake(s_cam.lock, portMAX_DELAY);
        capture_pending = s_cam.capture_requested;
        use_flash = capture_pending && s_cam.flash_enabled;
        xSemaphoreGive(s_cam.lock);

        mosaico_camera_flash_state_t flash_state = {0};
        bool flash_prepared = false;
        esp_err_t err = ESP_OK;
        if (use_flash) {
            err = mosaico_camera_prepare_flash_capture(cam, &flash_state);
            if (err == ESP_OK) {
                flash_prepared = true;
                for (uint32_t i = 0;
                     i < FLASH_EXPOSURE_SETTLE_FRAMES && err == ESP_OK; ++i) {
                    err = mosaico_camera_discard_frames(cam, 1);
                }
            }
            if (err == ESP_OK) {
                err = mosaico_camera_flash_trigger(cam);
            }
            if (err == ESP_OK) {
                err = mosaico_camera_discard_frames(
                    cam, FLASH_PIPELINE_FLUSH_FRAMES);
            }
            if (err != ESP_OK) {
                if (flash_prepared) {
                    (void)mosaico_camera_flash_stop(cam);
                    (void)mosaico_camera_restore_flash_capture(
                        cam, &flash_state);
                }
                finish_capture_error(err);
                continue;
            }
        }

        mosaico_camera_frame_t frame = {0};
        err = mosaico_camera_get_frame(cam, &frame);
        if (err != ESP_OK || frame.data == NULL || frame.size == 0 ||
            frame.width == 0 || frame.height == 0) {
            if (flash_prepared) {
                (void)mosaico_camera_flash_stop(cam);
                (void)mosaico_camera_restore_flash_capture(cam, &flash_state);
            }
            capture_failures++;
            if (capture_failures >= CAPTURE_FAILURE_LIMIT) {
                /*
                 * Restart the existing stream. Destroying and creating a new
                 * DVP controller here previously leaked its GDMA channel, after
                 * which every later STREAMON failed permanently.
                 */
                ESP_LOGW(TAG, "DQBUF failing, restart stream");
                const esp_err_t restart_err = mosaico_camera_restart(cam);
                if (restart_err != ESP_OK) {
                    ESP_LOGW(TAG, "camera restart failed: %s",
                             esp_err_to_name(restart_err));
                    finish_capture_error(restart_err);
                    vTaskDelay(pdMS_TO_TICKS(CAMERA_RETRY_DELAY_MS));
                }
                capture_failures = 0;
            }
            continue;
        }
        if (flash_prepared) {
            (void)mosaico_camera_flash_stop(cam);
        }
        capture_failures = 0;

        if (!s_cam.logged_format) {
            ESP_LOGI(TAG, "camera frame %ux%u fourcc=0x%08x stride=%u",
                     (unsigned)frame.width, (unsigned)frame.height,
                     (unsigned)frame.pixel_format,
                     (unsigned)frame.bytes_per_line);
            s_cam.logged_format = true;
        }

        preview_slot_t *slot = acquire_slot();
        if (!slot) {
            /* Do not stall the V4L2 queue; drop like a missed display tick. */
            (void)mosaico_camera_return_frame(cam, &frame);
            s_cam.drop_no_slot++;
            if (flash_prepared) {
                (void)mosaico_camera_restore_flash_capture(cam, &flash_state);
                finish_capture_error(ESP_ERR_NOT_FINISHED);
            }
            continue;
        }

        /* Official order: convert while holding the mmap buffer, then QBUF. */
        err = convert_uyvy_ppa(&frame, slot->pixels);
        const esp_err_t ret_frame = mosaico_camera_return_frame(cam, &frame);
        if (ret_frame != ESP_OK) {
            ESP_LOGW(TAG, "return frame: %s", esp_err_to_name(ret_frame));
        }

        if (err != ESP_OK) {
            /* No software fallback — that path is what made preview stutter. */
            s_cam.ppa_fail++;
            slot_released(slot);
            if (flash_prepared) {
                (void)mosaico_camera_restore_flash_capture(cam, &flash_state);
                finish_capture_error(err);
            }
            continue;
        }

        bool capture = false;
        xSemaphoreTake(s_cam.lock, portMAX_DELAY);
        if (s_cam.capture_requested) {
            s_cam.capture_requested = false;
            capture = true;
        }
        xSemaphoreGive(s_cam.lock);

        if (capture) {
            memcpy(s_cam.capture_pixels, slot->pixels, s_cam.slot_bytes);
            size_t jpeg_size = 0;
            const esp_err_t capture_err =
                encode_rgb565_jpeg(s_cam.capture_pixels, &jpeg_size);
            xSemaphoreTake(s_cam.lock, portMAX_DELAY);
            s_cam.capture_err = capture_err;
            s_cam.capture_jpeg_size =
                capture_err == ESP_OK ? jpeg_size : 0;
            xSemaphoreGive(s_cam.lock);
            xSemaphoreGive(s_cam.capture_done);
        }
        if (flash_prepared) {
            (void)mosaico_camera_restore_flash_capture(cam, &flash_state);
        }

        s_cam.frames_in++;
        const esp_gsp_err_t push =
            esp_gsp_canvas_try_push(ui, bind, slot->pixels,
                                    (size_t)PREVIEW_W * 2U, slot_released, slot);
        if (push == ESP_GSP_OK) {
            s_cam.push_ok++;
        } else {
            s_cam.push_fail++;
            slot_released(slot);
        }

        const int64_t now = esp_timer_get_time();
        if (now - last_stat_us >= 1000000) {
            ESP_LOGI(TAG,
                     "preview stats in=%u ok=%u fail=%u ppa_fail=%u drop=%u",
                     (unsigned)s_cam.frames_in, (unsigned)s_cam.push_ok,
                     (unsigned)s_cam.push_fail, (unsigned)s_cam.ppa_fail,
                     (unsigned)s_cam.drop_no_slot);
            s_cam.frames_in = 0;
            s_cam.push_ok = 0;
            s_cam.push_fail = 0;
            s_cam.ppa_fail = 0;
            s_cam.drop_no_slot = 0;
            last_stat_us = now;
        }
    }

    /*
     * Keep the camera session and DVP stream alive across UI page changes.
     * The BSP camera_photo_app follows the same model when switching between
     * camera and gallery modes. Recreating the DVP controller after every
     * result page can fail to reacquire an AXI GDMA channel, which makes the
     * second capture permanently retry STREAMON.
     *
     * With no DQBUF consumer the two queued buffers simply fill and the
     * pipeline pauses. A newly started preview task drains them and resumes
     * the existing stream without reallocating DMA resources.
     */
    xSemaphoreTake(s_cam.lock, portMAX_DELAY);
    esp_gsp_handle_t ui = s_cam.ui;
    const uint16_t bind = s_cam.bind;
    for (int i = 0; i < PREVIEW_BUF_COUNT; ++i) {
        s_cam.slots[i].in_flight = false;
    }
    s_cam.ui = NULL;
    xSemaphoreGive(s_cam.lock);
    /*
     * Restore the preview placeholder before publishing this task as gone.
     * identify_task waits for that, then pushes the captured frame to the
     * result canvas. Stopping afterwards would paint the green plate back.
     */
    if (ui) {
        (void)esp_gsp_canvas_stop(ui, bind);
    }
    xSemaphoreTake(s_cam.lock, portMAX_DELAY);
    s_cam.task = NULL;
    xSemaphoreGive(s_cam.lock);
    ESP_LOGI(TAG, "preview stopped");
    vTaskDelete(NULL);
}

esp_err_t plant_camera_start_preview(esp_gsp_handle_t ui, uint16_t preview_bind)
{
    ESP_RETURN_ON_FALSE(ui, ESP_ERR_INVALID_ARG, TAG, "ui");
    if (!s_cam.lock) {
        s_cam.lock = xSemaphoreCreateMutex();
        ESP_RETURN_ON_FALSE(s_cam.lock, ESP_ERR_NO_MEM, TAG, "mutex");
    }

    xSemaphoreTake(s_cam.lock, portMAX_DELAY);
    if (s_cam.running) {
        s_cam.ui = ui;
        s_cam.bind = preview_bind;
        xSemaphoreGive(s_cam.lock);
        return ESP_OK;
    }
    TaskHandle_t previous = s_cam.task;
    xSemaphoreGive(s_cam.lock);

    for (int i = 0; previous != NULL && i < 100; ++i) {
        vTaskDelay(pdMS_TO_TICKS(10));
        xSemaphoreTake(s_cam.lock, portMAX_DELAY);
        previous = s_cam.task;
        xSemaphoreGive(s_cam.lock);
    }
    if (previous != NULL) {
        ESP_LOGE(TAG, "previous preview still stopping");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_RETURN_ON_ERROR(ensure_buffers(), TAG, "buffers");
    ESP_RETURN_ON_ERROR(ensure_ppa(), TAG, "ppa");

    xSemaphoreTake(s_cam.lock, portMAX_DELAY);
    s_cam.ui = ui;
    s_cam.bind = preview_bind;
    s_cam.running = true;
    s_cam.push_ok = 0;
    s_cam.push_fail = 0;
    s_cam.frames_in = 0;
    s_cam.ppa_fail = 0;
    s_cam.drop_no_slot = 0;
    for (int i = 0; i < PREVIEW_BUF_COUNT; ++i) {
        s_cam.slots[i].in_flight = false;
    }
    const BaseType_t ok =
        xTaskCreate(preview_task, "plant_cam", PREVIEW_TASK_STACK, NULL,
                    PREVIEW_TASK_PRIO, &s_cam.task);
    if (ok != pdPASS) {
        s_cam.running = false;
        s_cam.task = NULL;
        xSemaphoreGive(s_cam.lock);
        (void)close_camera();
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreGive(s_cam.lock);
    ESP_LOGI(TAG, "preview started bind=%u (PPA path like camera_lcd_preview)",
             (unsigned)preview_bind);
    return ESP_OK;
}

void plant_camera_stop_preview(void)
{
    if (!s_cam.lock) {
        return;
    }
    xSemaphoreTake(s_cam.lock, portMAX_DELAY);
    s_cam.running = false;
    xSemaphoreGive(s_cam.lock);
}

bool plant_camera_preview_running(void)
{
    if (!s_cam.lock) {
        return false;
    }
    xSemaphoreTake(s_cam.lock, portMAX_DELAY);
    const bool running = s_cam.running;
    xSemaphoreGive(s_cam.lock);
    return running;
}

esp_err_t plant_camera_wait_preview_stopped(uint32_t timeout_ms)
{
    if (!s_cam.lock) {
        return ESP_OK;
    }
    const uint32_t steps = timeout_ms / 10U + 1U;
    for (uint32_t i = 0; i < steps; ++i) {
        xSemaphoreTake(s_cam.lock, portMAX_DELAY);
        const bool stopped = s_cam.task == NULL;
        xSemaphoreGive(s_cam.lock);
        if (stopped) {
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return ESP_ERR_TIMEOUT;
}

esp_err_t plant_camera_capture_jpeg(uint8_t **out_jpeg, size_t *out_size,
                                    uint32_t *out_width, uint32_t *out_height)
{
    ESP_RETURN_ON_FALSE(out_jpeg && out_size, ESP_ERR_INVALID_ARG, TAG, "args");
    *out_jpeg = NULL;
    *out_size = 0;
    if (out_width) {
        *out_width = 0;
    }
    if (out_height) {
        *out_height = 0;
    }
    ESP_RETURN_ON_FALSE(s_cam.lock && s_cam.capture_done,
                        ESP_ERR_INVALID_STATE, TAG, "camera not initialized");

    (void)xSemaphoreTake(s_cam.capture_done, 0);
    xSemaphoreTake(s_cam.lock, portMAX_DELAY);
    if (!s_cam.running || !s_cam.cam || s_cam.capture_requested) {
        xSemaphoreGive(s_cam.lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_cam.capture_err = ESP_ERR_INVALID_STATE;
    s_cam.capture_jpeg_size = 0;
    s_cam.capture_requested = true;
    xSemaphoreGive(s_cam.lock);
    ESP_LOGI(TAG, "capture requested");

    if (xSemaphoreTake(s_cam.capture_done,
                       pdMS_TO_TICKS(CAPTURE_TIMEOUT_MS)) != pdTRUE) {
        xSemaphoreTake(s_cam.lock, portMAX_DELAY);
        s_cam.capture_requested = false;
        xSemaphoreGive(s_cam.lock);
        ESP_LOGE(TAG, "capture timeout");
        return ESP_ERR_TIMEOUT;
    }

    xSemaphoreTake(s_cam.lock, portMAX_DELAY);
    const esp_err_t err = s_cam.capture_err;
    const size_t size = s_cam.capture_jpeg_size;
    if (err != ESP_OK || size == 0) {
        xSemaphoreGive(s_cam.lock);
        return err == ESP_OK ? ESP_FAIL : err;
    }
    uint8_t *copy = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!copy) {
        copy = malloc(size);
    }
    if (copy) {
        memcpy(copy, s_cam.capture_jpeg, size);
    }
    xSemaphoreGive(s_cam.lock);
    ESP_RETURN_ON_FALSE(copy, ESP_ERR_NO_MEM, TAG, "capture copy");

    *out_jpeg = copy;
    *out_size = size;
    if (out_width) {
        *out_width = PREVIEW_W;
    }
    if (out_height) {
        *out_height = PREVIEW_H;
    }
    ESP_LOGI(TAG, "captured JPEG: %u bytes", (unsigned)size);
    return ESP_OK;
}

esp_err_t plant_camera_push_snapshot(esp_gsp_handle_t ui,
                                     uint16_t snapshot_bind)
{
    ESP_RETURN_ON_FALSE(ui && s_cam.lock && s_cam.capture_pixels,
                        ESP_ERR_INVALID_STATE, TAG, "snapshot unavailable");
    const esp_gsp_err_t ret =
        esp_gsp_canvas_try_push(ui, snapshot_bind, s_cam.capture_pixels,
                                (size_t)PREVIEW_W * sizeof(uint16_t), NULL, NULL);
    return ret == ESP_GSP_OK ? ESP_OK : ESP_FAIL;
}

uint16_t plant_camera_rotate_clockwise(void)
{
    if (!s_cam.lock) {
        return 0;
    }
    xSemaphoreTake(s_cam.lock, portMAX_DELAY);
    s_cam.rotation_quarters = (s_cam.rotation_quarters + 1U) & 3U;
    const uint16_t degrees = (uint16_t)s_cam.rotation_quarters * 90U;
    xSemaphoreGive(s_cam.lock);
    return degrees;
}

uint16_t plant_camera_rotation_degrees(void)
{
    if (!s_cam.lock) {
        return 0;
    }
    xSemaphoreTake(s_cam.lock, portMAX_DELAY);
    const uint16_t degrees = (uint16_t)s_cam.rotation_quarters * 90U;
    xSemaphoreGive(s_cam.lock);
    return degrees;
}

esp_err_t plant_camera_set_rotation(uint16_t degrees)
{
    ESP_RETURN_ON_FALSE(degrees == 0U || degrees == 90U || degrees == 180U ||
                            degrees == 270U,
                        ESP_ERR_INVALID_ARG, TAG, "invalid rotation");
    if (!s_cam.lock) {
        s_cam.rotation_quarters = (uint8_t)(degrees / 90U);
        return ESP_OK;
    }
    xSemaphoreTake(s_cam.lock, portMAX_DELAY);
    s_cam.rotation_quarters = (uint8_t)(degrees / 90U);
    xSemaphoreGive(s_cam.lock);
    return ESP_OK;
}

void plant_camera_set_flash_enabled(bool enabled)
{
    if (!s_cam.lock) {
        s_cam.flash_enabled = enabled;
        return;
    }
    xSemaphoreTake(s_cam.lock, portMAX_DELAY);
    s_cam.flash_enabled = enabled;
    xSemaphoreGive(s_cam.lock);
}
