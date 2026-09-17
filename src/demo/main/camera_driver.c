#include <assert.h>
#include <inttypes.h>
#include <math.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "usb/usb_host.h"
#include "usb/uvc_host.h"
#include "esp_private/uvc_isoc_diag.h"
#include "esp_private/uvc_stream.h"

#include "camera_driver.h"
#include "video_streamer.h"

static const char *TAG = "CAMERA";

#define CAMERA_DISCONNECTED BIT0
#define CAMERA_ERROR        BIT1
#define CAMERA_FORMATS_READY BIT2
#define MAX_CAMERA_FORMATS  32
#define CAMERA_STALL_LIMIT  1
#define CAMERA_SAME_FORMAT_RETRY_LIMIT 1
#define CAMERA_HANDOFF_COPY_SIZE  (256U * 1024U)
#define CAMERA_HANDOFF_COPY_COUNT 3
#define CAMERA_HANDOFF_QUEUE_LENGTH CAMERA_HANDOFF_COPY_COUNT
#define CAMERA_HANDOFF_TASK_PRIORITY 18
#define CAMERA_HANDOFF_TASK_STACK 4096
#define CAMERA_USB_ALL_FREE        BIT3
#define CAMERA_USB_EVENTS_EXITED   BIT4
#define CAMERA_HANDOFF_IDLE        BIT5
#define CAMERA_USB_EVENTS_PRIORITY 19
#define CAMERA_UVC_DRIVER_PRIORITY 20
#define CAMERA_USB_CORE             0
#if CAMERA_TEST_PROFILE == CAMERA_TEST_UVC_ONLY
#define CAMERA_REQUESTED_FPS        30.0f
#else
#define CAMERA_REQUESTED_FPS        30.0f
#endif
/* UVC 丢帧根因已定位（2026-09-12）：URB 数据缓冲落 PSRAM 会与 H.264/JPEG 争
 * PSRAM 仲裁，ISOC 回调最长被推迟 8 ms，期间等时包被跳过整帧丢弃。
 * 8×16 KB 配内部 RAM（CONFIG_USB_HOST_DWC_DMA_CAP_MEMORY_IN_PSRAM=n）后丢帧清零；
 * 反向对照：URB 96→8 只把 PSRAM 下的丢帧从 ~35% 降到 ~10%，真正清零靠内部 RAM。
 * 16 KB 被 MPS=3072 对齐成 18432 B/URB，8 个共 144 KiB。 */
#define CAMERA_USB_URB_COUNT        8
#define CAMERA_USB_URB_SIZE         (16U * 1024U)
#define CAMERA_FRAME_BUFFER_COUNT   3
#define CAMERA_REPORT_INTERVAL_MS   5000
#define CAMERA_FIRST_FRAME_TIMEOUT_MS 5000
#define CAMERA_FIRST_FRAME_POLL_MS     10
#define CAMERA_POWER_ON_DELAY_MS       2000

/* 纯 UVC 诊断固件一次只编译一个固定分辨率。切换宏后必须冷启动，运行中
 * 不再轮转格式，避免上一次失败残留在摄像头内部状态机中。 */
#if CAMERA_TEST_PROFILE == CAMERA_TEST_UVC_ONLY
#if CAMERA_UVC_COLD_TEST_MODE == CAMERA_UVC_COLD_TEST_1280X720
#define CAMERA_CAPTURE_WIDTH           1280U
#define CAMERA_CAPTURE_HEIGHT          720U
#elif CAMERA_UVC_COLD_TEST_MODE == CAMERA_UVC_COLD_TEST_640X480
#define CAMERA_CAPTURE_WIDTH           640U
#define CAMERA_CAPTURE_HEIGHT          480U
#else
#error "未知的纯 UVC 冷启动测试模式"
#endif
#else
#define CAMERA_CAPTURE_WIDTH           VIDEO_STREAM_WIDTH
#define CAMERA_CAPTURE_HEIGHT          VIDEO_STREAM_HEIGHT
#endif

static EventGroupHandle_t s_camera_events;
static portMUX_TYPE s_stats_lock = portMUX_INITIALIZER_UNLOCKED;
static uvc_host_frame_info_t s_camera_formats[MAX_CAMERA_FORMATS];
static size_t s_camera_format_count;
static uint8_t s_camera_device_address;
static uint8_t s_camera_stream_index;
/* 是否从真实摄像头连接回调里读到过格式列表。
 * 只有从未读到过时才允许用“已知格式”兜底；一旦确认过真实设备，
 * 之后的断开只能靠真实的重新接入事件恢复，避免对空总线反复探测。 */
static bool s_saw_real_formats;
static volatile bool s_usb_events_running;
static TaskHandle_t s_usb_events_task;

typedef struct {
    uint32_t frames;
    uint32_t target_frames;      /* 分辨率和格式符合目标的 MJPEG 帧，包括损坏帧 */
    /* 有 SOI、未超缓冲上限，已交给编解码任务待结构校验；是否包含 EOI
     * 由编解码任务在 USB 回调之外判定。 */
    uint32_t candidate_frames;
    uint32_t missing_soi_frames;
    uint32_t missing_eoi_frames;
    uint32_t oversized_frames;
    uint32_t empty_frames;
    uint64_t bytes;
    uint64_t callback_us;
    uint32_t callback_count;
    uint32_t callback_max_us;
    uint32_t handoff_rejected;
    size_t last_size;
    unsigned width;
    unsigned height;
    int format;
} camera_stats_t;

static camera_stats_t s_stats;

static void camera_record_frame_callback_time(int64_t started_us)
{
    const uint32_t elapsed_us = (uint32_t)(esp_timer_get_time() - started_us);
    portENTER_CRITICAL(&s_stats_lock);
    s_stats.callback_us += elapsed_us;
    s_stats.callback_count++;
    if (elapsed_us > s_stats.callback_max_us) {
        s_stats.callback_max_us = elapsed_us;
    }
    portEXIT_CRITICAL(&s_stats_lock);
}

static camera_stats_t camera_stats_snapshot(void)
{
    camera_stats_t snapshot;
    portENTER_CRITICAL(&s_stats_lock);
    snapshot = s_stats;
    portEXIT_CRITICAL(&s_stats_lock);
    return snapshot;
}

/* uvc_host_stream_start() only submits the transfer ring and returns. Wait for
 * an actual target MJPEG frame before treating the start as successful. This
 * distinguishes a live stream from a ring that only receives empty ISOC
 * packets during a failed first start. */
static bool camera_wait_first_frame(uint32_t timeout_ms, uint32_t *elapsed_ms)
{
    const int64_t started_us = esp_timer_get_time();
    const int64_t deadline_us = started_us + (int64_t)timeout_ms * 1000;

    while (esp_timer_get_time() < deadline_us) {
        const EventBits_t bits = xEventGroupGetBits(s_camera_events);
        if (bits & (CAMERA_DISCONNECTED | CAMERA_ERROR)) {
            return false;
        }

        const camera_stats_t current = camera_stats_snapshot();
        if (current.candidate_frames != 0) {
            if (elapsed_ms != NULL) {
                *elapsed_ms = (uint32_t)((esp_timer_get_time() - started_us) / 1000);
            }
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(CAMERA_FIRST_FRAME_POLL_MS));
    }

    if (elapsed_ms != NULL) {
        *elapsed_ms = timeout_ms;
    }
    return false;
}

typedef struct {
    uint8_t *data;
    size_t data_len;
    bool in_use;
} camera_frame_copy_t;

typedef struct {
    camera_frame_copy_t *copy;
} camera_frame_handoff_item_t;

