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
#define ISOC_DIAG_EVENT_CAPACITY 128
#define ISOC_DIAG_OFFSET_NONE    (-1)
#define ISOC_DIAG_OFFSET_CROSS   (-2)
#define ISOC_DIAG_TIME_UNSET     UINT32_MAX
#define JPEG_EOI                 0xD9U

typedef struct {
    uint32_t timestamp_ms;
    uint32_t packet_index;
    uint32_t pts;
    uint32_t current_frame_len;
    uint16_t actual_num_bytes;
    int16_t soi_offset;
    int16_t eoi_offset;
    uint8_t header_len;
    uint8_t header_info;
    uint8_t fid;
    uint8_t eof;
    uint8_t error;
    uint8_t frame_active_before;
    uint8_t frame_active_after;
} isoc_diag_event_t;

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
static uint32_t s_diag_pts_change_count;
static uint32_t s_diag_pts_same_count;
static uint32_t s_diag_soi_count;
static uint32_t s_diag_eoi_count;
static uint32_t s_diag_frame_start_by_soi;
static uint32_t s_diag_frame_start_by_fid;
static uint32_t s_diag_frame_finish_by_eof;
static uint32_t s_diag_frame_finish_by_fid;
static uint32_t s_diag_frame_finish_by_eoi;
static uint32_t s_diag_drop_by_err;
static uint32_t s_diag_drop_by_fid_change;
static uint32_t s_diag_drop_by_missing_soi;
static uint32_t s_diag_drop_by_overflow;
static uint32_t s_diag_drop_by_new_soi;
static uint32_t s_diag_current_frame_len;
static uint32_t s_diag_max_frame_len;
static uint32_t s_diag_frame_buffer_capacity;
static uint32_t s_diag_first_nonempty_ms;
static uint32_t s_diag_first_soi_ms;
static uint32_t s_diag_first_complete_frame_ms;
static TickType_t s_diag_stream_start_ticks;
static bool s_diag_stream_started;
static bool s_diag_seen_nonempty;
static bool s_diag_seen_soi;
static bool s_diag_have_pts;
static uint32_t s_diag_last_pts;
static bool s_diag_previous_payload_byte_valid;
static uint8_t s_diag_previous_payload_byte;
static uint32_t s_diag_packet_index;
static isoc_diag_event_t s_diag_events[ISOC_DIAG_EVENT_CAPACITY];
static uint32_t s_diag_event_write_index;
static uint32_t s_diag_event_count;
static bool s_diag_events_frozen;
static uint32_t s_diag_empty_inside_active_frame;
static uint32_t s_diag_frame_abort_by_empty;
static uint32_t s_diag_header_only_inside_active_frame;
static uint32_t s_diag_frame_abort_by_header_only;

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
    s_diag_pts_change_count = 0;
    s_diag_pts_same_count = 0;
    s_diag_soi_count = 0;
    s_diag_eoi_count = 0;
    s_diag_frame_start_by_soi = 0;
    s_diag_frame_start_by_fid = 0;
    s_diag_frame_finish_by_eof = 0;
    s_diag_frame_finish_by_fid = 0;
    s_diag_frame_finish_by_eoi = 0;
    s_diag_drop_by_err = 0;
    s_diag_drop_by_fid_change = 0;
    s_diag_drop_by_missing_soi = 0;
    s_diag_drop_by_overflow = 0;
    s_diag_drop_by_new_soi = 0;
    s_diag_current_frame_len = 0;
    s_diag_max_frame_len = 0;
    s_diag_frame_buffer_capacity = 0;
    s_diag_first_nonempty_ms = ISOC_DIAG_TIME_UNSET;
    s_diag_first_soi_ms = ISOC_DIAG_TIME_UNSET;
    s_diag_first_complete_frame_ms = ISOC_DIAG_TIME_UNSET;
    s_diag_stream_start_ticks = 0;
    s_diag_stream_started = false;
    s_diag_seen_nonempty = false;
    s_diag_seen_soi = false;
    s_diag_have_pts = false;
    s_diag_last_pts = 0;
    s_diag_previous_payload_byte_valid = false;
    s_diag_previous_payload_byte = 0;
    s_diag_packet_index = 0;
    s_diag_event_write_index = 0;
    s_diag_event_count = 0;
    s_diag_events_frozen = false;
    s_diag_empty_inside_active_frame = 0;
    s_diag_frame_abort_by_empty = 0;
    s_diag_header_only_inside_active_frame = 0;
    s_diag_frame_abort_by_header_only = 0;
}

