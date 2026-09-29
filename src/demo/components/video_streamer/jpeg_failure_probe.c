#include "jpeg_failure_probe.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"


/* 两份副本独立于摄像头缓冲，不改变送入硬件的指针、长度或缓存策略。 */
static uint8_t *s_before, *s_after;
static size_t s_capacity, s_offset;
static uint32_t s_size, s_sequence, s_decoded, s_mismatch, s_before_hash, s_after_hash;
static int s_first_diff, s_error, s_format;
static int64_t s_begin_us, s_end_us;
static bool s_captured, s_disabled, s_header_sent, s_done;
static unsigned s_export_stream;
static jpeg_frame_trace_t s_uvc_trace, s_decode_trace;
static uint32_t s_trace_sequence;
static bool s_trace_reported;

static void trace_print(const char *stage, const jpeg_frame_trace_t *r)
{
    ESP_LOGW("JPEG_TRACE", "%s seq=%" PRIu32 " uvc_id=%" PRIu32
        " len=%" PRIu32 " hash=%08" PRIx32 " rst_hash=%08" PRIx32
        " rst=%" PRIu32 "/%" PRIu32 " order=%d first_bad=%" PRIu32
        " expected=%u actual=%u dri=%" PRIu32 " wh=%" PRIu32 "x%" PRIu32
        " eoi_end=%" PRIu32 " header_ok=%d inspect_us=%" PRIu32,
        stage, s_trace_sequence, s_uvc_trace.id, r->bytes, r->hash, r->rst_hash,
        r->rst_count, r->expected_rst, r->order_ok, r->first_bad_offset,
        r->expected_marker, r->actual_marker, r->dri, r->width, r->height,
        r->eoi_end, r->header_ok, r->inspect_us);
}

static void trace_report(void)
{
    /* USB 侧没有扫描，不能把零值 hash 当作有效对比结果。 */
    ESP_LOGW("JPEG_TRACE", "UVC_METADATA seq=%" PRIu32 " uvc_id=%" PRIu32
        " len=%" PRIu32 " callback_scan=OFF payload_scan=OFF entropy_filter=ON",
        s_trace_sequence, s_uvc_trace.id, s_uvc_trace.bytes);
    trace_print("PRE_DECODE", &s_decode_trace);
    const uvc_frame_audit_t *a = &s_uvc_trace.assembly;
    static const char *const reasons[] = {"EOF", "FID", "EOI", "NEW_SOI"};
    ESP_LOGW("JPEG_TRACE", "ASSEMBLY seq=%" PRIu32 " uvc_id=%" PRIu32
        " valid=%d frame=%" PRIu32 " finish=%s packets=%" PRIu32 "..%" PRIu32
        " fid=%" PRIu32 " start_fid=%" PRIu32 " start_soi=%" PRIu32
        " skip_cleared_at_start=%" PRIu32 " start_error=%" PRIu32 " final_skip=%" PRIu32,
        s_trace_sequence, s_uvc_trace.id, a->valid, a->frame_id,
        a->finish_reason < 4 ? reasons[a->finish_reason] : "UNKNOWN",
        a->packet_first, a->packet_last, a->fid, a->start_fid, a->start_soi,
        a->start_skip_cleared, a->start_error, a->final_skip);
    ESP_LOGW("JPEG_TRACE", "PACKETS seq=%" PRIu32 " appended=%" PRIu32
        " bytes=%" PRIu32 " delivered=%" PRIu32 " append_fail=%" PRIu32
        " error=%" PRIu32 " timeout=%" PRIu32 " skipped=%" PRIu32
        " bad_header=%" PRIu32 " camera_error=%" PRIu32
        " soi=%" PRIu32 " eoi=%" PRIu32 " eof=%" PRIu32,
        s_trace_sequence, a->appended_packets, a->appended_bytes, a->delivered_bytes,
        a->append_fail, a->packet_error, a->timeout, a->skipped, a->invalid_header,
        a->camera_error, a->soi_hits, a->eoi_hits, a->eof_seen);
    ESP_LOGW("JPEG_TRACE", "PTS seq=%" PRIu32 " present=%" PRIu32
        " first=%" PRIu32 " last=%" PRIu32 " changes=%" PRIu32
        " first_change_offset=%" PRIu32 " ends_frame=0",
        s_trace_sequence, a->pts_valid, a->pts_first, a->pts_last,
        a->pts_changes, a->pts_change_offset);
    /* 仅在异常帧报告中输出有限包窗口，避免逐包串口日志干扰 USB。 */
    for (uint32_t i = 0; a->pts_changes && i < a->pts_packet_count; ++i) {
        const uvc_pts_packet_t *p = &a->pts_packets[i];
        ESP_LOGW("JPEG_TRACE", "PTS_PACKET seq=%" PRIu32 " relative=%d packet=%" PRIu32
            " transfer=%" PRIu32 " slot=%u offset=%" PRIu32 " actual=%" PRIu32
            " header=%u flags=0x%02x payload=%" PRIu32 " pts=%" PRIu32
            " first4=%08" PRIx32 " last4=%08" PRIx32,
            s_trace_sequence, (int)i - (int)a->pts_trigger_index, p->packet,
            p->transfer, p->transfer_slot, p->offset, p->actual_bytes,
            p->header_bytes, p->flags, p->payload_bytes, p->pts, p->prefix, p->suffix);
    }

}