static camera_frame_copy_t s_frame_copies[CAMERA_HANDOFF_COPY_COUNT];
static portMUX_TYPE s_frame_copy_lock = portMUX_INITIALIZER_UNLOCKED;
static QueueHandle_t s_camera_handoff_queue;
static volatile bool s_camera_handoff_accepting;

static const char *camera_format_name(enum uvc_host_stream_format format)
{
    switch (format) {
    case UVC_VS_FORMAT_MJPEG:
        return "MJPEG";
    case UVC_VS_FORMAT_YUY2:
        return "YUY2";
    case UVC_VS_FORMAT_H264:
        return "H264";
    case UVC_VS_FORMAT_H265:
        return "H265";
    default:
        return "UNKNOWN";
    }
}

/* UVC 帧间隔单位为 100 ns，转换成驱动配置所需的帧率。 */
static float camera_interval_to_fps(uint32_t interval)
{
    return interval ? 10000000.0f / interval : 30.0f;
}

/* 选择当前档位要求的帧率（纯 UVC 冷启动测试为 30 fps，生产链路为 20 fps）。连续区间按 step 对齐，离散区间只
 * 接受精确匹配；如果设备没有该帧间隔，返回默认帧率，避免 UVC 控制
 * 请求失败后把问题误判成 USB 丢包。 */
static float camera_select_stream_fps(const uvc_host_frame_info_t *frame_info)
{
    const float default_fps = camera_interval_to_fps(frame_info->default_interval);
    if (frame_info->format != UVC_VS_FORMAT_MJPEG ||
        frame_info->h_res != CAMERA_CAPTURE_WIDTH ||
        frame_info->v_res != CAMERA_CAPTURE_HEIGHT) {
        return default_fps;
    }

    if (frame_info->interval_type == 0) {
        if (frame_info->interval_min == 0 || frame_info->interval_max == 0 ||
            frame_info->interval_step == 0) {
            ESP_LOGW(TAG, "目标 MJPEG 帧间隔未声明，使用默认 %.2f fps", default_fps);
            return default_fps;
        }
        const uint32_t requested_interval =
            (uint32_t)lroundf(10000000.0f / CAMERA_REQUESTED_FPS);
        if (requested_interval < frame_info->interval_min ||
            requested_interval > frame_info->interval_max) {
            ESP_LOGW(TAG, "目标 MJPEG 不支持 %.1f fps，使用默认 %.2f fps",
                     CAMERA_REQUESTED_FPS, default_fps);
            return default_fps;
        }
        const uint32_t steps =
            (requested_interval - frame_info->interval_min +
             frame_info->interval_step / 2U) / frame_info->interval_step;
        const uint32_t selected_interval =
            frame_info->interval_min + steps * frame_info->interval_step;
        if (selected_interval > frame_info->interval_max) {
            ESP_LOGW(TAG, "目标 MJPEG 无法对齐 %.1f fps，使用默认 %.2f fps",
                     CAMERA_REQUESTED_FPS, default_fps);
            return default_fps;
        }
        return camera_interval_to_fps(selected_interval);
    }

    for (unsigned i = 0; i < frame_info->interval_type; ++i) {
        const float fps = camera_interval_to_fps(frame_info->interval[i]);
        if (fabsf(fps - CAMERA_REQUESTED_FPS) < 0.01f) {
            return fps;
        }
    }
    ESP_LOGW(TAG, "目标 MJPEG 未声明 %.1f fps 离散帧间隔，使用默认 %.2f fps",
             CAMERA_REQUESTED_FPS, default_fps);
    return default_fps;
}

static void camera_log_target_intervals(const uvc_host_frame_info_t *frame_info)
{
    if (frame_info->format != UVC_VS_FORMAT_MJPEG ||
        frame_info->h_res != CAMERA_CAPTURE_WIDTH ||
        frame_info->v_res != CAMERA_CAPTURE_HEIGHT) {
        return;
    }

    if (frame_info->interval_type == 0) {
        ESP_LOGI(TAG,
                 "目标 MJPEG 帧间隔：连续 range=%" PRIu32 "..%" PRIu32
                 " step=%" PRIu32 "（约 %.2f..%.2f fps）",
                 frame_info->interval_min, frame_info->interval_max,
                 frame_info->interval_step,
                 camera_interval_to_fps(frame_info->interval_min),
                 camera_interval_to_fps(frame_info->interval_max));
        return;
    }

    ESP_LOGI(TAG, "目标 MJPEG 帧间隔：离散 count=%u", frame_info->interval_type);
    for (unsigned i = 0; i < frame_info->interval_type; ++i) {
        ESP_LOGI(TAG, "目标 MJPEG interval[%u]=%" PRIu32 "（%.2f fps）",
                 i, frame_info->interval[i],
                 camera_interval_to_fps(frame_info->interval[i]));
    }
}

/* LRCPG720p 已实测支持以下 MJPEG 模式（当前 H.264 链路只吃
 * VIDEO_STREAM_WIDTH×VIDEO_STREAM_HEIGHT）。顺序即轮转时的偏好顺序，
 * 目标分辨率放最前，若设备连接回调暂时没有给出格式列表，使用该列表避免再次传入 0 FPS。 */
static void camera_load_known_formats(void)
{
    static const struct {
        enum uvc_host_stream_format format;
        unsigned width;
        unsigned height;
    } known_sizes[] = {
        {UVC_VS_FORMAT_MJPEG, CAMERA_CAPTURE_WIDTH, CAMERA_CAPTURE_HEIGHT},
        {UVC_VS_FORMAT_MJPEG, 640, 480},
        {UVC_VS_FORMAT_MJPEG, 1280, 720},
        {UVC_VS_FORMAT_MJPEG, 1280, 960},
        {UVC_VS_FORMAT_MJPEG, 800, 600},
        {UVC_VS_FORMAT_MJPEG, 720, 960},
        {UVC_VS_FORMAT_MJPEG, 640, 360},
    };

    s_camera_format_count = sizeof(known_sizes) / sizeof(known_sizes[0]);
    for (size_t i = 0; i < s_camera_format_count; ++i) {
        s_camera_formats[i] = (uvc_host_frame_info_t) {
            .format = known_sizes[i].format,
            .h_res = known_sizes[i].width,
            .v_res = known_sizes[i].height,
            .default_interval = 333333,
        };
    }
    s_camera_device_address = UVC_HOST_ANY_DEV_ADDR;
    s_camera_stream_index = 0;
    ESP_LOGW(TAG, "暂未收到格式回调，使用  已知格式，优先 %ux%u MJPEG 30 FPS",
             CAMERA_CAPTURE_WIDTH, CAMERA_CAPTURE_HEIGHT);
    xEventGroupSetBits(s_camera_events, CAMERA_FORMATS_READY);
}