void uvc_isoc_diag_mark_stream_start(void)
{
    s_diag_stream_start_ticks = xTaskGetTickCount();
    s_diag_stream_started = true;
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
#define COPY_DIAG_FIELD(name) stats->name = __atomic_load_n(&s_diag_##name, __ATOMIC_RELAXED)
    COPY_DIAG_FIELD(pts_change_count);
    COPY_DIAG_FIELD(pts_same_count);
    COPY_DIAG_FIELD(soi_count);
    COPY_DIAG_FIELD(eoi_count);
    COPY_DIAG_FIELD(frame_start_by_soi);
    COPY_DIAG_FIELD(frame_start_by_fid);
    COPY_DIAG_FIELD(frame_finish_by_eof);
    COPY_DIAG_FIELD(frame_finish_by_fid);
    COPY_DIAG_FIELD(frame_finish_by_eoi);
    COPY_DIAG_FIELD(drop_by_err);
    COPY_DIAG_FIELD(drop_by_fid_change);
    COPY_DIAG_FIELD(drop_by_missing_soi);
    COPY_DIAG_FIELD(drop_by_overflow);
    COPY_DIAG_FIELD(drop_by_new_soi);
    COPY_DIAG_FIELD(current_frame_len);
    COPY_DIAG_FIELD(max_frame_len);
    COPY_DIAG_FIELD(frame_buffer_capacity);
    COPY_DIAG_FIELD(first_nonempty_ms);
    COPY_DIAG_FIELD(first_soi_ms);
    COPY_DIAG_FIELD(first_complete_frame_ms);
    COPY_DIAG_FIELD(empty_inside_active_frame);
    COPY_DIAG_FIELD(frame_abort_by_empty);
    COPY_DIAG_FIELD(header_only_inside_active_frame);
    COPY_DIAG_FIELD(frame_abort_by_header_only);
#undef COPY_DIAG_FIELD
}

static uint32_t isoc_diag_elapsed_ms(void)
{
    if (!s_diag_stream_started) {
        return ISOC_DIAG_TIME_UNSET;
    }
    return (uint32_t)(xTaskGetTickCount() - s_diag_stream_start_ticks) * portTICK_PERIOD_MS;
}

/* “活动”表示当前帧存在且仍允许追加数据；skip 置位后虽然 buffer 还在，
 * assembler 已经不会再写入，诊断上应视为活动帧被中止。 */
static bool isoc_frame_active(const uvc_stream_t *uvc_stream)
{
    return uvc_stream->single_thread.frame_in_progress &&
           uvc_stream->dynamic.current_frame != NULL &&
           !uvc_stream->single_thread.skip_current_frame;
}

static void isoc_diag_record_event(uint16_t actual_num_bytes,
                                   const uvc_payload_header_t *header,
                                   uint32_t pts, int16_t soi_offset, int16_t eoi_offset,
                                   bool frame_active_before, bool frame_active_after)
{
    UVC_ENTER_CRITICAL();
    if (s_diag_events_frozen) {
        UVC_EXIT_CRITICAL();
        return;
    }
    isoc_diag_event_t *event = &s_diag_events[s_diag_event_write_index];
    *event = (isoc_diag_event_t) {
        .timestamp_ms = isoc_diag_elapsed_ms(),
        .packet_index = s_diag_packet_index,
        .pts = pts,
        .current_frame_len = s_diag_current_frame_len,
        .actual_num_bytes = actual_num_bytes,
        .soi_offset = soi_offset,
        .eoi_offset = eoi_offset,
        .header_len = header ? header->bHeaderLength : 0,
        .header_info = header ? header->bmHeaderInfo.val : 0,
        .fid = header ? header->bmHeaderInfo.frame_id : 0,
        .eof = header ? header->bmHeaderInfo.end_of_frame : 0,
        .error = header ? header->bmHeaderInfo.error : 0,
        .frame_active_before = frame_active_before,
        .frame_active_after = frame_active_after,
    };
    s_diag_event_write_index = (s_diag_event_write_index + 1) % ISOC_DIAG_EVENT_CAPACITY;
    if (s_diag_event_count < ISOC_DIAG_EVENT_CAPACITY) {
        s_diag_event_count++;
    }
    UVC_EXIT_CRITICAL();
}

