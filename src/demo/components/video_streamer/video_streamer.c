#include "video_streamer.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "driver/jpeg_decode.h"
#include "esp_h264_alloc.h"
#include "esp_h264_enc_single.h"
#include "esp_h264_enc_single_hw.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "network_manager.h"

static const char *TAG = "VIDEO_STREAM";

/* 当前电脑 WLAN IPv4 为 192.168.1.66。地址变化后修改此项并重新构建。 */
#define VIDEO_STREAM_URL              "http://192.168.1.66:8000/h264"
/* 分辨率统一取 video_streamer.h 的公共常量，与摄像头侧"可编码帧"门控一致。 */
#define VIDEO_WIDTH                   VIDEO_STREAM_WIDTH
#define VIDEO_HEIGHT                  VIDEO_STREAM_HEIGHT
/* 800×600 的像素量高于 640×480，先使用 15fps 保留编解码余量。
 * GOP 与 PTS 除数跟随该宏，避免改帧率时漏改。 */
#define VIDEO_ENCODE_FPS              15
#define VIDEO_GOP                     VIDEO_ENCODE_FPS
/* 800×600 使用 1.5Mbps，在清晰度和 WiFi 发送压力之间取平衡。 */
#define VIDEO_BITRATE                 1500000
#define VIDEO_QP_MIN                  20
#define VIDEO_QP_MAX                  40
/* 摄像头侧 MJPEG 输入环槽数（提交时 memcpy 进槽）。 */
#define VIDEO_SLOT_COUNT              2
/* H.264 编码输出（码流）槽数：编码任务写满一个槽就交给发送任务，
 * 发送完成归还。槽数 4 可在 HTTP 短暂变慢时吸收抖动。 */
#define VIDEO_OUT_SLOT_COUNT          4
/* 摄像头 MJPEG 实测为 YUV422 采样；JPEG 硬件直接输出 U Y0 V Y1，16bpp。 */
#define VIDEO_YUV422_SIZE             (VIDEO_WIDTH * VIDEO_HEIGHT * 2)
/* H.264 硬件输入固定为 O_UYY_E_VYY 交错 YUV420，1.5 字节/像素。 */
#define VIDEO_H264_INPUT_SIZE         (VIDEO_WIDTH * VIDEO_HEIGHT * 3 / 2)
/* 码流输出缓冲：沿用原尺寸（远大于实际 NAL），每槽独立一份。 */
#define VIDEO_H264_OUTPUT_SIZE        VIDEO_H264_INPUT_SIZE

#define VIDEO_TASK_STACK              8192
/* 编解码任务：JPEG 解码 + YUV 重排 + H.264 编码，全程不等待网络。 */
#define VIDEO_CODEC_PRIORITY          8
#define VIDEO_CODEC_CORE              1
/* 网络发送任务：只做 HTTP 上传与失败处理，与编解码任务并行。 */
#define VIDEO_UPLOAD_PRIORITY         9
#define VIDEO_UPLOAD_CORE             0
#define VIDEO_HTTP_TIMEOUT_MS         3000
#define VIDEO_REPORT_INTERVAL_US      (10 * 1000 * 1000LL)
/* 门控按 VIDEO_ENCODE_FPS 的帧周期推进；允许提前 5 ms 接帧，
 * 避免整数取整导致的周期漂移。 */
#define VIDEO_SUBMIT_EARLY_US         5000

typedef struct {
    uint8_t *data;
    size_t data_len;   /* 本帧 JPEG 压缩数据长度（可变），解码按它喂位流 */
    uint32_t sequence;
    bool busy;
} video_input_slot_t;

/* H.264 输出槽：编码任务写入码流后连同元数据交给发送任务。
 * data_len/sequence/pts/frame_type 由编码任务填写，发送任务只读。 */
typedef struct {
    uint8_t *data;
    size_t data_len;
    uint32_t sequence;
    uint32_t pts;
    int frame_type;
} video_out_slot_t;

