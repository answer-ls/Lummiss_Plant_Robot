#include "camera_photo.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "sd_card.h"

static const char *TAG = "CAMERA_PHOTO";

#define PHOTO_TASK_STACK     6144U
#define PHOTO_TASK_PRIORITY  4U
#define PHOTO_TASK_CORE      1U
#define PHOTO_TEMP_PATH      "/sdcard/photo_test.tmp"

typedef struct {
    camera_photo_complete_cb_t complete_cb;
    void *ctx;
    int64_t requested_us;
} photo_request_t;

static const uint8_t *s_photo_data;
static camera_photo_frame_release_cb_t s_photo_release_cb;
static void *s_photo_release_ctx;
static size_t s_photo_size;
static QueueHandle_t s_request_queue;
static TaskHandle_t s_photo_task;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static volatile camera_photo_state_t s_state = PHOTO_IDLE;
static bool s_initialized;
static camera_photo_status_t s_pending_failure = CAMERA_PHOTO_OK;

/* 这些统计只在真正抓到完整照片时更新，不逐帧打印。 */
static uint64_t s_jpeg_size_total;
static size_t s_jpeg_size_current;
static size_t s_jpeg_size_max;
static uint32_t s_capture_count;

const char *camera_photo_status_name(camera_photo_status_t status)
{
    switch (status) {
    case CAMERA_PHOTO_OK: return "PHOTO_OK";
    case CAMERA_PHOTO_NO_MEMORY: return "PHOTO_NO_MEMORY";
    case CAMERA_PHOTO_TIMEOUT: return "PHOTO_TIMEOUT";
    case CAMERA_PHOTO_TOO_LARGE: return "PHOTO_TOO_LARGE";
    case CAMERA_PHOTO_STORAGE_UNAVAILABLE: return "PHOTO_STORAGE_UNAVAILABLE";
    case CAMERA_PHOTO_SAVE_FAILED: return "PHOTO_SAVE_FAILED";
    default: return "PHOTO_UNKNOWN";
    }
}

camera_photo_state_t camera_photo_get_state(void)
{
    return __atomic_load_n(&s_state, __ATOMIC_ACQUIRE);
}

bool camera_photo_is_initialized(void)
{
    return __atomic_load_n(&s_initialized, __ATOMIC_ACQUIRE);
}

static void photo_set_state(camera_photo_state_t state)
{
    __atomic_store_n(&s_state, state, __ATOMIC_RELEASE);
}

