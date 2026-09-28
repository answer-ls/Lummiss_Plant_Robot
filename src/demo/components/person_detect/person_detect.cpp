#include "person_detect.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <inttypes.h>
#include <list>

#include "coco_detect.hpp"
#include "dl_image_define.hpp"
#include "driver/jpeg_decode.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/task.h"

namespace {

static constexpr const char *TAG = "PERSON_DETECT";
static constexpr size_t JPEG_BUFFER_SIZE = 256U * 1024U;
static constexpr uint32_t SNAPSHOT_INTERVAL_MS = 1000U;
static constexpr uint32_t STATS_INTERVAL_MS = 5000U;
static constexpr uint16_t CAMERA_WIDTH = 640U;
static constexpr uint16_t CAMERA_HEIGHT = 480U;
static constexpr uint16_t MODEL_WIDTH = 320U;
static constexpr uint16_t MODEL_HEIGHT = 320U;
static constexpr size_t RGB565_BUFFER_SIZE =
    static_cast<size_t>(CAMERA_WIDTH) * CAMERA_HEIGHT * 2U;
static constexpr size_t MODEL_RGB565_BUFFER_SIZE =
    static_cast<size_t>(MODEL_WIDTH) * MODEL_HEIGHT * 2U;
static_assert(CAMERA_WIDTH == MODEL_WIDTH * 2U &&
              CAMERA_HEIGHT == (MODEL_HEIGHT - 80U) * 2U,
              "当前 letterbox 仅适配 640x480 -> 320x240 + 上下各 40px");
static constexpr int PERSON_CATEGORY = 0;
static constexpr float PERSON_SCORE_THRESHOLD = 0.45F;//置信度
static constexpr uint32_t TASK_STACK_SIZE = 16384U;
static constexpr UBaseType_t TASK_PRIORITY = 4U;
static constexpr BaseType_t TASK_CORE = 1;
static constexpr EventBits_t MODEL_READY_BIT = BIT0;
static constexpr EventBits_t MODEL_FAILED_BIT = BIT1;

typedef struct {
    size_t size;
} queued_frame_t;

static QueueHandle_t s_queue;
static TaskHandle_t s_task;
static StaticEventGroup_t s_startup_event_storage;
static EventGroupHandle_t s_startup_events;
static uint8_t *s_jpeg_input;
static size_t s_jpeg_allocated;
static uint8_t *s_rgb565;
static size_t s_rgb565_allocated;
static uint8_t *s_model_rgb565;
static size_t s_model_rgb565_allocated;
static std::atomic<bool> s_initialized{false};
static std::atomic<bool> s_stop_requested{false};
static std::atomic<bool> s_processing{false};
static std::atomic<uint32_t> s_frame_seq{0};
static std::atomic<int64_t> s_last_submit_us{0};
static std::atomic<uint32_t> s_input_frames{0};
static std::atomic<uint32_t> s_processed_frames{0};
static std::atomic<uint32_t> s_dropped_frames{0};
static std::atomic<uint32_t> s_decode_errors{0};
static std::atomic<uint32_t> s_invalid_output_frames{0};
static std::atomic<uint32_t> s_invalid_input_frames{0};
static std::atomic<uint32_t> s_detected_frames{0};
static std::atomic<int64_t> s_preprocess_sum_us{0};
static std::atomic<int64_t> s_preprocess_max_us{0};
static std::atomic<int64_t> s_letterbox_sum_us{0};
static std::atomic<int64_t> s_letterbox_max_us{0};
static std::atomic<int64_t> s_inference_sum_us{0};
static std::atomic<int64_t> s_inference_max_us{0};

static void person_detect_log_memory(const char *stage)
{
    ESP_LOGI(TAG,
             "MEM[%s]: DMA free=%u largest=%u INT free=%u largest=%u "
             "PSRAM free=%u largest=%u",
             stage,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
}

static void update_max(std::atomic<int64_t> &value, int64_t sample)
{
    int64_t old = value.load(std::memory_order_relaxed);
    while (old < sample &&
           !value.compare_exchange_weak(old, sample,
                                        std::memory_order_relaxed,
                                        std::memory_order_relaxed)) {
    }
}

static void print_stats(void)
{
    const uint32_t input = s_input_frames.exchange(0);
    const uint32_t processed = s_processed_frames.exchange(0);
    const uint32_t dropped = s_dropped_frames.exchange(0);
    const uint32_t errors = s_decode_errors.exchange(0);
    const uint32_t detected = s_detected_frames.exchange(0);
    const uint32_t invalid_output = s_invalid_output_frames.exchange(0);
    const uint32_t invalid_input = s_invalid_input_frames.exchange(0);
    const int64_t preprocess_sum = s_preprocess_sum_us.exchange(0);
    const int64_t preprocess_max = s_preprocess_max_us.exchange(0);
    const int64_t letterbox_sum = s_letterbox_sum_us.exchange(0);
    const int64_t letterbox_max = s_letterbox_max_us.exchange(0);
    const int64_t inference_sum = s_inference_sum_us.exchange(0);
    const int64_t inference_max = s_inference_max_us.exchange(0);

    const uint32_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const uint32_t internal_largest =
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    const uint32_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    const uint32_t psram_min = heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM);

    ESP_LOGI(TAG,
             "stats input=%u processed=%u dropped=%u errors=%u detected=%u "
             "invalid_output=%u invalid_input=%u "
             "AI_FPS=%.2f jpeg_decode_ms=%.2f/max=%.2f "
             "letterbox_ms=%.2f/max=%.2f "
             "coco_run_ms=%.2f/max=%.2f "
             "espdl_preprocess_postprocess_ms=API内置 "
             "PSRAM free/min=%u/%u internal free/largest=%u/%u",
             input, processed, dropped, errors, detected, invalid_output,
             invalid_input,
             processed * (1000.0F / STATS_INTERVAL_MS),
             processed ? (float)preprocess_sum / processed / 1000.0F : 0.0F,
             (float)preprocess_max / 1000.0F,
             processed ? (float)letterbox_sum / processed / 1000.0F : 0.0F,
             (float)letterbox_max / 1000.0F,
             processed ? (float)inference_sum / processed / 1000.0F : 0.0F,
             (float)inference_max / 1000.0F,
             (unsigned)psram_free, (unsigned)psram_min,
             (unsigned)internal_free, (unsigned)internal_largest);
}

static void release_runtime_resources(void)
{
    if (s_queue != nullptr) {
        vQueueDelete(s_queue);
        s_queue = nullptr;
    }
    if (s_jpeg_input != nullptr) {
        heap_caps_free(s_jpeg_input);
        s_jpeg_input = nullptr;
    }
    if (s_rgb565 != nullptr) {
        heap_caps_free(s_rgb565);
        s_rgb565 = nullptr;
    }
    if (s_model_rgb565 != nullptr) {
        heap_caps_free(s_model_rgb565);
        s_model_rgb565 = nullptr;
    }
    s_jpeg_allocated = 0;
    s_rgb565_allocated = 0;
    s_model_rgb565_allocated = 0;
    s_task = nullptr;
}

/* ESP-DL 的 img_t 没有 stride 字段，RGB565 必须是紧密排列。 */
static bool validate_input(const dl::image::img_t &image,
                           const uint8_t *base, size_t capacity,
                           uint32_t seq)
{
    const size_t stride = static_cast<size_t>(image.width) * 2U;
    const size_t bytes = stride * image.height;
    const uintptr_t src = reinterpret_cast<uintptr_t>(image.data);
    const uintptr_t begin = reinterpret_cast<uintptr_t>(base);
    const bool valid = image.data != nullptr && base != nullptr &&
        image.width > 0 && image.height > 0 &&
        image.pix_type == dl::image::DL_IMAGE_PIX_TYPE_RGB565LE &&
        image.row_step() == static_cast<int>(stride) &&
        image.bytes() >= bytes && capacity >= bytes &&
        (src & 3U) == 0U && src >= begin &&
        src - begin <= capacity - bytes;
    if (seq <= 3U || seq % 100U == 0U || !valid) {
        ESP_LOGI(TAG,
                 "INPUT_CHECK[before_run] frame_seq=%u src=%p buffer=[%p,%p) "
                 "width=%u height=%u stride=%u bytes=%u stack_hw=%u "
                 "DMA=%u/%u INT=%u/%u PSRAM=%u/%u valid=%d",
                 (unsigned)seq, image.data, base, base + capacity,
                 (unsigned)image.width, (unsigned)image.height,
                 (unsigned)stride, (unsigned)bytes,
                 (unsigned)uxTaskGetStackHighWaterMark(nullptr),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM),
                 valid ? 1 : 0);
    }
    if (!valid) {
        s_invalid_input_frames.fetch_add(1);
    }
    return valid;
}