typedef struct {
    uint32_t submitted;
    uint32_t rate_limited;
    uint32_t dropped;        /* 提交侧：MJPEG 环槽全忙时丢弃 */
    uint32_t slot_dropped;   /* 编码侧：无空闲输出槽时在编码前丢弃 MJPEG 帧 */
    uint32_t encoded;
    uint32_t sent;
    uint32_t send_failed;
    uint64_t encoded_bytes;
    uint64_t jpeg_decode_us;
    uint64_t yuv_repack_us;
    uint64_t h264_encode_us;
    uint64_t http_send_us;
    uint64_t http_max_us;    /* 报告周期内 HTTP 单帧最长耗时（重置式） */
} video_stream_stats_t;

static video_input_slot_t s_slots[VIDEO_SLOT_COUNT];
static video_out_slot_t s_out_slots[VIDEO_OUT_SLOT_COUNT];
static QueueHandle_t s_input_queue;
/* 输出槽所有权两条队列：free=可写槽，ready=已编码待发送槽。 */
static QueueHandle_t s_out_free;
static QueueHandle_t s_out_ready;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static video_stream_stats_t s_stats;
static int64_t s_next_submit_us;
static uint32_t s_next_sequence;
static bool s_initialized;

/* 网络或服务器异常时最多每 5 秒输出一次详情，避免 20 FPS 的失败日志刷屏。 */
static void video_stream_log_error_limited(const char *message, int error)
{
    static int64_t last_log_us;
    const int64_t now_us = esp_timer_get_time();
    if (last_log_us == 0 || now_us - last_log_us >= 5 * 1000 * 1000LL) {
        ESP_LOGW(TAG, "%s：%d", message, error);
        last_log_us = now_us;
    }
}

/* ESP32-P4 v1.x 的 H.264 硬件输入格式为交错 YUV420（O_UYY_E_VYY）：
 * JPEG 硬件直出的 YUV422 每两个像素为 U Y0 V Y1。对相邻两行的
 * U/V 分别求平均完成垂直 2:1 色度抽样，再重排为编码器所需布局。
 * 这里只做字节复制和两个均值，避免 RGB565 路径每像素的颜色换算。 */
static void yuv422_to_h264_yuv420(const uint8_t *src, uint8_t *dst)
{
    const size_t src_line_bytes = VIDEO_WIDTH * 2;
    const size_t dst_line_bytes = VIDEO_WIDTH * 3 / 2;

    for (unsigned by = 0; by < VIDEO_HEIGHT / 2; ++by) {
        const uint8_t *src_even = src + (by * 2) * src_line_bytes;
        const uint8_t *src_odd = src_even + src_line_bytes;
        uint8_t *dst_even = dst + (by * 2) * dst_line_bytes;
        uint8_t *dst_odd = dst_even + dst_line_bytes;

        for (unsigned bx = 0; bx < VIDEO_WIDTH / 2; ++bx) {
            /* 输入：U0 Y00 V0 Y01 / U1 Y10 V1 Y11。 */
            dst_even[0] = (uint8_t)(((unsigned)src_even[0] + src_odd[0] + 1) >> 1);
            dst_even[1] = src_even[1];
            dst_even[2] = src_even[3];
            dst_odd[0] = (uint8_t)(((unsigned)src_even[2] + src_odd[2] + 1) >> 1);
            dst_odd[1] = src_odd[1];
            dst_odd[2] = src_odd[3];

            src_even += 4;
            src_odd += 4;
            dst_even += 3;
            dst_odd += 3;
        }
    }
}

static esp_http_client_handle_t video_http_client_create(void)
{
    const esp_http_client_config_t config = {
        .url = VIDEO_STREAM_URL,
        .method = HTTP_METHOD_POST,
        .timeout_ms = VIDEO_HTTP_TIMEOUT_MS,
        .buffer_size = 512,
        .buffer_size_tx = 2048,
        .keep_alive_enable = true,
        .disable_auto_redirect = true,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client != NULL) {
        /* 分辨率/帧率头必须与编码配置一致，PC 端按它推算时间轴。 */
        char size_text[16];
        char fps_text[8];
        snprintf(size_text, sizeof(size_text), "%u", VIDEO_WIDTH);
        esp_http_client_set_header(client, "X-Video-Width", size_text);
        snprintf(size_text, sizeof(size_text), "%u", VIDEO_HEIGHT);
        esp_http_client_set_header(client, "X-Video-Height", size_text);
        snprintf(fps_text, sizeof(fps_text), "%d", VIDEO_ENCODE_FPS);
        esp_http_client_set_header(client, "Content-Type", "video/h264");
        esp_http_client_set_header(client, "X-Video-FPS", fps_text);
        esp_http_client_set_header(client, "Connection", "keep-alive");
    }
    return client;
}

