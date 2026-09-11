#include <assert.h>
#include <inttypes.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
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
#define CAMERA_LOAN_COUNT 2
#define CAMERA_USB_ALL_FREE        BIT3
#define CAMERA_USB_EVENTS_EXITED   BIT4
#define CAMERA_USB_EVENTS_PRIORITY 19
#define CAMERA_UVC_DRIVER_PRIORITY 20
/* URB 对照测试当前档位：64 为已完成基线，下一轮使用 96，再切换到 128。
 * 只改变这个数量，其他摄像头、编码和网络参数保持不变。 */
#define CAMERA_USB_URB_COUNT        96
#define CAMERA_USB_URB_SIZE         (32U * 1024U)
#define CAMERA_FRAME_BUFFER_COUNT   3
#define CAMERA_REPORT_INTERVAL_MS   5000

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

typedef struct {
    uvc_host_stream_hdl_t stream;
    uvc_host_frame_t *frame;
    bool in_use;
} camera_frame_loan_t;

static camera_frame_loan_t s_frame_loans[CAMERA_LOAN_COUNT];
static portMUX_TYPE s_frame_loan_lock = portMUX_INITIALIZER_UNLOCKED;

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
        {UVC_VS_FORMAT_MJPEG, VIDEO_STREAM_WIDTH, VIDEO_STREAM_HEIGHT},
        {UVC_VS_FORMAT_MJPEG, 640, 480},
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
    ESP_LOGW(TAG, "暂未收到格式回调，使用 LRCPG720p 已知格式，优先 %ux%u MJPEG 30 FPS",
             VIDEO_STREAM_WIDTH, VIDEO_STREAM_HEIGHT);
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

    /* 优先 VIDEO_STREAM_WIDTH×VIDEO_STREAM_HEIGHT MJPEG：冷启动时该模式
     * 已实机跑通完整视频链路。热复位后可能出现 0 帧，主循环会先原地重开
     * 一次再决定是否轮转。 */
    for (size_t i = 0; i < count; ++i) {
        if (s_camera_formats[i].format == UVC_VS_FORMAT_MJPEG &&
            s_camera_formats[i].h_res == VIDEO_STREAM_WIDTH &&
            s_camera_formats[i].v_res == VIDEO_STREAM_HEIGHT) {
            uvc_host_frame_info_t preferred = s_camera_formats[i];
            s_camera_formats[i] = s_camera_formats[0];
            s_camera_formats[0] = preferred;
            break;
        }
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
             VIDEO_STREAM_WIDTH, VIDEO_STREAM_HEIGHT);

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

/* 帧回调做统计，并把候选帧的 UVC 缓冲所有权交给解码队列。
 *
 * 这里**刻意不做整帧的 JPEG 结构扫描**。本回调运行在 USB 等时传输的上下文里，
 * 而等时传输既没有 CRC 也没有重传：回调每多占一毫秒，transfer 就晚一毫秒重新
 * 入队（见 uvc_isoc.c 末尾的 usb_host_transfer_submit），那段时间的包直接丢失。
 * 一次全帧扫描约 50KB，在 PSRAM 上要花掉接近一毫秒——正好是这个回调负担不起的
 * 量级。判定“这帧到底能不能解码”的工作因此挪到了编解码任务里做
 * （H.264 链路会在独立编解码任务中继续做结构校验）。
 *
 * 本函数只剩常数时间的工作：判空、判格式、判长度、读两个 SOI 字节，以及把
 * UVC 帧所有权转交给编解码任务。编解码任务完成 JPEG 解码后才调用
 * uvc_host_frame_return()，因此这里不再复制约 50 KB 的 MJPEG 数据。
 *
 * 注意：等时传输丢包的帧即使带有 SOI，也可能在下游结构校验时被丢弃；
 * 那条线索由 uvc_isoc.c 的 packet/FID/EoF 诊断计数给出。 */
static uvc_host_stream_hdl_t s_camera_stream;

