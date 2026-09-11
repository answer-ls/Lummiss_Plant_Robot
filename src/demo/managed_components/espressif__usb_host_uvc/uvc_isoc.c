/*
 * SPDX-FileCopyrightText: 2024-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <inttypes.h>
#include <stdbool.h>
#include <string.h> // For memcpy

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"

#include "uvc_stream.h" // For uvc_host_stream_pause()
#include "esp_private/uvc_isoc_diag.h"
#include "uvc_types_priv.h"
#include "uvc_check_priv.h"
#include "uvc_frame_priv.h"
#include "uvc_critical_priv.h"

static const char *TAG = "uvc-isoc";

/* 丢包诊断计数。
 *
 * 等时传输既没有 CRC 也没有重传（见 isoc_transfer_callback 的说明），包丢了
 * 上层是无从察觉的——留在 MJPEG 帧里的只是一处空洞，任何"SOI/EOI 齐全 +
 * 结构扫描通过"的校验都可能放它过去。所以丢包必须单独计数：这几个数只要
 * 随帧数一起增长，就说明帧损坏的根源在 USB 链路，而不在别处。
 *
 * 只在传输回调里累加（同一时刻只有一个传输回调在跑，不需要加锁），
 * 打印按 5 秒限频——丢包严重时频繁写串口反而会把回调拖得更慢。 */
#define ISOC_DIAG_LOG_INTERVAL_MS 5000

static uint32_t s_diag_packet_timeout;   /* 等时包超时 */
static uint32_t s_diag_packet_skipped;   /* 等时包被跳过（系统延迟/总线过载） */
static uint32_t s_diag_packet_error;     /* 等时包报错：ERROR / OVERFLOW / STALL */
static uint32_t s_diag_empty_packet;     /* 零长度等时包：摄像头这个微帧没发数据 */
static uint32_t s_diag_invalid_header;   /* 长度够但 UVC payload 头部校验失败 */
static uint32_t s_diag_frame_error;      /* 摄像头在包头里置了 error 位 */
static uint32_t s_diag_frame_dropped;    /* 已经收到整帧、却因上面的原因被丢弃的帧数 */
static uint32_t s_diag_skipped_in_frame;  /* 跳过发生在 FID 帧组装期间 */
static uint32_t s_diag_skipped_idle;      /* 跳过发生在没有活动帧时 */
static TickType_t s_diag_last_callback_ticks;
static uint32_t s_diag_max_callback_gap_ms;
static TickType_t s_diag_last_log_ticks;

void uvc_isoc_diag_reset(void)
{
    s_diag_packet_timeout = 0;
    s_diag_packet_skipped = 0;
    s_diag_packet_error = 0;
    s_diag_empty_packet = 0;
    s_diag_invalid_header = 0;
    s_diag_frame_error = 0;
    s_diag_frame_dropped = 0;
    s_diag_skipped_in_frame = 0;
    s_diag_skipped_idle = 0;
    s_diag_last_callback_ticks = 0;
    s_diag_max_callback_gap_ms = 0;
    s_diag_last_log_ticks = 0;
}

void uvc_isoc_diag_get(uvc_isoc_diag_stats_t *stats)
{
    if (stats == NULL) {
        return;
    }

    stats->packet_timeout = __atomic_load_n(&s_diag_packet_timeout, __ATOMIC_RELAXED);
    stats->packet_skipped = __atomic_load_n(&s_diag_packet_skipped, __ATOMIC_RELAXED);
    stats->packet_error = __atomic_load_n(&s_diag_packet_error, __ATOMIC_RELAXED);
    stats->empty_packet = __atomic_load_n(&s_diag_empty_packet, __ATOMIC_RELAXED);
    stats->invalid_header = __atomic_load_n(&s_diag_invalid_header, __ATOMIC_RELAXED);
    stats->frame_error = __atomic_load_n(&s_diag_frame_error, __ATOMIC_RELAXED);
    stats->frame_dropped = __atomic_load_n(&s_diag_frame_dropped, __ATOMIC_RELAXED);
    stats->skipped_in_frame = __atomic_load_n(&s_diag_skipped_in_frame, __ATOMIC_RELAXED);
    stats->skipped_idle = __atomic_load_n(&s_diag_skipped_idle, __ATOMIC_RELAXED);
    stats->max_callback_gap_ms = __atomic_load_n(&s_diag_max_callback_gap_ms, __ATOMIC_RELAXED);
}

