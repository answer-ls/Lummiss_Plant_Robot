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

#include "camera_driver.h"
#include "video_streamer.h"

static const char *TAG = "CAMERA";

#define CAMERA_DISCONNECTED BIT0
#define CAMERA_ERROR        BIT1
#define CAMERA_FORMATS_READY BIT2
#define MAX_CAMERA_FORMATS  32
#define CAMERA_STALL_LIMIT  1
#define CAMERA_SAME_FORMAT_RETRY_LIMIT 1

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

typedef struct {
    uint32_t frames;
    uint32_t target_frames;      /* 分辨率和格式符合目标的 MJPEG 帧，包括损坏帧 */
    uint32_t complete_frames;    /* SOI/EOI 完整且未超过缓冲上限，可送入解码器 */
    uint32_t missing_soi_frames;
    uint32_t missing_eoi_frames;
    uint32_t oversized_frames;
    uint32_t empty_frames;
    uint64_t bytes;
    size_t last_size;
    unsigned width;
    unsigned height;
    int format;
} camera_stats_t;

static camera_stats_t s_stats;

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
    xEventGroupSetBits(s_camera_events, CAMERA_FORMATS_READY);
}

/* 帧回调只做统计，并按限速策略将完整的 VIDEO_STREAM_WIDTH×VIDEO_STREAM_HEIGHT
 * MJPEG 帧复制到解码队列。JPEG 解码、H.264 编码和 HTTP 请求由独立任务执行，
 * 本回调不会阻塞 USB 收帧。 */
static bool camera_frame_cb(const uvc_host_frame_t *frame, void *ctx)
{
    (void)ctx;
    const bool valid_frame = frame->data != NULL && frame->data_len > 0;
    const bool target_frame = valid_frame &&
                              frame->vs_format.format == UVC_VS_FORMAT_MJPEG &&
                              frame->vs_format.h_res == VIDEO_STREAM_WIDTH &&
                              frame->vs_format.v_res == VIDEO_STREAM_HEIGHT;
    const bool oversized_frame = target_frame &&
                                 frame->data_len > VIDEO_STREAM_JPEG_MAX_SIZE;
    const bool has_soi = target_frame && frame->data_len >= 2 &&
                         frame->data[0] == 0xff && frame->data[1] == 0xd8;
    /* 从末尾反查 EOI：兼容某些 UVC 摄像头在 JPEG 后附带少量填充字节，
     * 提交时只复制到 FF D9，避免把填充内容交给硬件解码器。 */
    size_t jpeg_size = 0;
    if (target_frame && frame->data_len >= 2) {
        for (size_t end = frame->data_len; end >= 2; --end) {
            if (frame->data[end - 2] == 0xff && frame->data[end - 1] == 0xd9) {
                jpeg_size = end;
                break;
            }
        }
    }
    const bool has_eoi = jpeg_size != 0;
    const bool complete_frame = target_frame && !oversized_frame && has_soi && has_eoi;

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
            if (complete_frame) {
                s_stats.complete_frames++;
            }
            if (!has_soi) {
                s_stats.missing_soi_frames++;
            }
            if (!has_eoi) {
                s_stats.missing_eoi_frames++;
            }
            if (oversized_frame) {
                s_stats.oversized_frames++;
            }
        }
    } else {
        s_stats.empty_frames++;
    }
    portEXIT_CRITICAL(&s_stats_lock);

    /* MJPEG 帧复制必须在统计临界区之外执行。 */
    if (complete_frame) {
        video_streamer_submit_jpeg(frame->data, jpeg_size);
    }
    return true;
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