/* 2:1 最近邻缩小成 320x240，再放入 320x320 画布中央。
 * 上下各 40 行黑边在初始化时清零，正常播放只改写中间 240 行。 */
static void downsample_letterbox_320(const uint8_t *src, uint8_t *dst)
{
    constexpr uint32_t border_y = (MODEL_HEIGHT - CAMERA_HEIGHT / 2U) / 2U;
    for (uint32_t y = 0; y < CAMERA_HEIGHT / 2U; ++y) {
        const uint8_t *src_row = src + static_cast<size_t>(y * 2U) * CAMERA_WIDTH * 2U;
        uint8_t *dst_row = dst + static_cast<size_t>(y + border_y) * MODEL_WIDTH * 2U;
        for (uint32_t x = 0; x < MODEL_WIDTH; ++x) {
            const size_t from = static_cast<size_t>(x) * 4U;
            const size_t to = static_cast<size_t>(x) * 2U;
            dst_row[to] = src_row[from];
            dst_row[to + 1U] = src_row[from + 1U];
        }
    }
}

/* 模型坐标反变换到原始 640x480；黑边中的坐标裁剪到图像边缘。 */
static int model_x_to_camera(int x)
{
    return std::min(std::clamp(x, 0, static_cast<int>(MODEL_WIDTH)) * 2,
                    static_cast<int>(CAMERA_WIDTH) - 1);
}