static void isoc_diag_update_callback_gap(void)
{
    const TickType_t now_ticks = xTaskGetTickCount();
    if (s_diag_last_callback_ticks != 0) {
        const uint32_t gap_ms =
            (uint32_t)(now_ticks - s_diag_last_callback_ticks) * portTICK_PERIOD_MS;
        if (gap_ms > s_diag_max_callback_gap_ms) {
            s_diag_max_callback_gap_ms = gap_ms;
        }
    }
    s_diag_last_callback_ticks = now_ticks;
}

static void isoc_diag_log_limited(const char *reason)
{
    /* 用 FreeRTOS tick 计时，不用 esp_timer_get_time()：本组件不在 esp_timer 的
     * 依赖里（编译命令的 -I 列表可以确认），引它进来还得改组件的 CMakeLists，
     * 那就又多一个"组件更新后要重打"的文件。tick 是无符号数，相减的写法对
     * 回绕天然正确。 */
    const TickType_t now_ticks = xTaskGetTickCount();
    if (s_diag_last_log_ticks != 0 &&
        (now_ticks - s_diag_last_log_ticks) < pdMS_TO_TICKS(ISOC_DIAG_LOG_INTERVAL_MS)) {
        return;
    }
    s_diag_last_log_ticks = now_ticks;
    ESP_LOGW(TAG, "%s；累计 超时=%" PRIu32 " 跳过=%" PRIu32
             "(帧内=%" PRIu32 ",空闲=%" PRIu32 ") 包错误=%" PRIu32
             " 空包=%" PRIu32 " 头部非法=%" PRIu32 " 帧错误位=%" PRIu32 " 丢整帧=%" PRIu32
             " 回调最大间隔=%" PRIu32 "ms",
             reason, s_diag_packet_timeout, s_diag_packet_skipped,
             s_diag_skipped_in_frame, s_diag_skipped_idle, s_diag_packet_error,
             s_diag_empty_packet, s_diag_invalid_header, s_diag_frame_error,
             s_diag_frame_dropped, s_diag_max_callback_gap_ms);
}

static size_t jpeg_find_soi(const uint8_t *data, size_t data_len)
{
    if (data == NULL || data_len < 2) {
        return SIZE_MAX;
    }

    for (size_t i = 0; i + 1 < data_len; ++i) {
        if (data[i] == JPEG_MARKER && data[i + 1] == JPEG_SOI) {
            return i;
        }
    }
    return SIZE_MAX;
}

/* Complete the current frame outside the critical section.
 *
 * Do not scan the whole JPEG here.  This function runs from the USB transfer
 * callback, and a recovered FID boundary is precisely the path used when EOF
 * was lost.  Full-frame validation belongs to the codec task; doing it here
 * can delay re-submission of the isochronous transfer and create more packet
 * loss.  camera_frame_cb() checks SOI immediately; the codec task validates
 * and trims EOI after the frame has left the USB callback. */
static void isoc_finish_frame(uvc_stream_t *uvc_stream)
{
    bool return_frame = true;
    uvc_host_frame_t *this_frame;

    UVC_ENTER_CRITICAL();
    this_frame = uvc_stream->dynamic.current_frame;
    uvc_stream->dynamic.current_frame = NULL;

    const bool frame_skipped = uvc_stream->single_thread.skip_current_frame;
    const bool invoke_fb_callback =
        (uvc_stream->dynamic.streaming && uvc_stream->constant.frame_cb && this_frame &&
         !uvc_stream->single_thread.skip_current_frame);
    UVC_EXIT_CRITICAL();
    uvc_stream->single_thread.frame_in_progress = false;

    /* 单独数"整帧被丢"。只看"跳过=NNN"不知道有多少帧被牵连——一个包丢一次
     * 计数，可能毁掉一帧，也可能落在两个帧之间的空闲里什么都没毁。这个计数才
     * 是丢包真正付出的代价，也是判断"丢帧换干净帧"这笔交易划不划算的唯一依据。 */
    if (this_frame != NULL && frame_skipped) {
        s_diag_frame_dropped++;
    }

    if (invoke_fb_callback) {
        return_frame = uvc_stream->constant.frame_cb(this_frame, uvc_stream->constant.cb_arg);
    }
    if (this_frame && return_frame) {
        uvc_host_frame_return(uvc_stream, this_frame);
    }
}