/* 持续处理 USB Host 库事件，摄像头拔出后才能正确回收设备资源。 */
static void usb_events_task(void *arg)
{
    (void)arg;
    while (true) {
        uint32_t event_flags = 0;
        ESP_ERROR_CHECK(usb_host_lib_handle_events(portMAX_DELAY, &event_flags));
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            usb_host_device_free_all();
        }
    }
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
    const usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
        .peripheral_map = BIT0,
    };
    ESP_ERROR_CHECK(usb_host_install(&host_config));

    BaseType_t task_created = xTaskCreate(usb_events_task, "usb_events", 4096,
                                          NULL, 15, NULL);
    assert(task_created == pdPASS);

    const uvc_host_driver_config_t driver_config = {
        .driver_task_stack_size = 4096,
        .driver_task_priority = 16,
        .xCoreID = tskNO_AFFINITY,
        .create_background_task = true,
        .event_cb = camera_driver_event_cb,
        .user_ctx = NULL,
    };
    ESP_ERROR_CHECK(uvc_host_install(&driver_config));

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
                /* 摄像头协商的 dwMaxVideoFrameSize 偏小会截断复杂画面。
                 * 显式预留 512KB，并将大块 DMA 缓冲放入 PSRAM。 */
                .frame_size = VIDEO_STREAM_JPEG_MAX_SIZE,
                .number_of_frame_buffers = 3,
                /* 较大的 URB 减少等时传输的提交和中断频率。 */
                .number_of_urbs = 4,
                .urb_size = 32 * 1024,
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

        portENTER_CRITICAL(&s_stats_lock);
        memset(&s_stats, 0, sizeof(s_stats));
        portEXIT_CRITICAL(&s_stats_lock);

        err = camera_start(stream);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Stream started；等待实际图像帧");
            camera_stats_t previous = {0};
            int64_t last_report = esp_timer_get_time();
            unsigned stalls = 0;

            while (true) {
                EventBits_t bits = xEventGroupWaitBits(
                    s_camera_events, CAMERA_DISCONNECTED | CAMERA_ERROR,
                    pdFALSE, pdFALSE, pdMS_TO_TICKS(10000));
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
                uint32_t complete = current.complete_frames - previous.complete_frames;
                ESP_LOGI(TAG, "RX frames=%" PRIu32 " (+%" PRIu32 ", 完整%ux%uMJPEG +%" PRIu32
                         "), fps=%.2f, bytes=%" PRIu64
                         ", last=%u, size=%ux%u, format=%d, empty=%" PRIu32
                         ", JPEG目标=%" PRIu32 ", 缺SOI=%" PRIu32
                         ", 缺EOI=%" PRIu32 ", 过大=%" PRIu32,
                         current.frames, received, VIDEO_STREAM_WIDTH, VIDEO_STREAM_HEIGHT,
                         complete, received / seconds, current.bytes,
                         (unsigned)current.last_size, current.width, current.height,
                         current.format, current.empty_frames, current.target_frames,
                         current.missing_soi_frames, current.missing_eoi_frames,
                         current.oversized_frames);

                /* 以“可编码帧”判定停顿：非目标分辨率 MJPEG（如 1280×960）即便一直在
                 * 收帧，也不能喂 H.264，继续驻留只会无声无息没有视频，故同样计入停顿轮转。 */
                if (complete) {
                    stalls = 0;
                    /* 当前格式已经真正收到可编码帧，下次停顿允许重新做一次原地恢复。 */
                    same_format_retries = 0;
                } else {
                    stalls++;
                }
                previous = current;
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
        if (!camera_disconnected) {
            esp_err_t stop_error = camera_stop(stream);
            if (stop_error != ESP_OK) {
                ESP_LOGW(TAG, "停止视频流失败：%s", esp_err_to_name(stop_error));
            }
        }

        /* 摄像头拔出时设备句柄已失效，close 可能失败：失败即终止摄像头任务，
         * 需重新上电恢复。热拔插支持不在本需求范围内。 */
        esp_err_t close_error = uvc_host_stream_close(stream);
        if (close_error != ESP_OK) {
            ESP_LOGE(TAG, "关闭视频流失败：%s，请复位开发板恢复", esp_err_to_name(close_error));
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