static int model_y_to_camera(int y)
{
    return std::clamp((std::clamp(y, 0, static_cast<int>(MODEL_HEIGHT)) - 40) * 2,
                      0, static_cast<int>(CAMERA_HEIGHT) - 1);
}

static void person_detect_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "person_detect_task 启动（CPU%d，队列深度=1）",
             xPortGetCoreID());

    /* 模型独立烧录到 Flash 的 model 分区，避免从 SDMMC 读取模型时申请 DMA。 */
    const esp_partition_t *model_partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "model");
    if (model_partition == nullptr) {
        ESP_LOGE(TAG, "model partition not found");
        xEventGroupSetBits(s_startup_events, MODEL_FAILED_BIT);
        s_initialized.store(false);
        release_runtime_resources();
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG,
             "model partition found: address=0x%08" PRIx32 " size=%" PRIu32
             " bytes",
             model_partition->address, model_partition->size);

    /* 使用官方 coco_detect 的 YOLO11n 320x320 INT8 封装和后处理。 */
    /* CONFIG_COCO_DETECT_MODEL_IN_FLASH_PARTITION 使官方封装从 model 分区加载。 */
    person_detect_log_memory("YOLO_LOAD_BEFORE");
    COCODetect *detector = new COCODetect(
        COCODetect::YOLO11N_320_S8_V1, false);
    if (detector == nullptr) {
        ESP_LOGE(TAG, "model load failed");
        xEventGroupSetBits(s_startup_events, MODEL_FAILED_BIT);
        s_initialized.store(false);
        release_runtime_resources();
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "model load OK (COCO YOLO11n 320x320 INT8)");
    person_detect_log_memory("YOLO_LOAD_AFTER");

    const jpeg_decode_engine_cfg_t engine_cfg = {
        .intr_priority = 0,
        .timeout_ms = 1000,
    };
    jpeg_decoder_handle_t decoder = nullptr;
    if (jpeg_new_decoder_engine(&engine_cfg, &decoder) != ESP_OK) {
        ESP_LOGE(TAG, "创建 AI JPEG 硬件解码引擎失败");
        xEventGroupSetBits(s_startup_events, MODEL_FAILED_BIT);
        delete detector;
        s_initialized.store(false);
        release_runtime_resources();
        vTaskDelete(NULL);
        return;
    }

    const jpeg_decode_cfg_t decode_cfg = {
        .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
        .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_RGB,
        .conv_std = JPEG_YUV_RGB_CONV_STD_BT601,
    };
    /* 模型的临时加载内存已经释放，允许天气 HTTPS 再启动。 */
    xEventGroupSetBits(s_startup_events, MODEL_READY_BIT);
    queued_frame_t queued = {};
    int64_t last_stats_us = esp_timer_get_time();
    bool first_inference_logged = false;

    while (!s_stop_requested.load(std::memory_order_acquire)) {
        if (xQueueReceive(s_queue, &queued, pdMS_TO_TICKS(200)) != pdTRUE) {
            if (esp_timer_get_time() - last_stats_us >=
                static_cast<int64_t>(STATS_INTERVAL_MS) * 1000) {
                print_stats();
                last_stats_us = esp_timer_get_time();
            }
            continue;
        }

        s_processing.store(true, std::memory_order_release);
        uint32_t decoded_size = 0;
        const int64_t preprocess_started_us = esp_timer_get_time();
        const esp_err_t decode_error = jpeg_decoder_process(
            decoder, &decode_cfg, s_jpeg_input, (uint32_t)queued.size,
            s_rgb565, (uint32_t)s_rgb565_allocated, &decoded_size);
        const int64_t preprocess_elapsed_us =
            esp_timer_get_time() - preprocess_started_us;
        s_preprocess_sum_us.fetch_add(preprocess_elapsed_us);
        update_max(s_preprocess_max_us, preprocess_elapsed_us);
        if (decode_error != ESP_OK) {
            s_decode_errors.fetch_add(1);
            ESP_LOGW(TAG, "AI JPEG decode failed: %s", esp_err_to_name(decode_error));
            s_processing.store(false, std::memory_order_release);
            continue;
        }

        /* 即使 JPEG 驱动返回 ESP_OK，也必须确认完整输出 640x480，
         * 避免下采样读取未写入区域。 */
        if (decoded_size != RGB565_BUFFER_SIZE) {
            s_invalid_output_frames.fetch_add(1);
            ESP_LOGW(TAG,
                     "AI JPEG 输出尺寸异常：decoded=%u expected=%u，跳过本帧",
                     (unsigned)decoded_size,
                     (unsigned)RGB565_BUFFER_SIZE);
            s_processing.store(false, std::memory_order_release);
            continue;
        }

        /* 先验证硬解输出确实完整，避免把坏尺寸传给 ESP-DL。 */
        const uint32_t frame_seq = s_frame_seq.fetch_add(1) + 1U;
        dl::image::img_t decoded_image = {
            .data = s_rgb565,
            .width = CAMERA_WIDTH,
            .height = CAMERA_HEIGHT,
            .pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB565LE,
        };
        if (!validate_input(decoded_image, s_rgb565, s_rgb565_allocated,
                            frame_seq)) {
            s_processing.store(false, std::memory_order_release);
            continue;
        }

        /* 在调用 ESP-DL 前完成 2:1 缩小与 letterbox，模型只看到
         * 320x320 RGB565LE，因此不会调用其 640x480 resize SIMD。 */
        const int64_t letterbox_started_us = esp_timer_get_time();
        downsample_letterbox_320(s_rgb565, s_model_rgb565);
        const int64_t letterbox_elapsed_us =
            esp_timer_get_time() - letterbox_started_us;
        s_letterbox_sum_us.fetch_add(letterbox_elapsed_us);
        update_max(s_letterbox_max_us, letterbox_elapsed_us);
        dl::image::img_t image = {
            .data = s_model_rgb565,
            .width = MODEL_WIDTH,
            .height = MODEL_HEIGHT,
            .pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB565LE,
        };
        const uint8_t *image_base = s_model_rgb565;
        const size_t image_capacity = s_model_rgb565_allocated;
        if (!validate_input(image, image_base, image_capacity, frame_seq)) {
            s_processing.store(false, std::memory_order_release);
            continue;
        }
        const int64_t inference_started_us = esp_timer_get_time();
        std::list<dl::detect::result_t> &results = detector->run(image);
        const int64_t inference_elapsed_us =
            esp_timer_get_time() - inference_started_us;
        s_inference_sum_us.fetch_add(inference_elapsed_us);
        update_max(s_inference_max_us, inference_elapsed_us);
        s_processed_frames.fetch_add(1);
        if (!first_inference_logged) {
            first_inference_logged = true;
            person_detect_log_memory("YOLO_FIRST_INFER");
            ESP_LOGI(TAG, "AI inference started");
        }

        bool person_found = false;
        float best_score = 0.0F;
        int best_box[4] = {0, 0, 0, 0};
        uint32_t person_count = 0;
        for (const auto &result : results) {
            if (result.category != PERSON_CATEGORY ||
                result.score < PERSON_SCORE_THRESHOLD || result.box.size() < 4) {
                continue;
            }
            const int x1 = model_x_to_camera(result.box[0]);
            const int y1 = model_y_to_camera(result.box[1]);
            const int x2 = model_x_to_camera(result.box[2]);
            const int y2 = model_y_to_camera(result.box[3]);
            /* 完全落在上下黑边的检测框裁剪后没有面积，应忽略。 */
            if (x2 <= x1 || y2 <= y1) {
                continue;
            }
            ++person_count;
            if (!person_found || result.score > best_score) {
                person_found = true;
                best_score = result.score;
                best_box[0] = x1;
                best_box[1] = y1;
                best_box[2] = x2;
                best_box[3] = y2;
            }
        }
        if (person_found) {
            s_detected_frames.fetch_add(1);
            ESP_LOGI(TAG,
                     "PERSON YES score=%.2f bbox=(%d,%d)-(%d,%d) person_count=%u",
                     best_score, best_box[0], best_box[1], best_box[2], best_box[3],
                     (unsigned)person_count);
        } else {
            ESP_LOGI(TAG, "PERSON NO");
        }
        s_processing.store(false, std::memory_order_release);

        if (esp_timer_get_time() - last_stats_us >=
            static_cast<int64_t>(STATS_INTERVAL_MS) * 1000) {
            print_stats();
            last_stats_us = esp_timer_get_time();
        }
    }

    jpeg_del_decoder_engine(decoder);
    delete detector;
    release_runtime_resources();
    ESP_LOGI(TAG, "person_detect_task 已停止");
    vTaskDelete(NULL);
}

} // namespace

