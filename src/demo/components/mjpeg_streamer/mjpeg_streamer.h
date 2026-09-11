#ifndef LUMMISS_MJPEG_STREAMER_H
#define LUMMISS_MJPEG_STREAMER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* 摄像头和网页直通链路共用的编译期格式。 */
#define VIDEO_STREAM_WIDTH          800
#define VIDEO_STREAM_HEIGHT         600
#define VIDEO_STREAM_JPEG_MAX_SIZE  (512U * 1024U)

esp_err_t mjpeg_streamer_init(void);

/* 非阻塞提交一张完整 MJPEG/JPEG 帧；函数返回前复制到 PSRAM 队列槽。 */
bool mjpeg_streamer_submit_jpeg(const uint8_t *data, size_t data_len);

void mjpeg_streamer_set_enabled(bool enabled);
bool mjpeg_streamer_is_enabled(void);

#endif /* LUMMISS_MJPEG_STREAMER_H */
