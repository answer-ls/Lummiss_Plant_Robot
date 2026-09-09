#ifndef LUMMISS_VIDEO_STREAMER_H
#define LUMMISS_VIDEO_STREAMER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* 初始化实时视频流水线（双任务并行，各自钉核）：
 *
 *   UVC 帧回调 submit_jpeg()  →  MJPEG 环槽 ×2
 *   video_codec 任务（核 1）：JPEG 解码 → YUV 重排 → H.264 编码
 *          ↓ H.264 输出槽 ×4（free / ready 两条队列交接，无额外拷贝）
 *   video_upload 任务（核 0）：HTTP POST 上传，失败自动重连，完成后归还槽
 *
 * 编码与网络发送拆开并行后不再互相等待：HTTP 慢时只会在编码前丢
 * MJPEG 输入帧（码流连续，PC 端不会花屏），不丢弃已编码帧。
 * 数据链路：MJPEG 640×480 → JPEG 硬件直出 YUV422 → 轻量抽样/重排 O_UYY_E_VYY →
 *           esp_h264 硬件编码 → HTTP 上传。 */
esp_err_t video_streamer_init(void);

/* 非阻塞提交一张 640×480 MJPEG 压缩帧。
 * 函数会在返回前将选中的帧复制到 PSRAM，UVC 回调可立即归还原帧。
 * 受 VIDEO_ENCODE_FPS 帧率门控限制，超出的帧直接丢弃。 */
bool video_streamer_submit_jpeg(const uint8_t *data,
                                size_t data_len);

#endif /* LUMMISS_VIDEO_STREAMER_H */
