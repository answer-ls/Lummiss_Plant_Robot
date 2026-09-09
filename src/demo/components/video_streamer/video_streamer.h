#ifndef LUMMISS_VIDEO_STREAMER_H
#define LUMMISS_VIDEO_STREAMER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* H.264 实时预览的目标分辨率。编码器分辨率、JPEG 解码尺寸校验、
 * 摄像头侧"可编码帧"门控三处共用此常量，改动实时预览分辨率只需改这里。
 * 约束：宽高必须是偶数（YUV 重排按像素对处理）。 */
#define VIDEO_STREAM_WIDTH   800
#define VIDEO_STREAM_HEIGHT  600
/* 摄像头 UVC 帧缓冲和视频输入环槽共用同一个上限，避免 UVC 协商值偏小
 * 截断复杂画面的 MJPEG 帧。保留 512KB 便于统计异常大帧，并为后续切换
 * 分辨率留出余量。 */
#define VIDEO_STREAM_JPEG_MAX_SIZE (512U * 1024U)

/* 初始化实时视频流水线（双任务并行，各自钉核）：
 *
 *   UVC 帧回调 submit_jpeg()  →  MJPEG 环槽 ×2
 *   video_codec 任务（核 1）：JPEG 解码 → YUV 重排 → H.264 编码
 *          ↓ H.264 输出槽 ×4（free / ready 两条队列交接，无额外拷贝）
 *   video_upload 任务（核 0）：HTTP POST 上传，失败自动重连，完成后归还槽
 *
 * 编码与网络发送拆开并行后不再互相等待：HTTP 慢时只会在编码前丢
 * MJPEG 输入帧（码流连续，PC 端不会花屏），不丢弃已编码帧。
 * 数据链路：MJPEG (VIDEO_STREAM_WIDTH×VIDEO_STREAM_HEIGHT) → JPEG 硬件直出
 *           YUV422 → 轻量抽样/重排 O_UYY_E_VYY → esp_h264 硬件编码 → HTTP 上传。 */
esp_err_t video_streamer_init(void);

/* 非阻塞提交一张 VIDEO_STREAM_WIDTH×VIDEO_STREAM_HEIGHT MJPEG 压缩帧。
 * 函数会在返回前将选中的帧复制到 PSRAM，UVC 回调可立即归还原帧。
 * 受 VIDEO_ENCODE_FPS 帧率门控限制，超出的帧直接丢弃。 */
bool video_streamer_submit_jpeg(const uint8_t *data,
                                size_t data_len);

#endif /* LUMMISS_VIDEO_STREAMER_H */