/* 摄像头接入后读取设备真实声明的格式，不根据产品名称猜测分辨率。 */
static void camera_driver_event_cb(const uvc_host_driver_event_data_t *event, void *ctx)
{
    (void)ctx;
    if (event->type != UVC_HOST_DRIVER_EVENT_DEVICE_CONNECTED) {
        return;
    }

    size_t count = event->device_connected.frame_info_num;
    if (count > MAX_CAMERA_FORMATS) {
        ESP_LOGW(TAG, "摄像头报告 %u 个格式，仅测试前 %u 个",
                 (unsigned)count, MAX_CAMERA_FORMATS);
        count = MAX_CAMERA_FORMATS;
    }

    esp_err_t err = uvc_host_get_frame_list(
        event->device_connected.dev_addr,
        event->device_connected.uvc_stream_index,
        (uvc_host_frame_info_t (*)[])&s_camera_formats,
        &count);
    if (err != ESP_OK || count == 0) {
        ESP_LOGE(TAG, "读取摄像头格式列表失败：%s", esp_err_to_name(err));
        return;
    }

    s_camera_device_address = event->device_connected.dev_addr;
    s_camera_stream_index = event->device_connected.uvc_stream_index;
    s_camera_format_count = count;
    s_saw_real_formats = true;

    /* 将本档位的目标 MJPEG 稳定移动到首位，同时保持其余格式原有顺序。
     * 纯 UVC 诊断只会使用这个首选项，不会在本次运行中轮转。 */
    for (size_t i = 0; i < count; ++i) {
        if (s_camera_formats[i].format == UVC_VS_FORMAT_MJPEG &&
            s_camera_formats[i].h_res == CAMERA_CAPTURE_WIDTH &&
            s_camera_formats[i].v_res == CAMERA_CAPTURE_HEIGHT) {
            uvc_host_frame_info_t preferred = s_camera_formats[i];
            memmove(&s_camera_formats[1], &s_camera_formats[0],
                    i * sizeof(s_camera_formats[0]));
            s_camera_formats[0] = preferred;
            break;
        }
    }

    for (size_t i = 0; i < count; ++i) {
        camera_log_target_intervals(&s_camera_formats[i]);
    }

    unsigned mjpeg_count = 0;
    unsigned yuy2_count = 0;
    unsigned h26x_count = 0;
    for (size_t i = 0; i < count; ++i) {
        if (s_camera_formats[i].format == UVC_VS_FORMAT_MJPEG) {
            mjpeg_count++;
        } else if (s_camera_formats[i].format == UVC_VS_FORMAT_YUY2) {
            yuy2_count++;
        } else if (s_camera_formats[i].format == UVC_VS_FORMAT_H264 ||
                   s_camera_formats[i].format == UVC_VS_FORMAT_H265) {
            h26x_count++;
        }
    }
    ESP_LOGI(TAG,
             "摄像头格式：共 %u 个（MJPEG=%u，YUY2=%u，H26x=%u）；使用 %ux%u MJPEG 30 FPS",
             (unsigned)count, mjpeg_count, yuy2_count, h26x_count,
             CAMERA_CAPTURE_WIDTH, CAMERA_CAPTURE_HEIGHT);

    /* 逐条列出设备声明的模式。汇总行只给个数，没法回答"这摄像头到底支不支持
     * 1920×1080"这类问题，而换分辨率前恰好必须知道。此处 s_camera_formats
     * 已完成"目标分辨率置顶"的重排，所以这里的下标与下面"尝试摄像头格式 N/%u"
     * 的 N 是同一个序号，可以直接对应日志。 */
    for (size_t i = 0; i < count; ++i) {
        ESP_LOGI(TAG, "模式[%u]：%ux%u %s @ %.2f FPS",
                 (unsigned)i + 1,
                 s_camera_formats[i].h_res, s_camera_formats[i].v_res,
                 camera_format_name(s_camera_formats[i].format),
                 camera_interval_to_fps(s_camera_formats[i].default_interval));
    }

    xEventGroupSetBits(s_camera_events, CAMERA_FORMATS_READY);
}

/* 帧回调做统计，并把候选帧复制到独立的 handoff 缓冲池。
 *
 * 这里**刻意不做整帧的 JPEG 结构扫描**。本回调运行在 USB 等时传输的上下文里，
 * 而等时传输既没有 CRC 也没有重传：回调每多占一毫秒，transfer 就晚一毫秒重新
 * 入队（见 uvc_isoc.c 末尾的 usb_host_transfer_submit），那段时间的包直接丢失。
 * 一次全帧扫描约 50KB，在 PSRAM 上要花掉接近一毫秒——正好是这个回调负担不起的
 * 量级。判定“这帧到底能不能解码”的工作因此挪到了编解码任务里做
 * （H.264 链路会在独立编解码任务中继续做结构校验）。
 *
 * 本函数只做判空、判格式、判长度、读两个 SOI 字节和一次有界 memcpy。
 * 复制完成后立即返回 true，UVC 可以复用自己的 frame buffer；编解码任务
 * 只持有独立池中的副本，因此不会因为 H.264 阻塞 UVC 回收。
 *
 * 注意：等时传输丢包的帧即使带有 SOI，也可能在下游结构校验时被丢弃；
 * 那条线索由 uvc_isoc.c 的 packet/FID/EoF 诊断计数给出。 */
static uvc_host_stream_hdl_t s_camera_stream;

static camera_frame_copy_t *camera_acquire_frame_copy(size_t data_len)
{
    if (data_len > CAMERA_HANDOFF_COPY_SIZE) {
        return NULL;
    }
    camera_frame_copy_t *copy = NULL;
    portENTER_CRITICAL(&s_frame_copy_lock);
    for (unsigned i = 0; i < CAMERA_HANDOFF_COPY_COUNT; ++i) {
        if (!s_frame_copies[i].in_use && s_frame_copies[i].data != NULL) {
            s_frame_copies[i].in_use = true;
            s_frame_copies[i].data_len = data_len;
            copy = &s_frame_copies[i];
            break;
        }
    }
    portEXIT_CRITICAL(&s_frame_copy_lock);
    return copy;
}

static void camera_release_frame_copy(void *release_ctx)
{
    camera_frame_copy_t *copy = (camera_frame_copy_t *)release_ctx;
    if (copy == NULL) {
        return;
    }

    portENTER_CRITICAL(&s_frame_copy_lock);
    copy->data_len = 0;
    copy->in_use = false;
    portEXIT_CRITICAL(&s_frame_copy_lock);
}

static bool camera_frame_copy_pool_init(void)
{
    for (unsigned i = 0; i < CAMERA_HANDOFF_COPY_COUNT; ++i) {
        s_frame_copies[i].data = heap_caps_malloc(
            CAMERA_HANDOFF_COPY_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_frame_copies[i].data == NULL) {
            ESP_LOGE(TAG, "分配 MJPEG handoff 复制缓冲失败：%u/%u",
                     i, CAMERA_HANDOFF_COPY_COUNT);
            for (unsigned j = 0; j <= i; ++j) {
                heap_caps_free(s_frame_copies[j].data);
                s_frame_copies[j].data = NULL;
            }
            return false;
        }
        s_frame_copies[i].data_len = 0;
        s_frame_copies[i].in_use = false;
    }
    ESP_LOGI(TAG, "MJPEG handoff 复制池已就绪：%u x %u KB",
             CAMERA_HANDOFF_COPY_COUNT,
             CAMERA_HANDOFF_COPY_SIZE / 1024U);
    return true;
}

static void camera_frame_copy_pool_deinit(void)
{
    for (unsigned i = 0; i < CAMERA_HANDOFF_COPY_COUNT; ++i) {
        heap_caps_free(s_frame_copies[i].data);
        s_frame_copies[i].data = NULL;
        s_frame_copies[i].data_len = 0;
        s_frame_copies[i].in_use = false;
    }
}

/* USB transfer 回调完成复制后只投递副本描述符。跨模块的视频队列操作统一
 * 在独立任务中完成，UVC 原始 frame buffer 不会跨回调生命周期。 */