static esp_err_t video_http_send(esp_http_client_handle_t client,
                                 const uint8_t *data,
                                 size_t data_len,
                                 uint32_t sequence,
                                 uint32_t pts,
                                 int frame_type)
{
    char sequence_text[16];
    char pts_text[16];
    char frame_type_text[8];
    snprintf(sequence_text, sizeof(sequence_text), "%" PRIu32, sequence);
    snprintf(pts_text, sizeof(pts_text), "%" PRIu32, pts);
    snprintf(frame_type_text, sizeof(frame_type_text), "%d", frame_type);

    esp_http_client_set_header(client, "X-Frame-Sequence", sequence_text);
    esp_http_client_set_header(client, "X-Frame-PTS", pts_text);
    esp_http_client_set_header(client, "X-Frame-Type", frame_type_text);
    esp_http_client_set_post_field(client, (const char *)data, data_len);

    esp_err_t err = esp_http_client_perform(client);
    const int status = esp_http_client_get_status_code(client);
    if (err == ESP_OK && status >= 200 && status < 300) {
        return ESP_OK;
    }

    if (err == ESP_OK) {
        return ESP_FAIL;
    }
    return err;
}

static void video_stream_report(int64_t now_us)
{
    static int64_t last_report_us;
    static video_stream_stats_t previous;
    if (last_report_us == 0) {
        last_report_us = now_us;
        return;
    }
    if (now_us - last_report_us < VIDEO_REPORT_INTERVAL_US) {
        return;
    }

    video_stream_stats_t current;
    portENTER_CRITICAL(&s_lock);
    current = s_stats;
    /* 报告周期内的 HTTP 最长单帧耗时：读出后清零，只统计当前窗口。 */
    s_stats.http_max_us = 0;
    portEXIT_CRITICAL(&s_lock);

    const double seconds = (now_us - last_report_us) / 1000000.0;
    const uint32_t encoded_delta = current.encoded - previous.encoded;
    const uint32_t sent_delta = current.sent - previous.sent;
    const uint64_t bytes_delta = current.encoded_bytes - previous.encoded_bytes;
    const uint64_t decode_us_delta = current.jpeg_decode_us - previous.jpeg_decode_us;
    const uint64_t repack_us_delta = current.yuv_repack_us - previous.yuv_repack_us;
    const uint64_t encode_us_delta = current.h264_encode_us - previous.h264_encode_us;
    const uint64_t send_us_delta = current.http_send_us - previous.http_send_us;
    const double sample_count = encoded_delta > 0 ? encoded_delta : 1;
    ESP_LOGI(TAG,
             "H.264：编码=%.1f fps，发送=%.1f fps，%.0f kbps，耗时 J=%.1f/Y=%.1f/H=%.1f/HTTP=%.1f(峰值 %.1f) ms，过载丢帧=%" PRIu32 "，失败=%" PRIu32,
             encoded_delta / seconds, sent_delta / seconds,
             bytes_delta * 8.0 / seconds / 1000.0,
             decode_us_delta / sample_count / 1000.0,
             repack_us_delta / sample_count / 1000.0,
             encode_us_delta / sample_count / 1000.0,
             send_us_delta / sample_count / 1000.0,
             current.http_max_us / 1000.0,
             current.dropped + current.slot_dropped, current.send_failed);

    previous = current;
    last_report_us = now_us;
}

