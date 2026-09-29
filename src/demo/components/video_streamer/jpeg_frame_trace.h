#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "esp_private/uvc_frame_audit.h"

/* 随帧传递数值快照，不保存 UVC 缓冲指针，也不额外占用 DMA 内存。 */
typedef struct {
    uint32_t id, bytes, hash, rst_hash, rst_count, expected_rst;
    uint32_t dri, width, height, eoi_end, first_bad_offset;
    uint32_t inspect_us;
    uvc_frame_audit_t assembly;
    uint8_t expected_marker, actual_marker;
    bool valid, header_ok, order_ok;
} jpeg_frame_trace_t;

/* 只检查 baseline 单扫描结构和 RST 顺序，不把它当作熵解码验证。 */
static inline jpeg_frame_trace_t jpeg_frame_trace_scan(const uint8_t *data, size_t size)
{
    jpeg_frame_trace_t r = {.bytes = size, .hash = 2166136261U,
        .rst_hash = 2166136261U, .valid = true, .order_ok = true};
    for (size_t k = 0; k < size; ++k) r.hash = (r.hash ^ data[k]) * 16777619U;
    if (size < 4 || data[0] != 255 || data[1] != 216) return r;
    size_t i = 2;
    unsigned max_h = 0, max_v = 0;
    bool baseline = false, scan = false;
    while (i + 4 <= size && data[i] == 255) {
        uint8_t marker = data[i+1];
        size_t n = ((size_t)data[i+2] << 8) | data[i+3];
        if (n < 2 || n > size-i-2) return r;
        if (marker == 0xc0 && n >= 8) {
            r.height = ((uint32_t)data[i+5] << 8) | data[i+6];
            r.width = ((uint32_t)data[i+7] << 8) | data[i+8];
            unsigned components = data[i+9];
            if (components != 3 || n != 8 + 3*components) return r;
            for (unsigned c = 0; c < components; ++c) {
                uint8_t sampling = data[i+11+3*c];
                if ((sampling >> 4) > max_h) max_h = sampling >> 4;
                if ((sampling & 15) > max_v) max_v = sampling & 15;
            }
            baseline = true;
        }
        if (marker == 0xdd && n == 4) r.dri = ((uint32_t)data[i+4] << 8) | data[i+5];
        i += n + 2;
        if (marker == 0xda) { scan = true; break; }
    }
    if (!scan || !baseline || !max_h || !max_v || !r.width || !r.height) return r;
    r.header_ok = true;
    uint32_t mcus = ((r.width+8*max_h-1)/(8*max_h))*((r.height+8*max_v-1)/(8*max_v));
    if (r.dri) r.expected_rst = (mcus-1)/r.dri;
    unsigned expected = 0;
    while (i + 1 < size) {
        if (data[i++] != 255) continue;
        size_t offset = i-1;
        while (i < size && data[i] == 255) ++i;
        if (i == size) break;
        uint8_t marker = data[i++];
        if (marker == 0) continue; /* FF00 是熵数据转义，不是标记。 */
        if (marker == 0xd9) { r.eoi_end = i; break; }
        if (marker < 0xd0 || marker > 0xd7) { r.header_ok = false; break; }
        unsigned actual = marker - 0xd0;
        r.rst_hash = (r.rst_hash ^ actual) * 16777619U;
        ++r.rst_count;
        if (actual != expected && r.order_ok) {
            r.order_ok = false;
            r.first_bad_offset = offset;
            r.expected_marker = expected;
            r.actual_marker = actual;
        }
        expected = (actual+1)&7;
    }
    return r;
}