static void camera_handoff_task(void *arg)
{
    (void)arg;
    camera_frame_handoff_item_t item;

    while (true) {
        if (xQueueReceive(s_camera_handoff_queue, &item, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        /* NULL copy 是关闭 stream 前插入的 FIFO fence。 */
        if (item.copy == NULL) {
            xEventGroupSetBits(s_camera_events, CAMERA_HANDOFF_IDLE);
            continue;
        }

        const bool accepting = __atomic_load_n(&s_camera_handoff_accepting,
                                               __ATOMIC_ACQUIRE);
        bool retained = false;
        if (accepting) {
            retained = video_streamer_submit_jpeg_owned(
                item.copy->data, item.copy->data_len,
                camera_release_frame_copy, item.copy);
        }
        if (!retained) {
            camera_release_frame_copy(item.copy);
        }
    }
}

/* 在关闭 UVC stream 前等待 handoff 任务处理完所有已入队帧。fence 位于
 * 队列尾部，因此即使任务已取出一帧但尚未处理，仍会先完成该帧。 */
static bool camera_handoff_flush(uint32_t timeout_ms)
{
    if (s_camera_handoff_queue == NULL) {
        return true;
    }

    xEventGroupClearBits(s_camera_events, CAMERA_HANDOFF_IDLE);
    const camera_frame_handoff_item_t fence = { 0 };
    if (xQueueSend(s_camera_handoff_queue, &fence, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        ESP_LOGE(TAG, "等待视频 handoff 队列排空失败：无法写入 fence");
        return false;
    }

    const EventBits_t bits = xEventGroupWaitBits(
        s_camera_events, CAMERA_HANDOFF_IDLE, pdFALSE, pdTRUE,
        pdMS_TO_TICKS(timeout_ms));
    if ((bits & CAMERA_HANDOFF_IDLE) == 0) {
        ESP_LOGE(TAG, "等待视频 handoff 任务完成当前帧超时");
        return false;
    }
    return true;
}

static bool camera_frame_cb(const uvc_host_frame_t *frame, void *ctx)
{
#if CAMERA_TEST_PROFILE == CAMERA_TEST_UVC_ONLY
    const uvc_host_frame_info_t *selected = (const uvc_host_frame_info_t *)ctx;
#else
    (void)ctx;
#endif
    const int64_t callback_started_us = esp_timer_get_time();
#define CAMERA_FRAME_CB_RETURN(value) do { \
        camera_record_frame_callback_time(callback_started_us); \
        return (value); \
    } while (0)
    const bool valid_frame = frame->data != NULL && frame->data_len > 0;
#if CAMERA_TEST_PROFILE == CAMERA_TEST_UVC_ONLY
    const bool target_frame = valid_frame &&
                              selected != NULL &&
                              frame->vs_format.format == selected->format &&
                              frame->vs_format.h_res == selected->h_res &&
                              frame->vs_format.v_res == selected->v_res;
#else
    const bool target_frame = valid_frame &&
                              frame->vs_format.format == UVC_VS_FORMAT_MJPEG &&
                              frame->vs_format.h_res == CAMERA_CAPTURE_WIDTH &&
                              frame->vs_format.v_res == CAMERA_CAPTURE_HEIGHT;
#endif
    const bool oversized_frame = target_frame &&
                                 frame->data_len > VIDEO_STREAM_JPEG_MAX_SIZE;
    const bool has_soi = target_frame && frame->data_len >= 2 &&
                         frame->data[0] == 0xff && frame->data[1] == 0xd8;
    const bool candidate_frame = target_frame && !oversized_frame && has_soi;

    portENTER_CRITICAL(&s_stats_lock);
    if (valid_frame) {
        s_stats.frames++;
        s_stats.bytes += frame->data_len;
        s_stats.last_size = frame->data_len;
        s_stats.width = frame->vs_format.h_res;
        s_stats.height = frame->vs_format.v_res;
        s_stats.format = frame->vs_format.format;
        if (target_frame) {
            s_stats.target_frames++;
            if (candidate_frame) {
                s_stats.candidate_frames++;
            }
            if (!has_soi) {
                s_stats.missing_soi_frames++;
            }
            if (oversized_frame) {
                s_stats.oversized_frames++;
            }
        }
    } else {
        s_stats.empty_frames++;
    }
    portEXIT_CRITICAL(&s_stats_lock);

    /* 复制池中的缓冲由 handoff/编解码链路异步释放；UVC 原始帧始终由回调
     * 返回 true 立即归还。 */
#if !TP_HAS(HANDOFF)
    /* 路径 1~4 只测 UVC 组帧，不启动 JPEG/H.264/WebSocket，也不把帧复制给
     * 下游，保证 USB 驱动可以立即回收每一帧。 */
    if (candidate_frame) {
        CAMERA_FRAME_CB_RETURN(true);
    }
#endif
    if (candidate_frame) {
        if (!__atomic_load_n(&s_camera_handoff_accepting, __ATOMIC_ACQUIRE)) {
            CAMERA_FRAME_CB_RETURN(true);
        }
        camera_frame_copy_t *copy = camera_acquire_frame_copy(frame->data_len);
        if (copy == NULL) {
            portENTER_CRITICAL(&s_stats_lock);
            s_stats.handoff_rejected++;
            portEXIT_CRITICAL(&s_stats_lock);
            CAMERA_FRAME_CB_RETURN(true);
        }

        /* 复制后立即返回 true，UVC 可以马上复用其 frame buffer；复制的
         * 缓冲由 handoff/编解码链路独立持有。memcpy 的耗时会被 callback
         * 统计覆盖，用于确认它没有重新成为 USB 等时瓶颈。 */
        memcpy(copy->data, frame->data, frame->data_len);
        const camera_frame_handoff_item_t item = {
            .copy = copy,
        };
        if (xQueueSend(s_camera_handoff_queue, &item, 0) == pdTRUE) {
            /* handoff 任务现在拥有 copy；UVC 帧本身可立即复用。 */
            CAMERA_FRAME_CB_RETURN(true);
        }

        portENTER_CRITICAL(&s_stats_lock);
        s_stats.handoff_rejected++;
        portEXIT_CRITICAL(&s_stats_lock);
        camera_release_frame_copy(copy);
    }
    CAMERA_FRAME_CB_RETURN(true);
#undef CAMERA_FRAME_CB_RETURN
}

/* 流事件只通知主任务，由主任务统一停止并关闭视频流。 */
static void camera_stream_event_cb(const uvc_host_stream_event_data_t *event, void *ctx)
{
    (void)ctx;
    switch (event->type) {
    case UVC_HOST_DEVICE_DISCONNECTED:
        ESP_LOGW(TAG, "摄像头已断开");
        s_camera_format_count = 0;
        xEventGroupClearBits(s_camera_events, CAMERA_FORMATS_READY);
        xEventGroupSetBits(s_camera_events, CAMERA_DISCONNECTED);
        break;
    case UVC_HOST_TRANSFER_ERROR:
        ESP_LOGE(TAG, "USB 传输错误：%s", esp_err_to_name(event->transfer_error.error));
        xEventGroupSetBits(s_camera_events, CAMERA_ERROR);
        break;
    case UVC_HOST_FRAME_BUFFER_OVERFLOW:
        ESP_LOGW(TAG, "摄像头帧缓冲区溢出");
        break;
    case UVC_HOST_FRAME_BUFFER_UNDERFLOW:
        ESP_LOGW(TAG, "没有可用的摄像头帧缓冲区");
        break;
    default:
        break;
    }
}

/* 持续处理 USB Host 库事件，摄像头拔出后才能正确回收设备资源。
 * 这个任务在 USB Host 重建前必须先退出，不能在 usb_host_uninstall()
 * 之后继续调用 usb_host_lib_handle_events()。 */
static void usb_events_task(void *arg)
{
    (void)arg;
    s_usb_events_task = xTaskGetCurrentTaskHandle();
    while (s_usb_events_running) {
        uint32_t event_flags = 0;
        esp_err_t err = usb_host_lib_handle_events(pdMS_TO_TICKS(100), &event_flags);
        if (err == ESP_ERR_INVALID_STATE) {
            break;
        }
        if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
            ESP_LOGW(TAG, "USB Host 事件处理失败：%s", esp_err_to_name(err));
        }
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            usb_host_device_free_all();
        }
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) {
            xEventGroupSetBits(s_camera_events, CAMERA_USB_ALL_FREE);
        }
    }
    s_usb_events_task = NULL;
    xEventGroupSetBits(s_camera_events, CAMERA_USB_EVENTS_EXITED);
    vTaskDelete(NULL);
}