void uvc_isoc_diag_dump_events(void)
{
    /* 冻结和回调写入共用 UVC 临界区；退出后 ring 不再变化，可以安全打印。 */
    UVC_ENTER_CRITICAL();
    s_diag_events_frozen = true;
    const uint32_t count = s_diag_event_count;
    const uint32_t first = (s_diag_event_write_index + ISOC_DIAG_EVENT_CAPACITY - count) %
                           ISOC_DIAG_EVENT_CAPACITY;
    UVC_EXIT_CRITICAL();
    ESP_LOGI(TAG, "ISOC 关键事件：保留最近 %" PRIu32 " 条", count);
    for (uint32_t i = 0; i < count; ++i) {
        const isoc_diag_event_t *event =
            &s_diag_events[(first + i) % ISOC_DIAG_EVENT_CAPACITY];
        ESP_LOGI(TAG,
                 "[ISOC_EVENT %" PRIu32 "] t=%" PRIu32 "ms packet=%" PRIu32
                 " bytes=%u header=%u/0x%02X FID=%u EOF=%u ERR=%u PTS=%" PRIu32
                 " SOI=%d EOI=%d frame_len=%" PRIu32 " active=%u->%u",
                 i, event->timestamp_ms, event->packet_index,
                 event->actual_num_bytes, event->header_len, event->header_info,
                 event->fid, event->eof, event->error, event->pts,
                 event->soi_offset, event->eoi_offset, event->current_frame_len,
                 event->frame_active_before, event->frame_active_after);
    }
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
    /* 保留计数，关闭此前 UVC 包级测试告警，避免干扰完整系统基线日志。 */
    (void)reason;
}

static size_t jpeg_find_marker(const uint8_t *data, size_t data_len, uint8_t marker)
{
    if (data == NULL || data_len < 2) {
        return SIZE_MAX;
    }

    for (size_t i = 0; i + 1 < data_len; ++i) {
        if (data[i] == JPEG_MARKER && data[i + 1] == marker) {
            return i;
        }
    }
    return SIZE_MAX;
}

static int16_t isoc_diag_marker_offset(const uint8_t *data, size_t data_len, uint8_t marker)
{
    if (data_len > 0 && s_diag_previous_payload_byte_valid &&
        s_diag_previous_payload_byte == JPEG_MARKER && data[0] == marker) {
        return ISOC_DIAG_OFFSET_CROSS;
    }
    const size_t offset = jpeg_find_marker(data, data_len, marker);
    return offset == SIZE_MAX ? ISOC_DIAG_OFFSET_NONE : (int16_t)offset;
}

typedef enum {
    ISOC_FINISH_EOF,
    ISOC_FINISH_FID,
    ISOC_FINISH_EOI,
    ISOC_FINISH_NEW_SOI,
} isoc_finish_reason_t;

/* Complete the current frame outside the critical section.
 *
 * Do not scan the whole JPEG here.  This function runs from the USB transfer
 * callback, and a recovered FID boundary is precisely the path used when EOF
 * was lost.  Full-frame validation belongs to the codec task; doing it here
 * can delay re-submission of the isochronous transfer and create more packet
 * loss.  camera_frame_cb() checks SOI immediately; the codec task validates
 * and trims EOI after the frame has left the USB callback. */