static camera_frame_loan_t *camera_acquire_frame_loan(uvc_host_frame_t *frame)
{
    camera_frame_loan_t *loan = NULL;
    portENTER_CRITICAL(&s_frame_loan_lock);
    for (unsigned i = 0; i < CAMERA_LOAN_COUNT; ++i) {
        if (!s_frame_loans[i].in_use) {
            s_frame_loans[i].in_use = true;
            s_frame_loans[i].stream = s_camera_stream;
            s_frame_loans[i].frame = frame;
            loan = &s_frame_loans[i];
            break;
        }
    }
    portEXIT_CRITICAL(&s_frame_loan_lock);
    return loan;
}

static void camera_return_frame(void *release_ctx)
{
    camera_frame_loan_t *loan = (camera_frame_loan_t *)release_ctx;
    if (loan == NULL) {
        return;
    }

    const uvc_host_stream_hdl_t stream = loan->stream;
    uvc_host_frame_t *frame = loan->frame;
    if (stream != NULL && frame != NULL) {
        esp_err_t err = uvc_host_frame_return(stream, frame);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "归还 UVC 帧缓冲失败：%s", esp_err_to_name(err));
        }
    }

    portENTER_CRITICAL(&s_frame_loan_lock);
    loan->stream = NULL;
    loan->frame = NULL;
    loan->in_use = false;
    portEXIT_CRITICAL(&s_frame_loan_lock);
}