static const usb_host_config_t s_usb_host_config = {
    .skip_phy_setup = false,
    /* Keep the root port controllable so a wedged ISOC ring can be forced
     * through the USB Host disconnect path.  This is the only public IDF
     * mechanism that can make already-submitted ISOC transfers complete when
     * their callbacks have stopped arriving. */
    .root_port_unpowered = true,
    .intr_flags = ESP_INTR_FLAG_LEVEL1,
    .peripheral_map = BIT0,
};

static const uvc_host_driver_config_t s_uvc_driver_config = {
    .driver_task_stack_size = 4096,
    .driver_task_priority = CAMERA_UVC_DRIVER_PRIORITY,
    /* USB/UVC 与 H.264 编解码隔离：USB/UVC 固定 CPU0，编码任务固定 CPU1。 */
    .xCoreID = CAMERA_USB_CORE,
    .create_background_task = true,
    .event_cb = camera_driver_event_cb,
    .user_ctx = NULL,
};

static void camera_usb_events_stop(void)
{
    if (!s_usb_events_running) {
        return;
    }
    s_usb_events_running = false;
    /* Wake the event task if it is blocked in usb_host_lib_handle_events(). */
    usb_host_lib_unblock();
    xEventGroupWaitBits(s_camera_events, CAMERA_USB_EVENTS_EXITED,
                        pdFALSE, pdTRUE, pdMS_TO_TICKS(1000));
}

/* camera_usb_events_task must be the only task calling
 * usb_host_lib_handle_events(). Once it has exited, take ownership of event
 * dispatch here and drain a few consecutive idle rounds before the Host
 * object can be destroyed. Root-port power-off and HCD recovery can enqueue a
 * processing request after ALL_FREE was reported; uninstalling immediately in
 * that window lets the ISR call proc_req_callback() with a freed Host object. */
static esp_err_t camera_usb_events_drain_pending(void)
{
    unsigned idle_rounds = 0;
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(1000);

    while ((int32_t)(xTaskGetTickCount() - deadline) < 0) {
        uint32_t event_flags = 0;
        const esp_err_t err = usb_host_lib_handle_events(0, &event_flags);
        if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
            return err;
        }
        if (event_flags == 0) {
            if (++idle_rounds >= 3) {
                return ESP_OK;
            }
            vTaskDelay(pdMS_TO_TICKS(1));
        } else {
            idle_rounds = 0;
        }
    }
    return ESP_ERR_TIMEOUT;
}

static esp_err_t camera_usb_stack_install(void)
{
    esp_err_t err = usb_host_install(&s_usb_host_config);
    if (err != ESP_OK) {
        return err;
    }

    xEventGroupClearBits(s_camera_events, CAMERA_USB_ALL_FREE | CAMERA_USB_EVENTS_EXITED);
    s_usb_events_running = true;
    if (xTaskCreatePinnedToCore(usb_events_task, "usb_events", 4096, NULL,
                                CAMERA_USB_EVENTS_PRIORITY,
                                &s_usb_events_task, CAMERA_USB_CORE) != pdPASS) {
        s_usb_events_running = false;
        usb_host_uninstall();
        return ESP_ERR_NO_MEM;
    }

    err = uvc_host_install(&s_uvc_driver_config);
    if (err != ESP_OK) {
        camera_usb_events_stop();
        usb_host_uninstall();
        return err;
    }

    /* UVC must be installed before power is applied, otherwise the first
     * enumeration event can arrive before the UVC client is registered. */
    err = usb_host_lib_set_root_port_power(true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "开启 USB 根端口失败：%s", esp_err_to_name(err));
        uvc_host_uninstall();
        camera_usb_events_stop();
        usb_host_uninstall();
        return err;
    }
    return ESP_OK;
}

/* A normal UVC stop can only wait for the HCD to complete submitted ISOC
 * transfers.  When the camera/HCD gets stuck, wait_idle() reports
 * submitted=N, callbacks=0 forever.  Power-cycling the root port produces a
 * real USB disconnect, which makes the host cancel/complete those transfers;
 * only after that drain is complete may the UVC objects be closed/freed. */
static esp_err_t camera_usb_force_disconnect(void)
{
    ESP_LOGW(TAG, "UVC transfer 长时间无完成回调，关闭 USB 根端口触发安全断开");
    const esp_err_t err = usb_host_lib_set_root_port_power(false);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "关闭 USB 根端口失败：%s", esp_err_to_name(err));
        return err;
    }
    /* Give the USB Host library and UVC client task time to dispatch the
     * disconnect and transfer completion events before wait_idle(). */
    vTaskDelay(pdMS_TO_TICKS(250));
    usb_host_lib_unblock();
    return ESP_OK;
}

/* Stop failure means the UVC stream may have left the USB controller in a
 * stale alternate setting. Once the stream is safely closed, rebuild both
 * layers so the next open starts with a fresh client/device state. */
static esp_err_t camera_usb_stack_rebuild(void)
{
    ESP_LOGW(TAG, "停止视频流失败，执行 USB Host/UVC 完整重建");
    s_camera_format_count = 0;
    s_saw_real_formats = false;
    xEventGroupClearBits(s_camera_events, CAMERA_FORMATS_READY | CAMERA_DISCONNECTED | CAMERA_ERROR);

    esp_err_t err = uvc_host_uninstall();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "卸载 UVC 驱动失败：%s", esp_err_to_name(err));
        return err;
    }

    xEventGroupClearBits(s_camera_events, CAMERA_USB_ALL_FREE);
    err = usb_host_device_free_all();
    if (err == ESP_ERR_NOT_FINISHED) {
        EventBits_t bits = xEventGroupWaitBits(
            s_camera_events, CAMERA_USB_ALL_FREE, pdFALSE, pdTRUE, pdMS_TO_TICKS(1000));
        if (!(bits & CAMERA_USB_ALL_FREE)) {
            ESP_LOGE(TAG, "等待 USB 设备释放超时");
            return ESP_ERR_TIMEOUT;
        }
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "标记 USB 设备释放失败：%s", esp_err_to_name(err));
        return err;
    }

    camera_usb_events_stop();
    err = camera_usb_events_drain_pending();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "USB Host 事件排空超时，拒绝卸载 Host：%s", esp_err_to_name(err));
        return err;
    }
    err = usb_host_uninstall();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "卸载 USB Host 失败：%s", esp_err_to_name(err));
        return err;
    }

    vTaskDelay(pdMS_TO_TICKS(100));
    err = camera_usb_stack_install();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "USB Host/UVC 完整重建完成，等待摄像头重新枚举");
    }
    return err;
}

/* 独立的开始和停止接口，方便后续拆分正式摄像头服务模块。 */
static esp_err_t camera_start(uvc_host_stream_hdl_t stream)
{
    return uvc_host_stream_start(stream);
}