static void isoc_finish_frame(uvc_stream_t *uvc_stream, isoc_finish_reason_t reason)
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
        if (reason == ISOC_FINISH_FID) {
            s_diag_drop_by_fid_change++;
        } else if (reason == ISOC_FINISH_NEW_SOI) {
            s_diag_drop_by_new_soi++;
        }
    }

    if (invoke_fb_callback) {
        if (reason == ISOC_FINISH_EOF) {
            s_diag_frame_finish_by_eof++;
        } else if (reason == ISOC_FINISH_FID) {
            s_diag_frame_finish_by_fid++;
        } else if (reason == ISOC_FINISH_EOI) {
            s_diag_frame_finish_by_eoi++;
        }
        if (s_diag_first_complete_frame_ms == ISOC_DIAG_TIME_UNSET) {
            s_diag_first_complete_frame_ms = isoc_diag_elapsed_ms();
        }
        return_frame = uvc_stream->constant.frame_cb(this_frame, uvc_stream->constant.cb_arg);
    }
    if (this_frame && return_frame) {
        uvc_host_frame_return(uvc_stream, this_frame);
    }
    s_diag_current_frame_len = 0;
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
        s_diag_packet_index++;
        bool first_nonempty_event = false;
        if (isoc_desc->actual_num_bytes > 0 && !s_diag_seen_nonempty) {
            s_diag_seen_nonempty = true;
            s_diag_first_nonempty_ms = isoc_diag_elapsed_ms();
            first_nonempty_event = true;
        }

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
            s_diag_drop_by_err++;
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
            const bool frame_active_before = isoc_frame_active(uvc_stream);
            if (isoc_desc->actual_num_bytes == 0) {
                /* 普通零长度 ISOC 包没有 UVC 语义，只统计并忽略。绝不能把
                 * 当前 JPEG 标为 skip，否则后续同 FID/PTS 数据将无法追加。 */
                s_diag_empty_packet++;
                if (frame_active_before) {
                    s_diag_empty_inside_active_frame++;
                }
                const bool frame_active_after = isoc_frame_active(uvc_stream);
                if (frame_active_before && !frame_active_after) {
                    s_diag_frame_abort_by_empty++;
                }
                if (frame_active_before) {
                    isoc_diag_record_event(0, NULL, 0,
                                           ISOC_DIAG_OFFSET_NONE, ISOC_DIAG_OFFSET_NONE,
                                           frame_active_before, frame_active_after);
                }
                goto next_isoc_packet;
            } else if (isoc_desc->actual_num_bytes < sizeof(uvc_payload_header_t)) {
                s_diag_invalid_header++;
            } else {
                s_diag_invalid_header++;
                ESP_LOGD(TAG, "invalid UVC payload header, %02x, %02x, len:%d", payload[0], payload[1], isoc_desc->actual_num_bytes);
                isoc_diag_log_limited("UVC payload 头部非法");
            }
            uvc_stream->single_thread.skip_current_frame = true;
            if (first_nonempty_event) {
                const uvc_payload_header_t *event_header =
                    isoc_desc->actual_num_bytes >= sizeof(uvc_payload_header_t) ?
                    payload_header : NULL;
                isoc_diag_record_event(isoc_desc->actual_num_bytes, event_header, 0,
                                       ISOC_DIAG_OFFSET_NONE, ISOC_DIAG_OFFSET_NONE,
                                       frame_active_before, isoc_frame_active(uvc_stream));
            }
            goto next_isoc_packet;
        }

        // Derive payload data pointer/length once and reuse below
        const uint8_t *payload_data = payload + payload_header->bHeaderLength;
        size_t payload_data_len = isoc_desc->actual_num_bytes - payload_header->bHeaderLength;

        uint32_t pts = 0;
        if (payload_header->bmHeaderInfo.presentation_time &&
            payload_header->bHeaderLength >= 6) {
            memcpy(&pts, payload + 2, sizeof(pts));
            if (s_diag_have_pts) {
                if (pts == s_diag_last_pts) {
                    s_diag_pts_same_count++;
                } else {
                    s_diag_pts_change_count++;
                }
            }
            s_diag_last_pts = pts;
            s_diag_have_pts = true;
        }
        const bool frame_active_before = isoc_frame_active(uvc_stream);

        if (payload_data_len == 0) {
            /* 有些摄像头用仅含 UVC header 的包携带 FID/EOF/ERR。不能因为
             * 没有图像字节就跳过边界，否则完整 JPEG 会一直挂在 assembler。 */
            const bool header_fid_changed =
                (uvc_stream->single_thread.current_frame_id !=
                 payload_header->bmHeaderInfo.frame_id);
            if (frame_active_before) {
                s_diag_header_only_inside_active_frame++;
            }
            if (payload_header->bmHeaderInfo.error) {
                s_diag_frame_error++;
                if (header_fid_changed || !uvc_stream->single_thread.skip_current_frame) {
                    s_diag_drop_by_err++;
                }
                uvc_stream->single_thread.skip_current_frame = true;
            }
            if (header_fid_changed) {
                s_diag_frame_start_by_fid++;
                if (uvc_stream->dynamic.current_frame != NULL) {
                    if (uvc_stream->dynamic.vs_format.format == UVC_VS_FORMAT_MJPEG) {
                        uvc_stream->single_thread.skip_current_frame = true;
                    }
                    isoc_finish_frame(uvc_stream, ISOC_FINISH_FID);
                }
                uvc_stream->single_thread.current_frame_id =
                    payload_header->bmHeaderInfo.frame_id;
            }
            const bool frame_active_after = payload_header->bmHeaderInfo.end_of_frame ?
                                            false : isoc_frame_active(uvc_stream);
            const bool plain_header_only = !header_fid_changed &&
                                           !payload_header->bmHeaderInfo.end_of_frame &&
                                           !payload_header->bmHeaderInfo.error;
            if (plain_header_only && frame_active_before && !frame_active_after) {
                s_diag_frame_abort_by_header_only++;
            }
            if (first_nonempty_event || frame_active_before ||
                payload_header->bmHeaderInfo.end_of_frame ||
                payload_header->bmHeaderInfo.error || header_fid_changed) {
                isoc_diag_record_event(isoc_desc->actual_num_bytes, payload_header, pts,
                                       ISOC_DIAG_OFFSET_NONE, ISOC_DIAG_OFFSET_NONE,
                                       frame_active_before, frame_active_after);
            }
            if (payload_header->bmHeaderInfo.end_of_frame &&
                uvc_stream->dynamic.current_frame != NULL) {
                isoc_finish_frame(uvc_stream, ISOC_FINISH_EOF);
            }
            goto next_isoc_packet;
        }

        const int16_t soi_offset = isoc_diag_marker_offset(payload_data, payload_data_len, JPEG_SOI);
        const int16_t eoi_offset = isoc_diag_marker_offset(payload_data, payload_data_len, JPEG_EOI);
        const bool has_soi = soi_offset != ISOC_DIAG_OFFSET_NONE;
        const bool has_local_soi = soi_offset >= 0;
        const bool has_eoi = eoi_offset != ISOC_DIAG_OFFSET_NONE;
        if (has_soi) {
            s_diag_soi_count++;
            if (!s_diag_seen_soi) {
                s_diag_seen_soi = true;
                s_diag_first_soi_ms = isoc_diag_elapsed_ms();
            }
        }
        if (has_eoi) {
            s_diag_eoi_count++;
        }

        const bool fid_changed =
            (uvc_stream->single_thread.current_frame_id != payload_header->bmHeaderInfo.frame_id);

        // Check for error flag
        if (payload_header->bmHeaderInfo.error) {
            /* 摄像头自己报告本帧出错。走限频日志，否则它会按包重复刷屏。 */
            s_diag_frame_error++;
            if (fid_changed || !uvc_stream->single_thread.skip_current_frame) {
                s_diag_drop_by_err++;
            }
            uvc_stream->single_thread.skip_current_frame = true;
            isoc_diag_log_limited("摄像头报告帧错误");
        }

        if (!fid_changed && has_local_soi && uvc_stream->dynamic.current_frame != NULL) {
            /* 同一 FID 中出现新的 SOI：丢弃旧的未完成帧，并从这个 SOI 重新同步。
             * 这样既记录摄像头异常，也能验证只靠 JPEG 边界是否可以稳定组帧。 */
            uvc_stream->single_thread.skip_current_frame = true;
            isoc_finish_frame(uvc_stream, ISOC_FINISH_NEW_SOI);
        }
        if (fid_changed) {
            s_diag_frame_start_by_fid++;
            /*
             * FID is the authoritative boundary for UVC streams.  The EOF
             * bit is useful, but it is carried by the packet that may have
             * been lost on an isochronous endpoint.  Finish the old frame at
             * the FID transition instead of retaining it until the buffer
             * fills up.  A valid JPEG can still be delivered even when EOF
             * itself was lost.
            */
            if (uvc_stream->dynamic.current_frame != NULL) {
                /* MJPEG 到下一 FID 仍未见 EOF/EOI，数据结构不完整，不能把它
                 * 当作完成帧交给上层。非 MJPEG 仍保留原来的 FID 完成语义。 */
                if (uvc_stream->dynamic.vs_format.format == UVC_VS_FORMAT_MJPEG) {
                    uvc_stream->single_thread.skip_current_frame = true;
                }
                isoc_finish_frame(uvc_stream, ISOC_FINISH_FID);
            }
            uvc_stream->single_thread.current_frame_id = payload_header->bmHeaderInfo.frame_id;
        }

        const bool start_of_frame = fid_changed ||
                                    (has_local_soi && uvc_stream->dynamic.current_frame == NULL);
        if (start_of_frame) {
            uvc_stream->single_thread.skip_current_frame = payload_header->bmHeaderInfo.error;
            uvc_stream->single_thread.frame_in_progress = true;

            if (uvc_stream->dynamic.vs_format.format == UVC_VS_FORMAT_MJPEG) {
                const size_t frame_soi_offset = jpeg_find_marker(payload_data, payload_data_len,
                                                                 JPEG_SOI);
                if (frame_soi_offset == SIZE_MAX) {
                    uvc_stream->single_thread.skip_current_frame = true;
                    s_diag_drop_by_missing_soi++;
                    ESP_LOGD(TAG, "discarding MJPEG frame without SOI");
                } else {
                    s_diag_frame_start_by_soi++;
                    if (frame_soi_offset != 0) {
                        ESP_LOGD(TAG, "resynchronized MJPEG SOI after %u bytes",
                                 (unsigned)frame_soi_offset);
                        payload_data += frame_soi_offset;
                        payload_data_len -= frame_soi_offset;
                    }
                }
            }

            // Get free frame buffer for this new frame
            UVC_ENTER_CRITICAL();
            const bool need_new_frame = (uvc_stream->dynamic.streaming && !uvc_stream->dynamic.current_frame);
            UVC_EXIT_CRITICAL();
            if (need_new_frame) {
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
                s_diag_frame_buffer_capacity =
                    (uint32_t)uvc_stream->dynamic.current_frame->data_buffer_len;
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
                s_diag_drop_by_overflow++;

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
            s_diag_current_frame_len = (uint32_t)current_frame->data_len;
            if (s_diag_current_frame_len > s_diag_max_frame_len) {
                s_diag_max_frame_len = s_diag_current_frame_len;
            }
        }

        if (payload_data_len > 0) {
            s_diag_previous_payload_byte = payload_data[payload_data_len - 1];
            s_diag_previous_payload_byte_valid = true;
        }

        const bool fid_event = fid_changed;
        if (first_nonempty_event || fid_event || payload_header->bmHeaderInfo.end_of_frame ||
            payload_header->bmHeaderInfo.error || has_soi || has_eoi) {
            isoc_diag_record_event(isoc_desc->actual_num_bytes, payload_header, pts,
                                   soi_offset, eoi_offset,
                                   frame_active_before,
                                   (payload_header->bmHeaderInfo.end_of_frame || has_eoi) ?
                                   false : isoc_frame_active(uvc_stream));
        }

        // End of Frame. Pass the frame to user
        if (payload_header->bmHeaderInfo.end_of_frame) {
            /* Explicit EOF remains the normal completion path. */
            isoc_finish_frame(uvc_stream, ISOC_FINISH_EOF);
        } else if (has_eoi) {
            /* 某些 UVC 设备 JPEG 数据完整却不可靠地置 EOF，用 EOI 作为保底边界。 */
            isoc_finish_frame(uvc_stream, ISOC_FINISH_EOI);
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
