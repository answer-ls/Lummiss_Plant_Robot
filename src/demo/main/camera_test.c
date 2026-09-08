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

static const char *TAG = "CAM_TEST";

#define CAMERA_DISCONNECTED BIT0
#define CAMERA_ERROR        BIT1
#define CAMERA_FORMATS_READY BIT2
#define MAX_CAMERA_FORMATS  32

static EventGroupHandle_t s_camera_events;
static portMUX_TYPE s_stats_lock = portMUX_INITIALIZER_UNLOCKED;
static uvc_host_frame_info_t s_camera_formats[MAX_CAMERA_FORMATS];
static size_t s_camera_format_count;
static uint8_t s_camera_device_address;
static uint8_t s_camera_stream_index;

typedef struct {
    uint32_t frames;
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

/* LRCPG720p 已实测支持这些 MJPEG 模式。
 * 若设备连接回调暂时没有给出格式列表，使用该列表避免再次传入 0 FPS。 */
static void camera_load_known_formats(void)
{
    static const struct {
        unsigned width;
        unsigned height;
    } known_sizes[] = {
        {640, 480},
        {1280, 720},
        {640, 360},
        {800, 600},
        {1280, 960},
        {720, 960},
    };

    s_camera_format_count = sizeof(known_sizes) / sizeof(known_sizes[0]);
    for (size_t i = 0; i < s_camera_format_count; ++i) {
        s_camera_formats[i] = (uvc_host_frame_info_t) {
            .format = UVC_VS_FORMAT_MJPEG,
            .h_res = known_sizes[i].width,
            .v_res = known_sizes[i].height,
            .default_interval = 333333,
        };
    }
    s_camera_device_address = UVC_HOST_ANY_DEV_ADDR;
    s_camera_stream_index = 0;
    ESP_LOGW(TAG, "暂未收到格式回调，使用 LRCPG720p 已知格式，优先 640x480 MJPEG 30 FPS");
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

    /* 基础连通性测试优先使用 640×480 MJPEG，降低 USB 和内存压力。 */
    for (size_t i = 0; i < count; ++i) {
        if (s_camera_formats[i].format == UVC_VS_FORMAT_MJPEG &&
            s_camera_formats[i].h_res == 640 && s_camera_formats[i].v_res == 480) {
            uvc_host_frame_info_t preferred = s_camera_formats[i];
            s_camera_formats[i] = s_camera_formats[0];
            s_camera_formats[0] = preferred;
            break;
        }
    }

    for (size_t i = 0; i < count; ++i) {
        ESP_LOGI(TAG, "Supported[%u]: %ux%u %s, default %.2f FPS",
                 (unsigned)i, s_camera_formats[i].h_res, s_camera_formats[i].v_res,
                 camera_format_name(s_camera_formats[i].format),
                 camera_interval_to_fps(s_camera_formats[i].default_interval));
    }
    xEventGroupSetBits(s_camera_events, CAMERA_FORMATS_READY);
}

/* 帧回调只做统计并立即归还缓冲区，避免解码或日志阻塞 USB 收帧。 */
static bool camera_frame_cb(const uvc_host_frame_t *frame, void *ctx)
{
    (void)ctx;
    portENTER_CRITICAL(&s_stats_lock);
    if (frame->data != NULL && frame->data_len > 0) {
        s_stats.frames++;
        s_stats.bytes += frame->data_len;
        s_stats.last_size = frame->data_len;
        s_stats.width = frame->vs_format.h_res;
        s_stats.height = frame->vs_format.v_res;
        s_stats.format = frame->vs_format.format;
    } else {
        s_stats.empty_frames++;
    }
    portEXIT_CRITICAL(&s_stats_lock);
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

void app_main(void)
{
    ESP_LOGI(TAG, "Lummiss USB 摄像头测试：LRCPG720p，屏幕未初始化");
    ESP_LOGI(TAG, "使用 ESP32-P4 高速 USB Host，当前 PSRAM 可用：%u 字节",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

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
    while (true) {
        ESP_LOGI(TAG, "等待 UVC 摄像头及格式列表");
        EventBits_t ready = xEventGroupWaitBits(s_camera_events, CAMERA_FORMATS_READY,
                                                pdFALSE, pdTRUE, pdMS_TO_TICKS(3000));
        if (!(ready & CAMERA_FORMATS_READY)) {
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
                .frame_size = 0,
                .number_of_frame_buffers = 3,
                .number_of_urbs = 3,
                .urb_size = 10 * 1024,
                .frame_heap_caps = MALLOC_CAP_SPIRAM,
            },
        };

        ESP_LOGI(TAG, "Probe supported format %u/%u: %ux%u %s @ %.2f FPS",
                 candidate + 1, (unsigned)s_camera_format_count,
                 stream_config.vs_format.h_res, stream_config.vs_format.v_res,
                 camera_format_name(stream_config.vs_format.format),
                 stream_config.vs_format.fps);

        uvc_host_stream_hdl_t stream = NULL;
        esp_err_t err = uvc_host_stream_open(&stream_config, pdMS_TO_TICKS(5000), &stream);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "打开视频流失败：%s，尝试设备声明的下一个格式", esp_err_to_name(err));
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
                    pdFALSE, pdFALSE, pdMS_TO_TICKS(5000));
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
                ESP_LOGI(TAG, "RX frames=%" PRIu32 " (+%" PRIu32 "), fps=%.2f, bytes=%" PRIu64
                         ", last=%u, size=%ux%u, format=%d, empty=%" PRIu32,
                         current.frames, received, received / seconds, current.bytes,
                         (unsigned)current.last_size, current.width, current.height,
                         current.format, current.empty_frames);

                stalls = received ? 0 : stalls + 1;
                previous = current;
                last_report = now;
                if (stalls >= 3) {
                    ESP_LOGW(TAG, "连续 15 秒没有有效帧，改试下一个格式");
                    candidate = (candidate + 1) % s_camera_format_count;
                    break;
                }
            }
        } else {
            ESP_LOGE(TAG, "启动视频流失败：%s", esp_err_to_name(err));
            candidate = (candidate + 1) % s_camera_format_count;
        }

        /* 摄像头已拔出时驱动会处理停止；其他情况下由主任务主动停止。 */
        if (!(xEventGroupGetBits(s_camera_events) & CAMERA_DISCONNECTED)) {
            esp_err_t stop_error = camera_stop(stream);
            if (stop_error != ESP_OK) {
                ESP_LOGW(TAG, "停止视频流失败：%s", esp_err_to_name(stop_error));
            }
        }

        esp_err_t close_error = uvc_host_stream_close(stream);
        if (close_error != ESP_OK) {
            ESP_LOGE(TAG, "关闭视频流失败：%s，请复位开发板恢复", esp_err_to_name(close_error));
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