extern "C" esp_err_t person_detect_init(void)
{
    if (s_initialized.load(std::memory_order_acquire)) {
        return ESP_OK;
    }
    if (s_queue != nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_startup_events == nullptr) {
        s_startup_events = xEventGroupCreateStatic(&s_startup_event_storage);
        if (s_startup_events == nullptr) {
            return ESP_ERR_NO_MEM;
        }
    }
    xEventGroupClearBits(s_startup_events, MODEL_READY_BIT | MODEL_FAILED_BIT);

    const jpeg_decode_memory_alloc_cfg_t input_cfg = {
        .buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER,
    };
    const jpeg_decode_memory_alloc_cfg_t output_cfg = {
        .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER,
    };
    s_jpeg_input = static_cast<uint8_t *>(jpeg_alloc_decoder_mem(
        JPEG_BUFFER_SIZE, &input_cfg, &s_jpeg_allocated));
    s_rgb565 = static_cast<uint8_t *>(jpeg_alloc_decoder_mem(
        RGB565_BUFFER_SIZE, &output_cfg, &s_rgb565_allocated));
    s_model_rgb565 = static_cast<uint8_t *>(heap_caps_malloc(
        MODEL_RGB565_BUFFER_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    s_model_rgb565_allocated = MODEL_RGB565_BUFFER_SIZE;
    if (s_jpeg_input == nullptr || s_rgb565 == nullptr ||
        s_model_rgb565 == nullptr) {
        ESP_LOGE(TAG, "AI 缓冲分配失败：jpeg=%p rgb565=%p letterbox=%p",
                 s_jpeg_input, s_rgb565, s_model_rgb565);
        if (s_jpeg_input != nullptr) {
            heap_caps_free(s_jpeg_input);
            s_jpeg_input = nullptr;
        }
        if (s_rgb565 != nullptr) {
            heap_caps_free(s_rgb565);
            s_rgb565 = nullptr;
        }
        if (s_model_rgb565 != nullptr) {
            heap_caps_free(s_model_rgb565);
            s_model_rgb565 = nullptr;
        }
        return ESP_ERR_NO_MEM;
    }
    /* 黑边只初始化一次；之后每帧只覆盖中间 240 行。 */
    memset(s_model_rgb565, 0, MODEL_RGB565_BUFFER_SIZE);
    ESP_LOGI(TAG,
             "AI buffers：MJPEG=%u bytes PSRAM，RGB565=%u bytes PSRAM，"
             "letterbox=%u bytes；"
             "未申请 MALLOC_CAP_DMA",
             (unsigned)s_jpeg_allocated, (unsigned)s_rgb565_allocated,
             (unsigned)s_model_rgb565_allocated);

    s_queue = xQueueCreate(1, sizeof(queued_frame_t));
    /* 先标记为已初始化，再创建任务，避免任务抢占后在模型失败路径中
     * 清理资源，而 app_main 随后又把已释放的实例误认为初始化成功。 */
    s_initialized.store(true, std::memory_order_release);
    /* Flash 分区模型通过 esp_partition_mmap() 映射时，ESP-IDF 会临时关闭
     * Flash/PSRAM cache。调用该接口的当前任务栈必须位于内部 DRAM，否则
     * cache_utils.c 的 esp_task_stack_is_sane_cache_disabled() 会触发断言。
     * 图像 Buffer 仍然放在 PSRAM；这里只把 16 KB 的任务栈固定到内部 RAM。 */
    if (s_queue == nullptr || xTaskCreatePinnedToCoreWithCaps(
            person_detect_task, "person_detect", TASK_STACK_SIZE, nullptr,
            TASK_PRIORITY, &s_task, TASK_CORE,
            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGE(TAG, "创建 person_detect_task 失败");
        s_initialized.store(false, std::memory_order_release);
        if (s_queue != nullptr) {
            vQueueDelete(s_queue);
            s_queue = nullptr;
        }
        heap_caps_free(s_jpeg_input);
        heap_caps_free(s_rgb565);
        heap_caps_free(s_model_rgb565);
        s_jpeg_input = nullptr;
        s_rgb565 = nullptr;
        s_model_rgb565 = nullptr;
        return ESP_ERR_NO_MEM;
    }
    s_stop_requested.store(false, std::memory_order_release);
    s_frame_seq.store(0, std::memory_order_release);
    ESP_LOGI(TAG, "AI 输入：640x480 RGB565 -> 2:1 最近邻 320x240 -> "
             "上下各 40px 黑边 -> 320x320 RGB565LE（PSRAM=%u bytes）",
             (unsigned)MODEL_RGB565_BUFFER_SIZE);
    ESP_LOGI(TAG, "本地人体检测已启用：每 %u ms 抽帧，阈值=%.2f",
             SNAPSHOT_INTERVAL_MS, PERSON_SCORE_THRESHOLD);
    return ESP_OK;
}

extern "C" esp_err_t person_detect_wait_startup(uint32_t timeout_ms)
{
    if (s_startup_events == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    const EventBits_t bits = xEventGroupWaitBits(
        s_startup_events, MODEL_READY_BIT | MODEL_FAILED_BIT,
        pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
    if ((bits & MODEL_READY_BIT) != 0) {
        return ESP_OK;
    }
    return (bits & MODEL_FAILED_BIT) != 0 ? ESP_FAIL : ESP_ERR_TIMEOUT;
}

extern "C" bool person_detect_submit_mjpeg(const uint8_t *data, size_t size)
{
    if (!s_initialized.load(std::memory_order_acquire) || data == nullptr ||
        size == 0 || size > JPEG_BUFFER_SIZE || s_queue == nullptr) {
        return false;
    }
    const int64_t now_us = esp_timer_get_time();
    const int64_t last_us = s_last_submit_us.load(std::memory_order_relaxed);
    if (last_us != 0 && now_us - last_us <
                            static_cast<int64_t>(SNAPSHOT_INTERVAL_MS) * 1000) {
        return false;
    }
    /* 单输入缓冲必须先原子占用，再 memcpy。原实现先检查 processing，
     * 任务随后才置位，中间窗口可能让回调覆盖正在被解码/推理的 JPEG。 */
    bool expected_idle = false;
    const bool claimed = s_processing.compare_exchange_strong(
        expected_idle, true, std::memory_order_acq_rel,
        std::memory_order_acquire);
    if (!claimed || uxQueueMessagesWaiting(s_queue) != 0) {
        if (claimed) {
            s_processing.store(false, std::memory_order_release);
        }
        s_dropped_frames.fetch_add(1);
        return false;
    }
    memcpy(s_jpeg_input, data, size);
    queued_frame_t queued = {.size = size};
    if (xQueueSend(s_queue, &queued, 0) != pdTRUE) {
        s_processing.store(false, std::memory_order_release);
        s_dropped_frames.fetch_add(1);
        return false;
    }
    s_last_submit_us.store(now_us, std::memory_order_relaxed);
    s_input_frames.fetch_add(1);
    return true;
}

extern "C" void person_detect_deinit(void)
{
    if (!s_initialized.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    s_stop_requested.store(true, std::memory_order_release);
    if (s_task != nullptr) {
        xTaskNotifyGive(s_task);
    }
    /* 任务会在下一次轮询退出；释放工作缓冲由调用者在系统停机阶段完成。 */
}
