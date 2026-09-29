#pragma once

#include "driver/jpeg_decode.h"
#include "jpeg_frame_trace.h"

void jpeg_failure_probe_compare(const jpeg_frame_trace_t *uvc,
    const uint8_t *input, size_t size, uint32_t sequence);

/* 仅捕获本次开机第一次硬解失败，所有数据副本使用 PSRAM。 */
esp_err_t jpeg_failure_probe_process(jpeg_decoder_handle_t decoder,
    const jpeg_decode_cfg_t *config, const uint8_t *input, uint32_t size,
    uint8_t *output, uint32_t output_size, uint32_t *decoded_size,
    uint32_t sequence);

/* 仅在视频 IDLE 时分块导出，不在推流期间打印 JPEG 数据。 */
void jpeg_failure_probe_export_step(void);