static esp_err_t camera_stop(uvc_host_stream_hdl_t stream)
{
    return uvc_host_stream_stop(stream);
}

void camera_driver_run(void)
{
    /* 完整基线阶段关闭 UVC 组件的协商/包级 INFO、DEBUG 日志，保留 WARN/ERROR
     * 和本驱动每 5 秒的帧率统计，避免历史测试信息淹没关键指标。 */
    esp_log_level_set("uvc", ESP_LOG_WARN);
    esp_log_level_set("uvc-control", ESP_LOG_WARN);
    esp_log_level_set("uvc-desc", ESP_LOG_WARN);
    esp_log_level_set("uvc-isoc", ESP_LOG_WARN);

    ESP_LOGI(TAG, "初始化 LRCPG720p USB 摄像头驱动");
    ESP_LOGI(TAG, "使用 ESP32-P4 高速 USB Host，当前 PSRAM 可用：%u 字节",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    /* UVC-only 隔离阶段不初始化视频流水线，避免 H.264/WebSocket 影响 USB。 */
#if !TP_HAS(HANDOFF)
    ESP_LOGI(TAG, "测试档位=%d（%s）：跳过 H.264/JPEG/WebSocket 视频流水线",
             CAMERA_TEST_PROFILE, test_profile_name());
#else
    /* 实时流初始化失败不影响本地 UVC 采集，错误会保留在串口日志中。 */
    esp_err_t streamer_error = video_streamer_init(NULL);
    if (streamer_error != ESP_OK) {
        ESP_LOGE(TAG, "初始化 H.264 实时流失败：%s",
                 esp_err_to_name(streamer_error));
    } else {
        /* 先保留 UVC 采集，关闭 H.264 编码和视频上传，便于单独验证音频链路。 */
        video_streamer_set_enabled(CAMERA_VIDEO_STREAM_ENABLED != 0);
        ESP_LOGI(TAG, "视频传输%s（UVC 采集仍保持运行）",
                 CAMERA_VIDEO_STREAM_ENABLED ? "已启用" : "已关闭");
    }
#endif

    s_camera_events = xEventGroupCreate();
    assert(s_camera_events != NULL);

    /* UVC 回调不再借用摄像头帧，而是复制到独立池；池的生命周期覆盖
     * 摄像头重启，避免编码任务尚未结束时复用或释放其输入缓冲。 */
    if (!camera_frame_copy_pool_init()) {
        return;
    }

    s_camera_handoff_queue = xQueueCreate(
        CAMERA_HANDOFF_QUEUE_LENGTH, sizeof(camera_frame_handoff_item_t));
    if (s_camera_handoff_queue == NULL ||
        xTaskCreatePinnedToCore(camera_handoff_task, "camera_handoff",
                                CAMERA_HANDOFF_TASK_STACK, NULL,
                                CAMERA_HANDOFF_TASK_PRIORITY, NULL,
                                CAMERA_USB_CORE) != pdPASS) {
        ESP_LOGE(TAG, "创建 camera handoff 任务失败");
        if (s_camera_handoff_queue != NULL) {
            vQueueDelete(s_camera_handoff_queue);
            s_camera_handoff_queue = NULL;
        }
        camera_frame_copy_pool_deinit();
        return;
    }

    /* ESP32-P4 外设映射 BIT0 对应内部高速 USB PHY。 */
    ESP_ERROR_CHECK(camera_usb_stack_install());
    /* 新摄像头上电后需要时间完成传感器和 UVC 状态机初始化。 */
    ESP_LOGI(TAG, "等待摄像头上电稳定 %u ms", CAMERA_POWER_ON_DELAY_MS);
    vTaskDelay(pdMS_TO_TICKS(CAMERA_POWER_ON_DELAY_MS));

    unsigned candidate = 0;
    unsigned same_format_retries = 0;
    while (true) {
        ESP_LOGI(TAG, "等待 UVC 摄像头及格式列表");
        EventBits_t ready = xEventGroupWaitBits(s_camera_events, CAMERA_FORMATS_READY,
                                                pdFALSE, pdTRUE, pdMS_TO_TICKS(3000));
        if (!(ready & CAMERA_FORMATS_READY) && !s_saw_real_formats) {
            camera_load_known_formats();
        }
        if (s_camera_format_count == 0) {
            continue;
        }

#if CAMERA_TEST_PROFILE == CAMERA_TEST_UVC_ONLY
        /* 冷启动测试只选择编译期指定的 MJPEG 模式，找不到就终止本次测试，
         * 绝不在同一次枚举中退到其他分辨率。 */
        candidate = (unsigned)s_camera_format_count;
        for (size_t i = 0; i < s_camera_format_count; ++i) {
            if (s_camera_formats[i].format == UVC_VS_FORMAT_MJPEG &&
                s_camera_formats[i].h_res == CAMERA_CAPTURE_WIDTH &&
                s_camera_formats[i].v_res == CAMERA_CAPTURE_HEIGHT) {
                candidate = (unsigned)i;
                break;
            }
        }
        if (candidate == s_camera_format_count) {
            ESP_LOGE(TAG, "摄像头未声明固定测试模式 %ux%u MJPEG@30，本次冷启动测试结束",
                     CAMERA_CAPTURE_WIDTH, CAMERA_CAPTURE_HEIGHT);
            return;
        }
#else
        candidate %= s_camera_format_count;
#endif
        const uvc_host_frame_info_t selected = s_camera_formats[candidate];
        const float stream_fps = camera_select_stream_fps(&selected);
        xEventGroupClearBits(s_camera_events, CAMERA_DISCONNECTED | CAMERA_ERROR);

        uvc_host_stream_config_t stream_config = {
            .event_cb = camera_stream_event_cb,
            .frame_cb = camera_frame_cb,
            .user_ctx = (void *)&selected,
            .usb = {
                .dev_addr = s_camera_device_address,
                .vid = UVC_HOST_ANY_VID,
                .pid = UVC_HOST_ANY_PID,
                .uvc_stream_index = s_camera_stream_index,
            },
            .vs_format = {
                .h_res = selected.h_res,
                .v_res = selected.v_res,
                .fps = stream_fps,
                .format = selected.format,
            },
            .advanced = {
                /* 摄像头协商的 dwMaxVideoFrameSize 偏小会截断复杂画面，显式按
                 * 最大分辨率预留（见 VIDEO_STREAM_JPEG_MAX_SIZE），并将大块 DMA
                 * 缓冲放入 PSRAM。
                 *
                 * 注意 UVC_HOST_FRAME_BUFFER_OVERFLOW 不等于"这帧比缓冲大"：
                 * 它在 uvc_frame_add_data() 里由「已累积 + 本 URB > frame_size」
                 * 触发，驱动在两次 SoF 之间认不出 EoF 就会一路累积到爆。
                 * 也就是说这条事件指向**帧重组失步**，把 frame_size 调大只会
                 * 让失步时攒下更多垃圾数据。真正该动的是 URB 参数。
                 * 3 个帧缓冲是连续取流的下限+1；其中最多 2 个会被编解码任务
                 * 暂时借用，剩下 1 个继续接收下一帧。 */
                .frame_size = VIDEO_STREAM_JPEG_MAX_SIZE,
                .number_of_frame_buffers = CAMERA_FRAME_BUFFER_COUNT,
                /* URB 环深和内存位置两个变量都影响丢帧（2026-09-12 定论）。
                 *
                 * URB 落 PSRAM 时，USB HCD 的 DMA 与 H.264/JPEG 解码争 PSRAM
                 * 仲裁，ISOC 回调最长被推迟（callback_gap_max 从 9 ms 涨到
                 * 22 ms），期间等时包被主机标 SKIPPED，MJPEG 是熵编码，丢一个包
                 * 这一帧从断点往后全错位，只能整帧丢弃（skipped 与 dropped 严格
                 * 1:1）。UVC 等时传输没有重传，丢一个微帧就毁一整帧。
                 *
                 * 环深：96×32 KB → 8×16 KB，把 PSRAM 下的丢帧从 ~35% 压到
                 * ~10%。不要再把环深当免维护窗口加大，那只会多烧 PSRAM。
                 *
                 * 内存位置：8×16 KB 落在内部 RAM 时 codec-only 档位能到 0%
                 * 丢帧，但**不能推广到完整档位**——8 × 18432 B = 144 KiB，而内部
                 * DMA 池只有 146 KiB，完整档位的 WiFi(SDIO)/LVGL/LCD SPI 会先
                 * 占用同一个池，分配必然失败；失败还会被组件的 double-free
                 * 放大成开机重启循环。所以 sdkconfig 里
                 * CONFIG_USB_HOST_DWC_DMA_CAP_MEMORY_IN_PSRAM 必须为 y。
                 * 完整档位的代价是丢帧 2.6%~32%（均值 ~16%），H.264 只有
                 * 15~20 FPS，20 FPS 目标尚未达成。详见
                 * VIDEO_20FPS_VALIDATION.md 与 PROJECT_HANDOFF.md 附录 A。 */
                .number_of_urbs = CAMERA_USB_URB_COUNT,
                .urb_size = CAMERA_USB_URB_SIZE,
                .frame_heap_caps = MALLOC_CAP_SPIRAM,
            },
        };

        ESP_LOGI(TAG, "尝试摄像头格式 %u/%u：%ux%u %s @ %.2f FPS",
                 candidate + 1, (unsigned)s_camera_format_count,
                 stream_config.vs_format.h_res, stream_config.vs_format.v_res,
                 camera_format_name(stream_config.vs_format.format),
                 stream_config.vs_format.fps);

        uvc_host_stream_hdl_t stream = NULL;
        esp_err_t err = uvc_host_stream_open(&stream_config, pdMS_TO_TICKS(5000), &stream);
        if (err != ESP_OK) {
#if CAMERA_TEST_PROFILE == CAMERA_TEST_UVC_ONLY
            ESP_LOGE(TAG, "固定模式打开失败：%s；本次冷启动测试结束，请复位后重试",
                     esp_err_to_name(err));
            return;
#else
            ESP_LOGW(TAG, "打开视频流失败：%s，尝试设备声明的下一个格式", esp_err_to_name(err));
            same_format_retries = 0;
            candidate = (candidate + 1) % s_camera_format_count;
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
#endif
        }

        s_camera_stream = stream;

        portENTER_CRITICAL(&s_stats_lock);
        memset(&s_stats, 0, sizeof(s_stats));
        portEXIT_CRITICAL(&s_stats_lock);

        /* 将 UVC 诊断窗口与本次视频流绑定，后面的报告均为本次流的增量。 */
        uvc_isoc_diag_reset();
        bool first_frame_ok = false;
        __atomic_store_n(&s_camera_handoff_accepting, true, __ATOMIC_RELEASE);
        err = camera_start(stream);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Stream started；等待首个有效 MJPEG 帧（超时 %u ms）",
                     CAMERA_FIRST_FRAME_TIMEOUT_MS);
            uint32_t first_frame_elapsed_ms = 0;
            first_frame_ok = camera_wait_first_frame(
                CAMERA_FIRST_FRAME_TIMEOUT_MS, &first_frame_elapsed_ms);
            if (first_frame_ok) {
                ESP_LOGI(TAG, "UVC 本次启动成功：首帧耗时 %u ms",
                         first_frame_elapsed_ms);
            } else {
                /* 首帧失败只保留一条摘要；详细 UVC 测试计数暂不刷屏。 */
                ESP_LOGW(TAG, "UVC 本次启动失败：%u ms 内没有有效帧",
                         CAMERA_FIRST_FRAME_TIMEOUT_MS);

#if CAMERA_TEST_PROFILE != CAMERA_TEST_UVC_ONLY
                /* 生产档位保留原有自动恢复；纯 UVC 冷启动测试禁止轮转。 */
                candidate = (candidate + 1) % s_camera_format_count;
                same_format_retries = 0;
                ESP_LOGW(TAG, "首帧超时，下次改试摄像头格式 %u/%u",
                         candidate + 1, (unsigned)s_camera_format_count);
#else
                ESP_LOGW(TAG, "固定模式测试失败，本次运行不再切换格式；请断电/复位后重试");
#endif
            }

            /* 首帧等待期间产生的帧属于启动过程；后续 5 秒统计从首帧之后开始。 */
            camera_stats_t previous = camera_stats_snapshot();
            uvc_isoc_diag_stats_t diag_previous = {0};
            uvc_isoc_diag_get(&diag_previous);
            int64_t last_report = esp_timer_get_time();
            unsigned stalls = 0;

            while (first_frame_ok) {
                EventBits_t bits = xEventGroupWaitBits(
                    s_camera_events, CAMERA_DISCONNECTED | CAMERA_ERROR,
                    pdFALSE, pdFALSE, pdMS_TO_TICKS(CAMERA_REPORT_INTERVAL_MS));
                if (bits & (CAMERA_DISCONNECTED | CAMERA_ERROR)) {
                    break;
                }

                camera_stats_t current;
                portENTER_CRITICAL(&s_stats_lock);
                current = s_stats;
                portEXIT_CRITICAL(&s_stats_lock);

                int64_t now = esp_timer_get_time();
                double seconds = (now - last_report) / 1000000.0;
                /* 名字里必须带 delta：外层 camera_driver_run 已经有一个同名的
                 * `candidate`（格式轮转下标），内层若重名会把它遮住，下面
                 * “停顿后切下一个格式”的两个分支就会改到错误的变量上。 */
                uint32_t candidate_delta =
                    current.candidate_frames - previous.candidate_frames;
                uint32_t target_delta = current.target_frames - previous.target_frames;
                 uint32_t callback_delta = current.callback_count - previous.callback_count;
                 uint64_t callback_us_delta = current.callback_us - previous.callback_us;
                 uint32_t callback_max_us = current.callback_max_us;
                 uint32_t handoff_rejected_delta =
                     current.handoff_rejected - previous.handoff_rejected;
                uvc_isoc_diag_stats_t diag_current;
                uvc_isoc_diag_get(&diag_current);
                uint32_t dropped_delta = diag_current.frame_dropped - diag_previous.frame_dropped;
                const uint32_t uvc_total_delta = target_delta + dropped_delta;
                const double uvc_drop_percent = uvc_total_delta > 0 ?
                    dropped_delta * 100.0 / uvc_total_delta : 0.0;

                ESP_LOGI(TAG,
                         "[VIDEO] UVC requested : %.1f fps | UVC complete : %.1f fps | "
                         "UVC drop : %.1f%% | callback avg/max : %.2f/%.2f ms | "
                         "URB : %u x %u KB | buffers : %u",
                         stream_config.vs_format.fps,
                         target_delta / seconds,
                         uvc_drop_percent,
                         callback_delta > 0 ? callback_us_delta / callback_delta / 1000.0 : 0.0,
                         callback_max_us / 1000.0,
                         CAMERA_USB_URB_COUNT, CAMERA_USB_URB_SIZE / 1024U,
                         CAMERA_FRAME_BUFFER_COUNT);
                 ESP_LOGI(TAG, "[VIDEO] handoff rejected=%" PRIu32,
                          handoff_rejected_delta);
                /* 以“候选帧”判定停顿：非目标分辨率 MJPEG（如 1280×960）即便一直在
                 * 收帧，也不能喂 H.264，继续驻留只会无声无息没有视频，故同样计入
                 * 停顿轮转。这里只能数候选帧——结构校验已经搬到下游任务，本函数
                 * 看不到校验结果。要区分“摄像头根本没给帧”和“给了但全损坏”，
                 * 去看 VIDEO_STREAM 那条日志里的“输入损坏”。 */
                if (candidate_delta) {
                    stalls = 0;
                    /* 当前格式已经真正收到可编码帧，下次停顿允许重新做一次原地恢复。 */
                    same_format_retries = 0;
                } else {
                    stalls++;
                }
                previous = current;
                diag_previous = diag_current;
                last_report = now;
                if (stalls >= CAMERA_STALL_LIMIT) {
                    const bool preferred_mjpeg =
                        selected.format == UVC_VS_FORMAT_MJPEG &&
                        selected.h_res == CAMERA_CAPTURE_WIDTH &&
                        selected.v_res == CAMERA_CAPTURE_HEIGHT;
                    if (preferred_mjpeg &&
                        same_format_retries < CAMERA_SAME_FORMAT_RETRY_LIMIT) {
                        same_format_retries++;
                        ESP_LOGW(TAG,
                                 "连续 10 秒没有可编码帧：关闭并原地重试 %ux%u MJPEG（%u/%u）",
                                 CAMERA_CAPTURE_WIDTH, CAMERA_CAPTURE_HEIGHT,
                                 same_format_retries, CAMERA_SAME_FORMAT_RETRY_LIMIT);
                        /* candidate 保持不变；下方完成 stop/close 后重新 open/start 同一格式。 */
                    } else if (preferred_mjpeg) {
                        ESP_LOGW(TAG,
                                 "%ux%u MJPEG 原地重试仍未恢复，改试设备声明的下一个格式",
                                 CAMERA_CAPTURE_WIDTH, CAMERA_CAPTURE_HEIGHT);
                        same_format_retries = 0;
                        candidate = (candidate + 1) % s_camera_format_count;
                    } else {
                        ESP_LOGW(TAG,
                                 "当前格式连续 10 秒没有可编码帧，继续轮转下一个格式");
                        same_format_retries = 0;
                        candidate = (candidate + 1) % s_camera_format_count;
                    }
                    break;
                }
            }
        } else {
            ESP_LOGE(TAG, "UVC 本次启动失败：uvc_stream_start 返回 %s",
                     esp_err_to_name(err));
            same_format_retries = 0;
            candidate = (candidate + 1) % s_camera_format_count;
        }

        /* 摄像头已拔出时驱动会处理停止；其他情况下由主任务主动停止。 */
        __atomic_store_n(&s_camera_handoff_accepting, false, __ATOMIC_RELEASE);
        const bool camera_disconnected =
            (xEventGroupGetBits(s_camera_events) & CAMERA_DISCONNECTED) != 0;
        /* 首帧超时不是格式不支持：旧流必须安全排空后重建整个 USB/UVC 栈，
         * 不要只轮换视频格式。 */
        bool rebuild_usb_stack =
#if CAMERA_TEST_PROFILE == CAMERA_TEST_UVC_ONLY
            false;
#else
            (err == ESP_OK && !first_frame_ok);
#endif
        if (!camera_disconnected) {
            esp_err_t stop_error = camera_stop(stream);
            if (stop_error != ESP_OK) {
                ESP_LOGW(TAG, "停止视频流失败：%s", esp_err_to_name(stop_error));
                rebuild_usb_stack = true;
                /* Do not close the stream yet.  First force the root-port
                 * disconnect, then wait for every submitted transfer to be
                 * completed by its callback. */
                if (camera_usb_force_disconnect() != ESP_OK) {
                    ESP_LOGE(TAG, "无法触发 USB 安全断开，拒绝释放活动 URB 并停止摄像头任务");
                    return;
                }
            }
        }

        if (rebuild_usb_stack) {
            const esp_err_t drain_error = uvc_host_stream_wait_idle(
                stream, pdMS_TO_TICKS(3000));
            if (drain_error != ESP_OK) {
                ESP_LOGE(TAG,
                         "USB 根端口断开后 URB 仍未完成，拒绝关闭/释放：%s；请检查 USB Host/HCD 或重新上电",
                         esp_err_to_name(drain_error));
                return;
            }
            ESP_LOGI(TAG, "所有 UVC transfer/callback 已完成，允许关闭旧流");
        }

        /* stop/断开之后不再接受新的帧。FIFO fence 确认 handoff 任务已处理
         * 所有借用帧，之后才能关闭 stream，避免 release 使用失效句柄。 */
        if (!camera_handoff_flush(3000)) {
            ESP_LOGE(TAG, "视频 handoff 未排空，拒绝关闭 UVC stream");
            return;
        }

        /* 编解码任务可能仍在读复制池中的最后一帧，但它已经不再引用
         * UVC stream/frame；因此不必阻塞 USB stream 的关闭。复制池保持到
         * camera_driver_run 结束，异步 release 只会归还池槽。 */
        unsigned copies_in_use = 0;
        portENTER_CRITICAL(&s_frame_copy_lock);
        for (unsigned i = 0; i < CAMERA_HANDOFF_COPY_COUNT; ++i) {
            copies_in_use += s_frame_copies[i].in_use ? 1U : 0U;
        }
        portEXIT_CRITICAL(&s_frame_copy_lock);
        if (copies_in_use != 0) {
            ESP_LOGI(TAG, "USB stream 关闭时仍有 %u 个独立 MJPEG 复制槽由编解码任务使用",
                     copies_in_use);
        }

        if (rebuild_usb_stack) {
            const esp_err_t reset_error = uvc_host_stream_reset_frame_state(stream);
            if (reset_error != ESP_OK) {
                ESP_LOGE(TAG, "清空 UVC 帧状态/缓冲区失败：%s；拒绝关闭旧流",
                         esp_err_to_name(reset_error));
                return;
            }
            ESP_LOGI(TAG, "UVC 帧状态和已归还帧缓冲已清空，准备完整 USB/UVC 重建");
        }

        /* 摄像头拔出时设备句柄已失效，close 可能失败：失败即终止摄像头任务，
         * 需重新上电恢复。热拔插支持不在本需求范围内。 */
        esp_err_t close_error = uvc_host_stream_close(stream);
        s_camera_stream = NULL;
        if (close_error != ESP_OK) {
            ESP_LOGE(TAG, "关闭视频流失败：%s；仍有 URB/回调未退出，拒绝释放并停止自动轮换",
                     esp_err_to_name(close_error));
            return;
        }
#if CAMERA_TEST_PROFILE == CAMERA_TEST_UVC_ONLY
        ESP_LOGI(TAG, "固定 %ux%u MJPEG@30 冷启动测试结束；不会在本次运行中切换格式",
                 CAMERA_CAPTURE_WIDTH, CAMERA_CAPTURE_HEIGHT);
        return;
#endif
        if (rebuild_usb_stack) {
            esp_err_t rebuild_error = camera_usb_stack_rebuild();
            if (rebuild_error != ESP_OK) {
                ESP_LOGE(TAG, "USB Host/UVC 重建失败：%s，请重新上电摄像头或复位开发板",
                         esp_err_to_name(rebuild_error));
                return;
            }
            /* candidate 已在失败或停顿分支中更新，重建后继续测试下一格式。 */
            same_format_retries = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