static esp_err_t video_encoder_create(esp_h264_enc_handle_t *encoder)
{
    const esp_h264_enc_cfg_hw_t config = {
        .pic_type = ESP_H264_RAW_FMT_O_UYY_E_VYY,
        .gop = VIDEO_GOP,
        .fps = VIDEO_ENCODE_FPS,
        .res = {
            .width = VIDEO_WIDTH,
            .height = VIDEO_HEIGHT,
        },
        .rc = {
            .bitrate = VIDEO_BITRATE,
            .qp_min = VIDEO_QP_MIN,
            .qp_max = VIDEO_QP_MAX,
        },
    };

    esp_h264_err_t err = esp_h264_enc_hw_new(&config, encoder);
    if (err != ESP_H264_ERR_OK) {
        ESP_LOGE(TAG, "创建 H.264 硬件编码器失败：%d", err);
        return ESP_FAIL;
    }
    err = esp_h264_enc_open(*encoder);
    if (err != ESP_H264_ERR_OK) {
        ESP_LOGE(TAG, "打开 H.264 硬件编码器失败：%d", err);
        esp_h264_enc_del(*encoder);
        *encoder = NULL;
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* 网络发送任务（钉核 0，优先 9）：
 * 只做 H.264 码流上传与失败恢复，不参与任何编解码。
 * 编码完成前绝不丢弃 P 帧——输出槽一旦入队就必须发送，
 * 否则 PC 端解码器会因缺帧花屏直到下一个 IDR。 */
static void video_upload_task(void *arg)
{
    (void)arg;
    esp_http_client_handle_t http_client = video_http_client_create();
    if (http_client == NULL) {
        ESP_LOGE(TAG, "创建 H.264 HTTP 客户端失败");
        vTaskDelete(NULL);
        return;
    }

    unsigned out_index;
    while (true) {
        if (xQueueReceive(s_out_ready, &out_index, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        video_out_slot_t *slot = &s_out_slots[out_index];

        esp_err_t send_error = ESP_ERR_INVALID_STATE;
        const int64_t send_started_us = esp_timer_get_time();
        if (network_manager_wait_connected(VIDEO_HTTP_TIMEOUT_MS)) {
            send_error = video_http_send(http_client, slot->data, slot->data_len,
                                         slot->sequence, slot->pts, slot->frame_type);
        }
        const int64_t send_elapsed_us = esp_timer_get_time() - send_started_us;

        portENTER_CRITICAL(&s_lock);
        s_stats.http_send_us += send_elapsed_us;
        if (send_elapsed_us > (int64_t)s_stats.http_max_us) {
            s_stats.http_max_us = send_elapsed_us;
        }
        if (send_error == ESP_OK) {
            s_stats.sent++;
        } else {
            s_stats.send_failed++;
        }
        portEXIT_CRITICAL(&s_lock);

        if (send_error != ESP_OK) {
            video_stream_log_error_limited("H.264 发送失败", send_error);
            /* 关闭连接，下一帧 perform 时自动重连恢复。 */
            esp_http_client_close(http_client);
        }

        /* 无论成败都归还输出槽，编码任务才可能继续推进。 */
        xQueueSend(s_out_free, &out_index, portMAX_DELAY);
        video_stream_report(esp_timer_get_time());
    }
}

/* 编解码任务（钉核 1，优先 8）：
 * JPEG 解码 → YUV 重排 → H.264 编码，完成后把输出槽索引交给上传任务，
 * 自身不等待网络，保证目标帧率下编码侧稳定供帧。 */
static void video_codec_task(void *arg)
{
    (void)arg;
    uint32_t aligned_input_size = VIDEO_H264_INPUT_SIZE;
    uint8_t *h264_input = esp_h264_aligned_calloc(
        16, 1, VIDEO_H264_INPUT_SIZE, &aligned_input_size, ESP_H264_MEM_SPIRAM);
    esp_h264_enc_handle_t encoder = NULL;
    jpeg_decoder_handle_t jpeg_decoder = NULL;
    uint8_t *jpeg_yuv = NULL;
    uint8_t *out_buffers[VIDEO_OUT_SLOT_COUNT] = { 0 };
    unsigned initialized_out = 0;
    bool upload_started = false;

    if (h264_input == NULL ||
        video_encoder_create(&encoder) != ESP_OK) {
        ESP_LOGE(TAG, "H.264 编码资源初始化失败");
        goto codec_fail;
    }

    /* JPEG 硬件解码引擎：一帧 40ms 超时（30fps 解码 >34ms 即认为超时）。 */
    const jpeg_decode_engine_cfg_t jpeg_engine_cfg = {
        .timeout_ms = 40,
    };
    if (jpeg_new_decoder_engine(&jpeg_engine_cfg, &jpeg_decoder) != ESP_OK) {
        ESP_LOGE(TAG, "创建 JPEG 硬件解码引擎失败");
        goto codec_fail;
    }

    /* YUV422 解码输出缓冲必须用 jpeg_alloc_decoder_mem 申请（PSRAM、cache 对齐）。
     * 解码输出只在编解码任务内使用，单份即可。 */
    const jpeg_decode_memory_alloc_cfg_t yuv_alloc_cfg = {
        .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER,
    };
    size_t jpeg_yuv_size = 0;
    jpeg_yuv = jpeg_alloc_decoder_mem(VIDEO_YUV422_SIZE,
                                      &yuv_alloc_cfg, &jpeg_yuv_size);
    if (jpeg_yuv == NULL || jpeg_yuv_size < VIDEO_YUV422_SIZE) {
        ESP_LOGE(TAG, "分配 JPEG 解码输出缓冲失败");
        goto codec_fail;
    }

    /* H.264 输出槽池与两条所有权队列：编解码任务独占写入，上传任务独占读出。 */
    s_out_free = xQueueCreate(VIDEO_OUT_SLOT_COUNT, sizeof(unsigned));
    s_out_ready = xQueueCreate(VIDEO_OUT_SLOT_COUNT, sizeof(unsigned));
    if (s_out_free == NULL || s_out_ready == NULL) {
        ESP_LOGE(TAG, "创建 H.264 输出槽队列失败");
        goto codec_fail;
    }
    for (unsigned i = 0; i < VIDEO_OUT_SLOT_COUNT; ++i) {
        uint32_t actual_size = 0;
        out_buffers[i] = esp_h264_aligned_calloc(
            16, 1, VIDEO_H264_OUTPUT_SIZE, &actual_size, ESP_H264_MEM_SPIRAM);
        if (out_buffers[i] == NULL || actual_size < VIDEO_H264_OUTPUT_SIZE) {
            ESP_LOGE(TAG, "分配第 %u 个 H.264 输出缓冲失败", i);
            goto codec_fail;
        }
        initialized_out = i + 1;
        s_out_slots[i].data = out_buffers[i];
        s_out_slots[i].data_len = 0;
        xQueueSend(s_out_free, &i, 0);
    }

    BaseType_t created = xTaskCreatePinnedToCore(video_upload_task,
                                                 "video_upload",
                                                 VIDEO_TASK_STACK,
                                                 NULL,
                                                 VIDEO_UPLOAD_PRIORITY,
                                                 NULL,
                                                 VIDEO_UPLOAD_CORE);
    if (created != pdPASS) {
        ESP_LOGE(TAG, "创建网络发送任务失败");
        goto codec_fail;
    }
    upload_started = true;

    ESP_LOGI(TAG,
             "编解码/上传双任务已就绪：MJPEG(YUV422直出)→硬编 H.264 "
             "%ux%u@%dfps，%dkbps，GOP=%d，输出槽=%d，HTTP=%s",
             VIDEO_WIDTH, VIDEO_HEIGHT, VIDEO_ENCODE_FPS,
             VIDEO_BITRATE / 1000, VIDEO_GOP,
             VIDEO_OUT_SLOT_COUNT, VIDEO_STREAM_URL);

    unsigned slot_index;
    while (true) {
        if (xQueueReceive(s_input_queue, &slot_index, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        video_input_slot_t *slot = &s_slots[slot_index];
        const uint32_t sequence = slot->sequence;

        /* JPEG 硬件解码：环槽里的 MJPEG 帧直接作为位流源，无需二次拷贝。
         * 解码完成后即可释放 MJPEG 环槽，后续各段不再引用它。 */
        const jpeg_decode_cfg_t decode_cfg = {
            .output_format = JPEG_DECODE_OUT_FORMAT_YUV422,
            /* YUV 输出不使用 rgb_order/conv_std，这里保留合法默认值。 */
            .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_RGB,
            .conv_std = JPEG_YUV_RGB_CONV_STD_BT601,
        };
        uint32_t decoded_size = 0;
        const int64_t decode_started_us = esp_timer_get_time();
        esp_err_t decode_error = jpeg_decoder_process(
            jpeg_decoder, &decode_cfg,
            slot->data, (uint32_t)slot->data_len,
            jpeg_yuv, (uint32_t)jpeg_yuv_size, &decoded_size);
        const int64_t decode_elapsed_us = esp_timer_get_time() - decode_started_us;

        portENTER_CRITICAL(&s_lock);
        slot->busy = false;
        portEXIT_CRITICAL(&s_lock);

        if (decode_error != ESP_OK || decoded_size != VIDEO_YUV422_SIZE) {
            video_stream_log_error_limited("JPEG 解码失败", decode_error);
            continue;
        }

        /* 没有空闲输出槽说明上传侧暂时落后：在编码之前丢弃本帧。
         * 丢弃的是“输入帧”而非“已编码 P 帧”，码流时间轴保持连续，
         * PC 端不会因缺帧花屏。 */
        unsigned out_index;
        if (xQueueReceive(s_out_free, &out_index, 0) != pdTRUE) {
            portENTER_CRITICAL(&s_lock);
            s_stats.slot_dropped++;
            portEXIT_CRITICAL(&s_lock);
            continue;
        }
        video_out_slot_t *out_slot = &s_out_slots[out_index];

        /* YUV422 → esp_h264 要求的 O_UYY_E_VYY 交错 YUV420。 */
        const int64_t repack_started_us = esp_timer_get_time();
        yuv422_to_h264_yuv420(jpeg_yuv, h264_input);
        const int64_t repack_elapsed_us = esp_timer_get_time() - repack_started_us;

        esp_h264_enc_in_frame_t input_frame = {
            .raw_data = {
                .buffer = h264_input,
                .len = aligned_input_size,
            },
            .pts = sequence * (90000 / VIDEO_ENCODE_FPS),
        };
        esp_h264_enc_out_frame_t output_frame = {
            .raw_data = {
                .buffer = out_slot->data,
                .len = VIDEO_H264_OUTPUT_SIZE,
            },
        };

        const int64_t encode_started_us = esp_timer_get_time();
        esp_h264_err_t encode_error =
            esp_h264_enc_process(encoder, &input_frame, &output_frame);
        const int64_t encode_elapsed_us = esp_timer_get_time() - encode_started_us;

        portENTER_CRITICAL(&s_lock);
        s_stats.jpeg_decode_us += decode_elapsed_us;
        s_stats.yuv_repack_us += repack_elapsed_us;
        s_stats.h264_encode_us += encode_elapsed_us;
        portEXIT_CRITICAL(&s_lock);

        if (encode_error == ESP_H264_ERR_OK && output_frame.length > 0) {
            out_slot->data_len = output_frame.length;
            out_slot->sequence = sequence;
            out_slot->pts = input_frame.pts;
            out_slot->frame_type = output_frame.frame_type;
            portENTER_CRITICAL(&s_lock);
            s_stats.encoded++;
            s_stats.encoded_bytes += output_frame.length;
            portEXIT_CRITICAL(&s_lock);

            /* 队列深度与输出槽数一致，槽在被发送任务归还前不会再入队，
             * 此发送不可能失败，只做防御性检查。 */
            if (xQueueSend(s_out_ready, &out_index, 0) != pdTRUE) {
                xQueueSend(s_out_free, &out_index, 0);
                portENTER_CRITICAL(&s_lock);
                s_stats.slot_dropped++;
                portEXIT_CRITICAL(&s_lock);
            }
        } else {
            video_stream_log_error_limited("H.264 编码失败", encode_error);
            /* 编码失败没有产出码流，槽直接归还，不影响上传侧连续解码。 */
            xQueueSend(s_out_free, &out_index, 0);
        }
    }

codec_fail:
    ESP_LOGE(TAG, "编解码任务初始化失败，H.264 实时流不可用");
    if (upload_started) {
        /* 上传任务一旦启动就独占 HTTP client，无法在此回收，保持现状并退出。 */
        vTaskDelete(NULL);
        return;
    }
    if (s_out_ready != NULL) {
        vQueueDelete(s_out_ready);
        s_out_ready = NULL;
    }
    if (s_out_free != NULL) {
        vQueueDelete(s_out_free);
        s_out_free = NULL;
    }
    for (unsigned i = 0; i < initialized_out; ++i) {
        heap_caps_free(out_buffers[i]);
    }
    if (jpeg_yuv != NULL) {
        heap_caps_free(jpeg_yuv);
    }
    if (jpeg_decoder != NULL) {
        jpeg_del_decoder_engine(jpeg_decoder);
    }
    if (encoder != NULL) {
        esp_h264_enc_close(encoder);
        esp_h264_enc_del(encoder);
    }
    if (h264_input != NULL) {
        esp_h264_free(h264_input);
    }
    vTaskDelete(NULL);
}

esp_err_t video_streamer_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    s_input_queue = xQueueCreate(VIDEO_SLOT_COUNT, sizeof(unsigned));
    if (s_input_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }

    /* 槽缓冲用解码器专用分配接口申请，返回的 PSRAM 缓冲可直接作为
     * JPEG 硬件解码的位流源（输入方向无需额外对齐）。 */
    const jpeg_decode_memory_alloc_cfg_t slot_alloc_cfg = {
        .buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER,
    };
    size_t slot_size = 0;
    for (unsigned i = 0; i < VIDEO_SLOT_COUNT; ++i) {
        s_slots[i].data = jpeg_alloc_decoder_mem(VIDEO_STREAM_JPEG_MAX_SIZE,
                                                 &slot_alloc_cfg, &slot_size);
        if (s_slots[i].data == NULL) {
            ESP_LOGE(TAG, "分配第 %u 个 MJPEG PSRAM 缓冲失败", i);
            while (i > 0) {
                heap_caps_free(s_slots[--i].data);
                s_slots[i].data = NULL;
            }
            vQueueDelete(s_input_queue);
            s_input_queue = NULL;
            return ESP_ERR_NO_MEM;
        }
    }

    BaseType_t created = xTaskCreatePinnedToCore(video_codec_task,
                                                 "video_codec",
                                                 VIDEO_TASK_STACK,
                                                 NULL,
                                                 VIDEO_CODEC_PRIORITY,
                                                 NULL,
                                                 VIDEO_CODEC_CORE);
    if (created != pdPASS) {
        for (unsigned i = 0; i < VIDEO_SLOT_COUNT; ++i) {
            heap_caps_free(s_slots[i].data);
            s_slots[i].data = NULL;
        }
        vQueueDelete(s_input_queue);
        s_input_queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_initialized = true;
    return ESP_OK;
}

bool video_streamer_submit_jpeg(const uint8_t *data,
                                size_t data_len)
{
    if (!s_initialized || !network_manager_is_connected() || data == NULL ||
        data_len == 0 || data_len > VIDEO_STREAM_JPEG_MAX_SIZE) {
        return false;
    }

    const int64_t now_us = esp_timer_get_time();
    const int64_t frame_interval_us = 1000000LL / VIDEO_ENCODE_FPS;
    if (s_next_submit_us != 0 &&
        now_us + VIDEO_SUBMIT_EARLY_US < s_next_submit_us) {
        portENTER_CRITICAL(&s_lock);
        s_stats.rate_limited++;
        portEXIT_CRITICAL(&s_lock);
        return false;
    }

    unsigned selected = VIDEO_SLOT_COUNT;
    portENTER_CRITICAL(&s_lock);
    for (unsigned i = 0; i < VIDEO_SLOT_COUNT; ++i) {
        if (!s_slots[i].busy) {
            s_slots[i].busy = true;
            selected = i;
            break;
        }
    }
    if (selected == VIDEO_SLOT_COUNT) {
        s_stats.dropped++;
    }
    portEXIT_CRITICAL(&s_lock);

    if (selected == VIDEO_SLOT_COUNT) {
        return false;
    }

    video_input_slot_t *slot = &s_slots[selected];
    memcpy(slot->data, data, data_len);
    slot->data_len = data_len;
    slot->sequence = ++s_next_sequence;

    if (xQueueSend(s_input_queue, &selected, 0) != pdTRUE) {
        portENTER_CRITICAL(&s_lock);
        slot->busy = false;
        s_stats.dropped++;
        portEXIT_CRITICAL(&s_lock);
        return false;
    }

    portENTER_CRITICAL(&s_lock);
    s_stats.submitted++;
    portEXIT_CRITICAL(&s_lock);
    /* 以既定时间轴前进，避免把每帧抖动累计到输出帧率；若任务曾长时间
     * 停顿，则从当前时刻重新建立基准，避免恢复后短时间突发提交。 */
    if (s_next_submit_us == 0 ||
        now_us - s_next_submit_us >= frame_interval_us) {
        s_next_submit_us = now_us + frame_interval_us;
    } else {
        s_next_submit_us += frame_interval_us;
    }
    return true;
}