static esp_err_t photo_save_jpeg(size_t jpeg_size)
{
    if (!sd_card_is_mounted()) {
        return ESP_ERR_INVALID_STATE;
    }

    FILE *file = fopen(PHOTO_TEMP_PATH, "wb");
    if (file == NULL) {
        ESP_LOGE(TAG, "无法创建临时照片：%s", strerror(errno));
        return ESP_FAIL;
    }

    const size_t written = fwrite(s_photo_data, 1, jpeg_size, file);
    const int flush_result = fflush(file);
    const int close_result = fclose(file);
    if (written != jpeg_size || flush_result != 0 || close_result != 0) {
        ESP_LOGE(TAG, "照片写入不足：expected=%u actual=%u errno=%d",
                 (unsigned)jpeg_size, (unsigned)written, errno);
        (void)remove(PHOTO_TEMP_PATH);
        return ESP_FAIL;
    }

    /* 先写临时文件再重命名，掉电或 SD 异常时不会留下半张正式照片。 */
    (void)remove(CAMERA_PHOTO_TEST_PATH);
    if (rename(PHOTO_TEMP_PATH, CAMERA_PHOTO_TEST_PATH) != 0) {
        ESP_LOGE(TAG, "照片重命名失败：%s", strerror(errno));
        (void)remove(PHOTO_TEMP_PATH);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static void photo_finish_request(const photo_request_t *request,
                                 camera_photo_status_t status,
                                 uint32_t capture_ms, uint32_t total_ms)
{
    const size_t completed_size = s_photo_size;
    camera_photo_result_t result = {
        .status = status,
        .path = status == CAMERA_PHOTO_OK ? CAMERA_PHOTO_TEST_PATH : NULL,
        .jpeg_size = status == CAMERA_PHOTO_OK ? completed_size : 0,
        .capture_ms = capture_ms,
        .total_ms = total_ms,
    };
    camera_photo_frame_release_cb_t release_cb;
    void *release_ctx;
    portENTER_CRITICAL(&s_lock);
    release_cb = s_photo_release_cb;
    release_ctx = s_photo_release_ctx;
    s_photo_data = NULL;
    s_photo_release_cb = NULL;
    s_photo_release_ctx = NULL;
    s_photo_size = 0;
    s_state = PHOTO_IDLE;
    portEXIT_CRITICAL(&s_lock);
    if (release_cb != NULL) {
        release_cb(release_ctx);
    }

    if (request->complete_cb != NULL) {
        request->complete_cb(&result, request->ctx);
    }
}

static void photo_task(void *arg)
{
    (void)arg;
    photo_request_t request;

    while (true) {
        if (xQueueReceive(s_request_queue, &request, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        portENTER_CRITICAL(&s_lock);
        s_photo_data = NULL;
        s_photo_release_cb = NULL;
        s_photo_release_ctx = NULL;
        s_photo_size = 0;
        s_pending_failure = CAMERA_PHOTO_OK;
        s_state = PHOTO_WAIT_FRAME;
        portEXIT_CRITICAL(&s_lock);

        ESP_LOGI(TAG, "PHOTO wait frame");
        uint32_t notified = ulTaskNotifyTake(pdTRUE,
                                             pdMS_TO_TICKS(CAMERA_PHOTO_TIMEOUT_MS));
        while (notified == 0 && camera_photo_get_state() == PHOTO_CAPTURING) {
            /* 超时边界若撞上 memcpy，必须等 UVC 回调彻底结束才能把 Buffer
             * 重新标为空闲，否则下一次请求可能与尚未结束的复制同时写入。 */
            notified = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20));
        }

        const uint32_t capture_ms =
            (uint32_t)((esp_timer_get_time() - request.requested_us) / 1000);
        const camera_photo_state_t wake_state = camera_photo_get_state();
        if (notified != 0 && wake_state == PHOTO_FAILED) {
            camera_photo_status_t failure;
            portENTER_CRITICAL(&s_lock);
            failure = s_pending_failure;
            portEXIT_CRITICAL(&s_lock);
            const uint32_t failed_ms =
                (uint32_t)((esp_timer_get_time() - request.requested_us) / 1000);
            ESP_LOGE(TAG, "PHOTO failed: %s",
                     camera_photo_status_name(failure));
            photo_finish_request(&request, failure, failed_ms, failed_ms);
            continue;
        }

        if (notified == 0 || wake_state != PHOTO_READY ||
            capture_ms > CAMERA_PHOTO_TIMEOUT_MS) {
            photo_set_state(PHOTO_FAILED);
            ESP_LOGW(TAG, "PHOTO failed: PHOTO_TIMEOUT");
            photo_finish_request(&request, CAMERA_PHOTO_TIMEOUT,
                                 capture_ms, capture_ms);
            continue;
        }

        photo_set_state(PHOTO_UPLOADING);
        ESP_LOGI(TAG, "PHOTO capture_ms=%u", (unsigned)capture_ms);
        ESP_LOGI(TAG, "PHOTO save begin: %s", CAMERA_PHOTO_TEST_PATH);
        camera_photo_status_t status = CAMERA_PHOTO_OK;
        if (!sd_card_is_mounted()) {
            status = CAMERA_PHOTO_STORAGE_UNAVAILABLE;
        } else if (photo_save_jpeg(s_photo_size) != ESP_OK) {
            status = CAMERA_PHOTO_SAVE_FAILED;
        }

        const uint32_t total_ms =
            (uint32_t)((esp_timer_get_time() - request.requested_us) / 1000);
        if (status == CAMERA_PHOTO_OK && total_ms > CAMERA_PHOTO_TIMEOUT_MS) {
            /* 文件已经安全落盘，但 MCP 约定的总时限已超出，仍按超时回报。 */
            status = CAMERA_PHOTO_TIMEOUT;
        }
        if (status == CAMERA_PHOTO_OK) {
            photo_set_state(PHOTO_DONE);
            ESP_LOGI(TAG, "PHOTO success: path=%s total_ms=%u",
                     CAMERA_PHOTO_TEST_PATH, (unsigned)total_ms);
        } else {
            photo_set_state(PHOTO_FAILED);
            ESP_LOGE(TAG, "PHOTO failed: %s", camera_photo_status_name(status));
        }
        photo_finish_request(&request, status, capture_ms, total_ms);
    }
}

esp_err_t camera_photo_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    s_request_queue = xQueueCreate(1, sizeof(photo_request_t));
    if (s_request_queue == NULL ||
        xTaskCreatePinnedToCoreWithCaps(photo_task, "photo_task", PHOTO_TASK_STACK,
                                        NULL, PHOTO_TASK_PRIORITY, &s_photo_task,
                                        PHOTO_TASK_CORE,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        if (s_request_queue != NULL) {
            vQueueDelete(s_request_queue);
            s_request_queue = NULL;
        }
        ESP_LOGE(TAG, "photo_task 或请求队列创建失败");
        return ESP_ERR_NO_MEM;
    }

    __atomic_store_n(&s_initialized, true, __ATOMIC_RELEASE);
    photo_set_state(PHOTO_IDLE);
    ESP_LOGI(TAG, "拍照模块已就绪：1280x720 MJPEG，复用 Camera handoff 共享槽");
    return ESP_OK;
}

esp_err_t camera_photo_request(camera_photo_complete_cb_t complete_cb, void *ctx)
{
    if (!s_initialized || complete_cb == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    photo_request_t request = {
        .complete_cb = complete_cb,
        .ctx = ctx,
        .requested_us = esp_timer_get_time(),
    };

    portENTER_CRITICAL(&s_lock);
    if (s_state != PHOTO_IDLE) {
        portEXIT_CRITICAL(&s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_state = PHOTO_REQUESTED;
    s_photo_size = 0;
    s_pending_failure = CAMERA_PHOTO_OK;
    portEXIT_CRITICAL(&s_lock);

    if (xQueueSend(s_request_queue, &request, 0) != pdTRUE) {
        photo_set_state(PHOTO_IDLE);
        return ESP_ERR_TIMEOUT;
    }
    ESP_LOGI(TAG, "PHOTO request received");
    return ESP_OK;
}

bool camera_photo_submit_mjpeg_owned(
    const uint8_t *data, size_t data_len, uint16_t width, uint16_t height,
    camera_photo_frame_release_cb_t release_cb, void *release_ctx)
{
    if (camera_photo_get_state() != PHOTO_WAIT_FRAME) {
        return false;
    }
    if (data == NULL || release_cb == NULL || data_len < 4 ||
        width != CAMERA_PHOTO_WIDTH ||
        height != CAMERA_PHOTO_HEIGHT || data[0] != 0xff || data[1] != 0xd8 ||
        data[data_len - 2] != 0xff || data[data_len - 1] != 0xd9) {
        return false;
    }
    if (data_len > CAMERA_PHOTO_BUFFER_CAPACITY) {
        ESP_LOGE(TAG, "PHOTO_TOO_LARGE: jpeg_size=%u capacity=%u",
                 (unsigned)data_len, (unsigned)CAMERA_PHOTO_BUFFER_CAPACITY);
        /* 尺寸正确且 JPEG 边界完整，但本地缓冲容不下时立即结束本次请求，
         * 不继续等待到 5 秒后再错误地报告 PHOTO_TIMEOUT。 */
        bool failure_signaled = false;
        portENTER_CRITICAL(&s_lock);
        if (s_state == PHOTO_WAIT_FRAME) {
            s_photo_size = data_len;
            s_pending_failure = CAMERA_PHOTO_TOO_LARGE;
            s_state = PHOTO_FAILED;
            failure_signaled = true;
        }
        portEXIT_CRITICAL(&s_lock);
        if (failure_signaled) {
            xTaskNotifyGive(s_photo_task);
        }
        return false;
    }

    portENTER_CRITICAL(&s_lock);
    if (s_state != PHOTO_WAIT_FRAME) {
        portEXIT_CRITICAL(&s_lock);
        return false;
    }
    s_state = PHOTO_CAPTURING;
    portEXIT_CRITICAL(&s_lock);

    portENTER_CRITICAL(&s_lock);
    s_photo_data = data;
    s_photo_release_cb = release_cb;
    s_photo_release_ctx = release_ctx;
    s_photo_size = data_len;
    s_jpeg_size_current = data_len;
    s_jpeg_size_total += data_len;
    s_capture_count++;
    if (data_len > s_jpeg_size_max) {
        s_jpeg_size_max = data_len;
    }
    s_state = PHOTO_READY;
    portEXIT_CRITICAL(&s_lock);

    const size_t jpeg_avg = s_capture_count > 0
                                ? (size_t)(s_jpeg_size_total / s_capture_count)
                                : 0;
    ESP_LOGI(TAG, "PHOTO captured: %ux%u", width, height);
    ESP_LOGI(TAG, "PHOTO jpeg_size=%u avg=%u max=%u",
             (unsigned)s_jpeg_size_current, (unsigned)jpeg_avg,
             (unsigned)s_jpeg_size_max);
    xTaskNotifyGive(s_photo_task);
    return true;
}