void jpeg_failure_probe_compare(const jpeg_frame_trace_t *uvc,
    const uint8_t *input, size_t size, uint32_t sequence)
{
    if (s_captured) return;
    s_uvc_trace = *uvc;
    const int64_t begin = esp_timer_get_time();
    s_decode_trace = jpeg_frame_trace_scan(input, size);
    s_decode_trace.inspect_us = (uint32_t)(esp_timer_get_time() - begin);
    s_trace_sequence = sequence;
    if (!s_trace_reported &&
        (uvc->bytes != size ||
         !s_decode_trace.header_ok || !s_decode_trace.order_ok ||
         !s_decode_trace.eoi_end || s_decode_trace.rst_count != s_decode_trace.expected_rst)) {
        trace_report();
        s_trace_reported = true;
    }
}

static uint32_t probe_hash(const uint8_t *data, size_t size)
{
    uint32_t hash = 2166136261U;
    for (size_t i = 0; i < size; ++i) hash = (hash ^ data[i]) * 16777619U;
    return hash;
}

esp_err_t jpeg_failure_probe_process(jpeg_decoder_handle_t decoder,
    const jpeg_decode_cfg_t *config, const uint8_t *input, uint32_t size,
    uint8_t *output, uint32_t output_size, uint32_t *decoded_size,
    uint32_t sequence)
{
    bool armed = !s_captured && !s_disabled;
    if (armed && size > s_capacity) {
        /* 诊断内存不足时只停诊断，不能跳过或改变业务解码。 */
        heap_caps_free(s_before);
        heap_caps_free(s_after);
        s_before = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        s_after = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_before || !s_after) {
            heap_caps_free(s_before);
            heap_caps_free(s_after);
            s_before = s_after = NULL;
            s_disabled = true;
            armed = false;
            ESP_LOGE("JPEG_PROBE", "PSRAM副本分配失败，诊断停用；业务解码继续");
        } else {
            s_capacity = size;
        }
    }
    if (armed) memcpy(s_before, input, size);
    const int64_t begin = esp_timer_get_time();
    const esp_err_t ret = jpeg_decoder_process(decoder, config, (uint8_t *)input,
                                               size, output, output_size, decoded_size);
    const int64_t end = esp_timer_get_time();
    if (armed && ret != ESP_OK) {
        /* 必须在输入槽归还或另一次解码之前保存失败现场。 */
        memcpy(s_after, input, size);
        s_size = size;
        s_sequence = sequence;
        s_error = ret;
        s_format = config->output_format;
        s_decoded = *decoded_size;
        s_begin_us = begin;
        s_end_us = end;
        s_first_diff = -1;
        for (uint32_t i = 0; i < size; ++i) {
            if (s_before[i] != s_after[i]) {
                if (s_first_diff < 0) s_first_diff = (int)i;
                ++s_mismatch;
            }
        }
        s_before_hash = probe_hash(s_before, size);
        s_after_hash = probe_hash(s_after, size);
        s_captured = true;
        /* 硬解失败帧再次输出自己的关联快照，不能拿之前另一坏帧作比较。 */
        trace_report();
        ESP_LOGW("JPEG_PROBE", "CAPTURED sequence=%" PRIu32 " bytes=%" PRIu32
                 " ret=%s mismatch=%" PRIu32 " first_diff=%d；请停止RTC等待JP_DONE，勿复位",
                 sequence, size, esp_err_to_name(ret), s_mismatch, s_first_diff);
    }
    return ret;
}

void jpeg_failure_probe_export_step(void)
{
    if (!s_captured || s_done) return;
    if (!s_header_sent) {
        printf("JP_BEGIN {\"size\":%" PRIu32 ",\"sequence\":%" PRIu32
               ",\"ret\":%d,\"output_format\":%d,\"decoded_size\":%" PRIu32
               ",\"begin_us\":%" PRId64 ",\"end_us\":%" PRId64
               ",\"mismatch\":%" PRIu32 ",\"first_diff\":%d"
               ",\"before_hash\":%" PRIu32 ",\"after_hash\":%" PRIu32 "}\n",
               s_size, s_sequence, s_error, s_format, s_decoded, s_begin_us,
               s_end_us, s_mismatch, s_first_diff, s_before_hash, s_after_hash);
        s_header_sent = true;
        return;
    }
    if (s_export_stream < 2) {
        /* 单行一次 printf，降低其他任务日志插入二进制导出行的风险。 */
        char hex[513];
        const char digits[] = "0123456789abcdef";
        const uint8_t *data = s_export_stream == 0 ? s_before : s_after;
        size_t length = s_size - s_offset;
        if (length > 256) length = 256;
        for (size_t i = 0; i < length; ++i) {
            hex[2*i] = digits[data[s_offset+i] >> 4];
            hex[2*i+1] = digits[data[s_offset+i] & 15];
        }
        hex[2*length] = 0;
        printf("JP_DATA %u %u %s\n", s_export_stream, (unsigned)s_offset, hex);
        s_offset += length;
        if (s_offset == s_size) {
            s_offset = 0;
            ++s_export_stream;
        }
        return;
    }
    printf("JP_DONE\n");
    heap_caps_free(s_before);
    heap_caps_free(s_after);
    s_before = s_after = NULL;
    s_done = true;
}