static bool camera_frame_cb(const uvc_host_frame_t *frame, void *ctx)
{
    (void)ctx;
    const int64_t callback_started_us = esp_timer_get_time();
#define CAMERA_FRAME_CB_RETURN(value) do { \
        camera_record_frame_callback_time(callback_started_us); \
        return (value); \
    } while (0)
    const bool valid_frame = frame->data != NULL && frame->data_len > 0;
    const bool target_frame = valid_frame &&
                              frame->vs_format.format == UVC_VS_FORMAT_MJPEG &&
                              frame->vs_format.h_res == VIDEO_STREAM_WIDTH &&
                              frame->vs_format.v_res == VIDEO_STREAM_HEIGHT;
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

    /* 成功时保留 UVC 帧，待编解码任务完成后异步归还；失败时不取得所有权，
     * 下面返回 true 让 UVC 驱动立即回收该帧。 */
    if (candidate_frame) {
        camera_frame_loan_t *loan = camera_acquire_frame_loan((uvc_host_frame_t *)frame);
        if (loan == NULL) {
            CAMERA_FRAME_CB_RETURN(true);
        }
        /* UVC 的 heap_caps_malloc 缓冲通常满足硬件读对齐；若某个堆配置
         * 只给出较弱对齐，则保留旧的复制路径，不能把未对齐地址交给 JPEG DMA。 */
        if (((uintptr_t)frame->data & 0x0fU) == 0) {
            const bool retained = video_streamer_submit_jpeg_owned(
                frame->data, frame->data_len, camera_return_frame, loan);
            if (retained) {
                CAMERA_FRAME_CB_RETURN(false);
            }
        } else {
            /* 复制必须在归还 UVC 帧之前完成。该分支只用于极少数未对齐
             * 的堆缓冲，优先保证 JPEG DMA 的输入约束。 */
            video_streamer_submit_jpeg(frame->data, frame->data_len);
        }
        camera_return_frame(loan);
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
    /* Keep the original scheduler behavior.  Do not force USB/UVC onto CPU1;
     * the previous CPU1 experiment did not reduce ISOC packet loss. */
    .xCoreID = tskNO_AFFINITY,
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

static esp_err_t camera_usb_stack_install(void)
{
    esp_err_t err = usb_host_install(&s_usb_host_config);
    if (err != ESP_OK) {
        return err;
    }

    xEventGroupClearBits(s_camera_events, CAMERA_USB_ALL_FREE | CAMERA_USB_EVENTS_EXITED);
    s_usb_events_running = true;
    if (xTaskCreate(usb_events_task, "usb_events", 4096, NULL,
                    CAMERA_USB_EVENTS_PRIORITY,
                    &s_usb_events_task) != pdPASS) {
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
    ESP_LOGI(TAG, "初始化 LRCPG720p USB 摄像头驱动");
    ESP_LOGI(TAG, "使用 ESP32-P4 高速 USB Host，当前 PSRAM 可用：%u 字节",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    /* 实时流初始化失败不影响本地 UVC 采集，错误会保留在串口日志中。 */
    esp_err_t streamer_error = video_streamer_init();
    if (streamer_error != ESP_OK) {
        ESP_LOGE(TAG, "初始化 H.264 实时流失败：%s",
                 esp_err_to_name(streamer_error));
    }

    s_camera_events = xEventGroupCreate();
    assert(s_camera_events != NULL);

    /* ESP32-P4 外设映射 BIT0 对应内部高速 USB PHY。 */
    ESP_ERROR_CHECK(camera_usb_stack_install());

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

        candidate %= s_camera_format_count;
        const uvc_host_frame_info_t selected = s_camera_formats[candidate];
        xEventGroupClearBits(s_camera_events, CAMERA_DISCONNECTED | CAMERA_ERROR);

        uvc_host_stream_config_t stream_config = {
            .event_cb = camera_stream_event_cb,
            .frame_cb = camera_frame_cb,
            .usb = {
                .dev_addr = s_camera_device_address,
                .vid = UVC_HOST_ANY_VID,
                .pid = UVC_HOST_ANY_PID,
                .uvc_stream_index = s_camera_stream_index,
            },
            .vs_format = {
                .h_res = selected.h_res,
                .v_res = selected.v_res,
                .fps = camera_interval_to_fps(selected.default_interval),
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
                /* URB 环深就是"主机晚一步重排"的容错窗口。
                 *
                 * 原来给 4：日志 `Each: 33792 bytes, 11 ISOC packets` → 4×11
                 * = 44 微帧 = **5.5 ms**。环里的包一旦排空，后面每一个微帧都
                 * 会被主机标成 SKIPPED（`uvc_isoc.c:36` 的注释原文就是"系统
                 * 延迟/总线过载"），而 MJPEG 是熵编码——丢一个包，这一帧从
                 * 断点往后全部错位，只能在 `isoc_finish_frame()` 里整帧丢掉。
                 * 实测 跳过 16.5/s（占 8000 微帧/s 的 0.21%）就毁掉了
                 * 丢整帧 10.3/s（≈ 全部帧的 34%）。这个放大约 160 倍，所以
                 * 这条路不是"差不多就行"，得把窗口开宽。
                 *
                 * 当前 96 个、32 KiB/个 → 约 1056 微帧 ≈ 132 ms，
                 * 约 3.24 MB PSRAM。这个窗口用于覆盖 100 ms 级别回调停顿的
                 * 大部分场景；如果最大回调间隔仍超过 88 ms，说明问题已经超出
                 * URB 环能吸收的范围，应继续查 HCD 调度或 USB 物理链路。 */
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
            ESP_LOGW(TAG, "打开视频流失败：%s，尝试设备声明的下一个格式", esp_err_to_name(err));
            same_format_retries = 0;
            candidate = (candidate + 1) % s_camera_format_count;
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        s_camera_stream = stream;

        portENTER_CRITICAL(&s_stats_lock);
        memset(&s_stats, 0, sizeof(s_stats));
        portEXIT_CRITICAL(&s_stats_lock);

        /* 将 UVC 诊断窗口与本次视频流绑定，后面的报告均为本次流的增量。 */
        uvc_isoc_diag_reset();
        err = camera_start(stream);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Stream started；等待实际图像帧");
            camera_stats_t previous = {0};
            uvc_isoc_diag_stats_t diag_previous = {0};
            int64_t last_report = esp_timer_get_time();
            unsigned stalls = 0;

            while (true) {
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
                uint32_t received = current.frames - previous.frames;
                /* 名字里必须带 delta：外层 camera_driver_run 已经有一个同名的
                 * `candidate`（格式轮转下标），内层若重名会把它遮住，下面
                 * “停顿后切下一个格式”的两个分支就会改到错误的变量上。 */
                uint32_t candidate_delta =
                    current.candidate_frames - previous.candidate_frames;
                uint32_t target_delta = current.target_frames - previous.target_frames;
                uint32_t callback_delta = current.callback_count - previous.callback_count;
                uint64_t callback_us_delta = current.callback_us - previous.callback_us;
                uint32_t callback_max_us = current.callback_max_us;
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
                ESP_LOGI(TAG,
                         "[UVC] packets timeout=%" PRIu32 " skipped=%" PRIu32
                         "(frame=%" PRIu32 ",idle=%" PRIu32 ") error=%" PRIu32
                         " empty=%" PRIu32 " invalid_header=%" PRIu32
                         " frame_error=%" PRIu32 " dropped=%" PRIu32
                         " callback_gap_max=%" PRIu32 " ms | RX=%" PRIu32
                         " candidate=%" PRIu32 " SOI缺失=%" PRIu32 " EOI缺失=%" PRIu32,
                         diag_current.packet_timeout,
                         diag_current.packet_skipped,
                         diag_current.skipped_in_frame,
                         diag_current.skipped_idle,
                         diag_current.packet_error,
                         diag_current.empty_packet,
                         diag_current.invalid_header,
                         diag_current.frame_error,
                         diag_current.frame_dropped,
                         diag_current.max_callback_gap_ms,
                         received, candidate_delta,
                         current.missing_soi_frames - previous.missing_soi_frames,
                         current.missing_eoi_frames - previous.missing_eoi_frames);

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
                        selected.h_res == VIDEO_STREAM_WIDTH &&
                        selected.v_res == VIDEO_STREAM_HEIGHT;
                    if (preferred_mjpeg &&
                        same_format_retries < CAMERA_SAME_FORMAT_RETRY_LIMIT) {
                        same_format_retries++;
                        ESP_LOGW(TAG,
                                 "连续 10 秒没有可编码帧：关闭并原地重试 %ux%u MJPEG（%u/%u）",
                                 VIDEO_STREAM_WIDTH, VIDEO_STREAM_HEIGHT,
                                 same_format_retries, CAMERA_SAME_FORMAT_RETRY_LIMIT);
                        /* candidate 保持不变；下方完成 stop/close 后重新 open/start 同一格式。 */
                    } else if (preferred_mjpeg) {
                        ESP_LOGW(TAG,
                                 "%ux%u MJPEG 原地重试仍未恢复，改试设备声明的下一个格式",
                                 VIDEO_STREAM_WIDTH, VIDEO_STREAM_HEIGHT);
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
            ESP_LOGE(TAG, "启动视频流失败：%s", esp_err_to_name(err));
            same_format_retries = 0;
            candidate = (candidate + 1) % s_camera_format_count;
        }

        /* 摄像头已拔出时驱动会处理停止；其他情况下由主任务主动停止。 */
        const bool camera_disconnected =
            (xEventGroupGetBits(s_camera_events) & CAMERA_DISCONNECTED) != 0;
        bool rebuild_usb_stack = false;
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
            ESP_LOGI(TAG, "USB 根端口断开后所有 UVC transfer 已完成，允许关闭旧流");
        }

        /* 编解码任务可能仍在读最后一个借出的 UVC 缓冲；先等它归还，
         * 再关闭 stream，避免异步 release 使用已经失效的句柄。 */
        const TickType_t loan_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(100);
        while (true) {
            bool loaned = false;
            portENTER_CRITICAL(&s_frame_loan_lock);
            for (unsigned i = 0; i < CAMERA_LOAN_COUNT; ++i) {
                loaned |= s_frame_loans[i].in_use;
            }
            portEXIT_CRITICAL(&s_frame_loan_lock);
            if (!loaned || (int32_t)(xTaskGetTickCount() - loan_deadline) >= 0) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(1));
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
        if (rebuild_usb_stack) {
            esp_err_t rebuild_error = camera_usb_stack_rebuild();
            if (rebuild_error != ESP_OK) {
                ESP_LOGE(TAG, "USB Host/UVC 重建失败：%s，请重新上电摄像头或复位开发板",
                         esp_err_to_name(rebuild_error));
                return;
            }
            candidate = 0;
            same_format_retries = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