/**
 * @brief Callback function for handling Isochronous USB transfers from a UVC camera.
 *
 * This function processes isochronous transfer packets, which may contain video frame data. The following key points
 * are handled in the transfer:
 *
 * - **Start of Frame (SoF)**: Detected by a change in Frame ID, which toggles between 0 and 1.
 * - **End of Frame (EoF)**: Signaled in the packet header.
 * - **Transfer Characteristics**:
 *   - **No CRC**: Data corruption is possible.
 *   - **No ACK**: Packets can be missed.
 *   - **Packet Header**: Each packet includes a header used to detect errors, missed packets, and other issues.
 *
 * The callback performs the following tasks:
 * 1. Checks the status of each isochronous packet and handles various USB transfer statuses (e.g., completed,
 *    error, device disconnected).
 * 2. Parses packet headers to detect the start of new frames, handles errors, and manages frame buffers.
 * 3. Aggregates valid data into a frame buffer, ensuring no buffer overflow occurs.
 * 4. Signals the end of a frame and invokes user-defined callbacks if necessary.
 *
 * @param[in] transfer Pointer to the completed USB transfer structure.
 */
void isoc_transfer_callback(usb_transfer_t *transfer)
{
    ESP_LOGD(TAG, "%s", __FUNCTION__);
    uvc_stream_t *uvc_stream = (uvc_stream_t *)transfer->context;
    isoc_diag_update_callback_gap();
    __atomic_add_fetch(&uvc_stream->constant.callbacks_in_flight, 1, __ATOMIC_SEQ_CST);
    /* This callback owns the completion of one previously submitted URB. */
    __atomic_sub_fetch(&uvc_stream->constant.transfers_in_flight, 1, __ATOMIC_SEQ_CST);
    __atomic_add_fetch(&uvc_stream->constant.transfers_completed, 1, __ATOMIC_SEQ_CST);

    // USB_TRANSFER_STATUS_NO_DEVICE is set in transfer->status.
    // Other error codes are saved in status of each ISOC packet descriptor
    if (transfer->status == USB_TRANSFER_STATUS_NO_DEVICE) {
        ESP_ERROR_CHECK(uvc_host_stream_pause(uvc_stream)); // This should never fail
    }

    if (!UVC_ATOMIC_LOAD(uvc_stream->dynamic.streaming)) {
        goto callback_exit; // If the streaming was turned off, we don't have to do anything
    }

    const uint8_t *payload = transfer->data_buffer;
    for (int i = 0; i < transfer->num_isoc_packets; i++) {
        usb_isoc_packet_desc_t *isoc_desc = &transfer->isoc_packet_desc[i];

        // Check USB status
        switch (isoc_desc->status) {
        case USB_TRANSFER_STATUS_COMPLETED:
            break;
        case USB_TRANSFER_STATUS_NO_DEVICE:
        case USB_TRANSFER_STATUS_CANCELED:
            ESP_ERROR_CHECK(uvc_host_stream_pause(uvc_stream)); // This should never fail
            goto callback_exit; // No need to process the rest
        case USB_TRANSFER_STATUS_ERROR:
        case USB_TRANSFER_STATUS_OVERFLOW:
        case USB_TRANSFER_STATUS_STALL:
            s_diag_packet_error++;
            ESP_LOGW(TAG, "usb err %d", isoc_desc->status);
            uvc_stream->single_thread.skip_current_frame = true;
            goto next_isoc_packet; // Data corrupted

        case USB_TRANSFER_STATUS_TIMED_OUT:
        case USB_TRANSFER_STATUS_SKIPPED:
            /* Upstream treats these as harmless ("not an issue"), which holds for
             * control transfers. For an MJPEG frame it does not: a lost ISOC packet
             * leaves a hole in the entropy-coded scan, and entropy data has no
             * self-delimiting structure, so every bit after the hole is misaligned.
             * Count it, and drop the frame here rather than handing a spliced-together
             * remnant upstairs for a structural check to discover afterwards. */
            if (isoc_desc->status == USB_TRANSFER_STATUS_TIMED_OUT) {
                s_diag_packet_timeout++;
            } else {
                s_diag_packet_skipped++;
                if (uvc_stream->single_thread.frame_in_progress) {
                    s_diag_skipped_in_frame++;
                } else {
                    s_diag_skipped_idle++;
                }
            }
            uvc_stream->single_thread.skip_current_frame = true;
            isoc_diag_log_limited("ISOC 包丢失");
            goto next_isoc_packet;
        default:
            assert(false);
        }

        // Check for start of new frame
        const uvc_payload_header_t *payload_header = (const uvc_payload_header_t *)payload;
        if (!uvc_frame_payload_header_validate(payload_header, isoc_desc->actual_num_bytes)) {
            /* Upstream logged this at ESP_LOGD, i.e. invisible at the default log
             * level — yet a payload header that fails validation is the most direct
             * evidence available that packets were lost or that the stream lost
             * byte alignment. Count it and make it visible.
             *
             * 但必须分成两类数，否则这个计数器会把手持日志的人带沟里（2026-09-10
             * 就发生过）：校验器在 packet_len < sizeof(uvc_payload_header_t) 时直接
             * 返回 false，所以**零长度的空包也会走这个分支**。空包的含义是"摄像头
             * 这个微帧一个字节都没发"，空闲的等时端点按 8kHz 微帧刷 → 计数以
             * ~8000/s 增长，看着像"整条流全坏了"，其实只是摄像头没出流。两者混在
             * 一起时无法区分，所以分开数。
             *
             * 流正常时不会出现空包：摄像头帧内空闲发的是"只有 2 字节包头"的包，
             * 它能过校验，然后在下面 payload_data_len == 0 分支被静默跳过。 */
            if (isoc_desc->actual_num_bytes < sizeof(uvc_payload_header_t)) {
                s_diag_empty_packet++;
            } else {
                s_diag_invalid_header++;
                ESP_LOGD(TAG, "invalid UVC payload header, %02x, %02x, len:%d", payload[0], payload[1], isoc_desc->actual_num_bytes);
                isoc_diag_log_limited("UVC payload 头部非法");
            }
            uvc_stream->single_thread.skip_current_frame = true;
            goto next_isoc_packet;
        }

        // Derive payload data pointer/length once and reuse below
        const uint8_t *payload_data = payload + payload_header->bHeaderLength;
        size_t payload_data_len = isoc_desc->actual_num_bytes - payload_header->bHeaderLength;

        if (payload_data_len == 0) {
            // This is a zero-length packet, skip it
            ESP_LOGD(TAG, "zero-length packet, skipping");
            goto next_isoc_packet;
        }

        // Check for error flag
        if (payload_header->bmHeaderInfo.error) {
            /* 摄像头自己报告本帧出错。走限频日志，否则它会按包重复刷屏。 */
            s_diag_frame_error++;
            uvc_stream->single_thread.skip_current_frame = true;
            isoc_diag_log_limited("摄像头报告帧错误");
        }

        const bool start_of_frame = (uvc_stream->single_thread.current_frame_id != payload_header->bmHeaderInfo.frame_id);
        if (start_of_frame) {
            /*
             * FID is the authoritative boundary for UVC streams.  The EOF
             * bit is useful, but it is carried by the packet that may have
             * been lost on an isochronous endpoint.  Finish the old frame at
             * the FID transition instead of retaining it until the buffer
             * fills up.  A valid JPEG can still be delivered even when EOF
             * itself was lost.
             */
            if (uvc_stream->dynamic.current_frame != NULL) {
                isoc_finish_frame(uvc_stream);
            }

            // We detected start of new frame. Update Frame ID and start fetching this frame
            uvc_stream->single_thread.current_frame_id   = payload_header->bmHeaderInfo.frame_id;
            uvc_stream->single_thread.skip_current_frame = payload_header->bmHeaderInfo.error;
            uvc_stream->single_thread.frame_in_progress = true;

            /*
             * The first payload after a FID change is normally SOI, but a
             * dropped first packet can leave a few bytes before SOI.  Resync
             * within this payload instead of poisoning the whole following
             * frame.  SIZE_MAX means that no SOI was found.
             */
            if (uvc_stream->dynamic.vs_format.format == UVC_VS_FORMAT_MJPEG) {
                const size_t soi_offset = jpeg_find_soi(payload_data, payload_data_len);
                if (soi_offset == SIZE_MAX) {
                    uvc_stream->single_thread.skip_current_frame = true;
                    ESP_LOGD(TAG, "discarding MJPEG frame without SOI");
                } else if (soi_offset != 0) {
                    ESP_LOGD(TAG, "resynchronized MJPEG SOI after %u bytes", (unsigned)soi_offset);
                    payload_data += soi_offset;
                    payload_data_len -= soi_offset;
                }
            }

            // Get free frame buffer for this new frame
            UVC_ENTER_CRITICAL();
            const bool need_new_frame = (uvc_stream->dynamic.streaming && !uvc_stream->dynamic.current_frame);
            if (need_new_frame) {
                UVC_EXIT_CRITICAL();
                uvc_stream->dynamic.current_frame = uvc_frame_get_empty(uvc_stream);
                if (uvc_stream->dynamic.current_frame == NULL) {
                // There is no free frame buffer now, skipping this frame
                uvc_stream->single_thread.skip_current_frame = true;

                    // Inform the user about the underflow
                    uvc_host_stream_callback_t stream_cb = uvc_stream->constant.stream_cb;
                    if (stream_cb) {
                        const uvc_host_stream_event_data_t event = {
                            .type = UVC_HOST_FRAME_BUFFER_UNDERFLOW,
                        };
                        stream_cb(&event, uvc_stream->constant.cb_arg);
                    }
                    goto next_isoc_packet;
                }
            }
        }

        // Add received data to frame buffer
        if (!uvc_stream->single_thread.skip_current_frame) {
            uvc_host_frame_t *current_frame = UVC_ATOMIC_LOAD(uvc_stream->dynamic.current_frame);

            if (current_frame == NULL) {
                uvc_stream->single_thread.skip_current_frame = true;
                goto next_isoc_packet;
            }

            esp_err_t ret = uvc_frame_add_data(current_frame, payload_data, payload_data_len);
            if (ret != ESP_OK) {
                // Release the broken frame immediately so it cannot consume a buffer until the next FID.
                uvc_stream->single_thread.skip_current_frame = true;

                UVC_ENTER_CRITICAL();
                uvc_host_frame_t *overflow_frame = uvc_stream->dynamic.current_frame;
                uvc_stream->dynamic.current_frame = NULL;
                UVC_EXIT_CRITICAL();
                if (overflow_frame != NULL) {
                    uvc_host_frame_return(uvc_stream, overflow_frame);
                }

                // Inform the user about the overflow
                uvc_host_stream_callback_t stream_cb = uvc_stream->constant.stream_cb;
                if (stream_cb) {
                    const uvc_host_stream_event_data_t event = {
                        .type = UVC_HOST_FRAME_BUFFER_OVERFLOW,
                    };
                    stream_cb(&event, uvc_stream->constant.cb_arg);
                }
                goto next_isoc_packet;
            }
        }

        // End of Frame. Pass the frame to user
        if (payload_header->bmHeaderInfo.end_of_frame) {
            /* Explicit EOF remains the normal completion path. */
            isoc_finish_frame(uvc_stream);
        }
next_isoc_packet:
        payload += isoc_desc->num_bytes;
        continue;
    }

    if (UVC_ATOMIC_LOAD(uvc_stream->dynamic.streaming)) {
        /* Reserve the lifecycle count before submitting. A completion can be
         * delivered immediately on another context after submit returns. */
        __atomic_add_fetch(&uvc_stream->constant.transfers_in_flight, 1, __ATOMIC_SEQ_CST);
        if (usb_host_transfer_submit(transfer) != ESP_OK) {
            __atomic_sub_fetch(&uvc_stream->constant.transfers_in_flight, 1, __ATOMIC_SEQ_CST);
            __atomic_add_fetch(&uvc_stream->constant.transfer_submit_failures, 1, __ATOMIC_SEQ_CST);
            /* The stream is no longer able to maintain its URB ring. Let the
             * camera watchdog rebuild it instead of silently losing the ring. */
            UVC_ENTER_CRITICAL();
            uvc_stream->dynamic.streaming = false;
            UVC_EXIT_CRITICAL();
        }
    }

callback_exit:
    __atomic_sub_fetch(&uvc_stream->constant.callbacks_in_flight, 1, __ATOMIC_SEQ_CST);
}
