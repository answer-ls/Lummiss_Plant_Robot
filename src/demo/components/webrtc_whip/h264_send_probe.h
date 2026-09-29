#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
/* 所有调用均由 WHIP 的 s_guard 串行保护；只读原始提交缓冲。 */
void h264_send_probe_before(const uint8_t *data, size_t size, uint32_t pts, bool idr);
void h264_send_probe_after(int result);
void h264_send_probe_export_step(void);
