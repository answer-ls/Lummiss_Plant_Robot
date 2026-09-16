#ifndef LUMMISS_VIDEO_STREAMER_H
#define LUMMISS_VIDEO_STREAMER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* 对照测试模式：三者最多启用一个。正式联网预览时全部改为 0。 */
#define VIDEO_STREAM_CODEC_ONLY_TEST 0
#define VIDEO_STREAM_JPEG_ONLY_TEST 0
#define VIDEO_STREAM_YUV_ONLY_TEST 0

#define VIDEO_STREAM_WS_URL_MAX_LEN    256
#define VIDEO_STREAM_WS_TOKEN_MAX_LEN  512
#define VIDEO_STREAM_WS_ID_MAX_LEN     64

typedef struct {
    char ws_url[VIDEO_STREAM_WS_URL_MAX_LEN];
    char token[VIDEO_STREAM_WS_TOKEN_MAX_LEN];
    char device_id[VIDEO_STREAM_WS_ID_MAX_LEN];
    char client_id[VIDEO_STREAM_WS_ID_MAX_LEN];
} video_streamer_config_t;

/* Agent WebSocket 与小智音频共用同一条连接，避免相同身份和 Token 建立
 * 两个会话。回调运行在 WebSocket 任务中，必须快速返回。 */
typedef struct {
    void (*connection_changed)(bool connected, void *ctx);
    void (*text_received)(const char *text, size_t len, void *ctx);
    void (*audio_received)(const uint8_t *data, size_t len, void *ctx);
    void *ctx;
} video_streamer_agent_callbacks_t;

#if (VIDEO_STREAM_CODEC_ONLY_TEST + VIDEO_STREAM_JPEG_ONLY_TEST + VIDEO_STREAM_YUV_ONLY_TEST) > 1
#error "Video test modes are mutually exclusive"
#endif

/* H.264 实时预览的目标分辨率。编码器分辨率、JPEG 解码尺寸校验、
 * 摄像头侧"可编码帧"门控三处共用此常量，改动实时预览分辨率只需改这里。
 * 约束：宽高必须是偶数（YUV 重排按像素对处理）。
 * 注意：目前仍是编译期常量，改分辨率要重新构建；运行期下发改分辨率尚未实现。 */
/* 800×600。2026-09-10 试过 1280×720，结论是**不值得**，别轻易再切回去。
 *
 * 两方面实测代价：
 *  1. 帧率：编解码链 800×600 约 31.3ms/帧（J 6.7 + Y 16.4 + H 8.2）→ 约 25 fps；
 *     720p 实机 60.8ms/帧（J 12.4 + Y 33.4 + H 15.0）→ 仅 15.3～16.3 fps。
 *  2. 清晰度**反而更差**：编码器每帧比特预算 = VIDEO_BITRATE / VIDEO_ENCODE_FPS
 *     = 4000000/30 = 133.3 kbit，与实际分辨率无关。800×600 得 0.28 bpp，
 *     720p 只有 0.146 bpp —— 像素多了 92%，每像素比特少了一半，画面明显发糊。
 *     要拉回 0.28 bpp 就得让线上跑满 4Mbps，而 WS 实测只有约 660 KB/s（5.3 Mbps）。
 *  3. 稳定性：UVC 组帧失步**与分辨率无关**，两个分辨率都有。720p 缺SOI 约 2.2%、
 *     3 次帧缓冲区溢出；800×600 复测缺SOI 仍达约 2.9%（13→75）、缺EOI 11、
 *     JPEG 解码失败 259 共 6 次。我一度写"800×600 这三项都是 0"，是**错的**——
 *     那只是更早一次短跑没撞上。降分辨率买不到帧完整性，得去查 EoF 检测与 URB 参数。
 *
 * 另外摄像头（LRCPG720p）实测声明最高只有 1280×960，**没有 1920×1080**。 */
#define VIDEO_STREAM_WIDTH   640
#define VIDEO_STREAM_HEIGHT  480
/* 单个 MJPEG 压缩帧的上限。这个值同时是 UVC 帧缓冲大小、视频输入环槽大小，
 * 以及 camera_driver 里"过大帧"的丢弃门限，三处必须保持一致。
 *
 * 别把 UVC_HOST_FRAME_BUFFER_OVERFLOW 读成"这一帧比缓冲区大"。真正的触发点是
 * uvc_frame.c 里的 uvc_frame_add_data()：
 *     UVC_CHECK(frame->data_len + data_len <= frame->data_buffer_len, ...)
 * 即「已累积字节 + 本次 URB 数据 > 本值」。驱动是在两次 SoF 之间一路累积直到
 * 认出 EoF 才收帧的，所以在没识别出 EoF 时会一直堆下去——也就是说这条事件的
 * 真实含义是**帧重组失步**，而不是单帧超限。单帧本来只有几十 KB
 * （800×600 实测 41～51KB），离这个上限还差一个数量级。
 *
 * 因此调大它救不了 overflow，反而有害：失步时会把更多垃圾数据攒进缓冲、
 * 污染更多后续帧。实测把这里从 512KB 改成 2MB 之后，缺SOI 从 0 升到 3%、
 * JPEG 解码错误变频繁、帧率从 29～30 掉到 23～27——已改回 512KB。
 *
 * 512KB 对 1280×720（约 80～100KB）仍有 5 倍余量，够用。真要治 overflow，
 * 该查 EoF 检测和 URB 参数（number_of_urbs / urb_size），不是这个缓冲大小。 */
#define VIDEO_STREAM_JPEG_MAX_SIZE (512U * 1024U)

/* 初始化实时视频流水线（双任务并行，各自钉核）：
 *
 *   UVC 帧回调 → MJPEG 独立复制池 → camera handoff FIFO → MJPEG 环槽 ×3
 *   video_codec 任务（核 1）：JPEG 解码 → YUV 重排 → H.264 编码
 *          ↓ H.264 输出槽 ×8（free / ready 两条队列交接，无额外拷贝）
 *   video_upload 任务（核 0）：WebSocket 上传，失败自动重连，完成后归还槽
 *
 * 编码与网络发送拆开并行后不再互相等待：正常发送慢时只会在编码前丢
 * MJPEG 输入帧；断线时清空过期 H.264 槽，PC 端等待下一个 IDR 重同步。
 * 数据链路：MJPEG (VIDEO_STREAM_WIDTH×VIDEO_STREAM_HEIGHT) → JPEG 硬件直出
 *           YUV422 → 轻量抽样/重排 O_UYY_E_VYY → esp_h264 硬件编码 → WebSocket 上传。
 * 每帧前景 16 字节自描述头（见 video_streamer.c），分辨率随帧携带，
 * PC 端不需要在连接建立时协商几何参数。 */
esp_err_t video_streamer_init(const video_streamer_config_t *config);

/* 如果在 video_streamer_init 之前调用，会在 WebSocket 上传任务启动时使用该配置。
 * 正式链路只接受 OTA 返回的 WSS 地址和动态 Token；未配置时不建立连接。
 * 典型用法：main.c 在 OTA 完成后调用此函数，再启动摄像头任务。 */
void video_streamer_set_config(const video_streamer_config_t *config);

/* 注册小智协议回调，并通过已鉴权的 Agent WSS 发送文本或原始 Opus 包。 */
void video_streamer_set_agent_callbacks(
    const video_streamer_agent_callbacks_t *callbacks);
esp_err_t video_streamer_agent_send_text(const char *text);
esp_err_t video_streamer_agent_send_audio(const uint8_t *data, size_t len);
/* 唤醒前音频需要按顺序完整进入发送队列，再发送 listen/detect。 */
esp_err_t video_streamer_agent_send_audio_wait(const uint8_t *data,
                                               size_t len,
                                               uint32_t timeout_ms);
esp_err_t video_streamer_agent_wait_audio_drain(uint32_t timeout_ms);

/* 当输入来自独立 MJPEG 复制池时，允许把该缓冲所有权交给编解码任务，
 * 避免 handoff 任务再次复制。release_cb 会在 JPEG 解码完成或输入被丢弃
 * 时调用；回调返回后，调用方不得再访问 release_ctx。 */
typedef void (*video_streamer_input_release_cb_t)(void *release_ctx);

/* 非阻塞提交一张 VIDEO_STREAM_WIDTH×VIDEO_STREAM_HEIGHT MJPEG 压缩帧。
 * 函数会在返回前将选中的帧复制到 PSRAM，UVC 回调可立即归还原帧。
 * 受 VIDEO_ENCODE_FPS 帧率门控限制，超出的帧直接丢弃。 */
bool video_streamer_submit_jpeg(const uint8_t *data,
                                size_t data_len);

/* 非阻塞提交一张由调用方拥有的 MJPEG 帧。成功时函数取得 data 的只读
 * 使用权，直到 release_cb 被调用；失败时不会取得所有权。 */
bool video_streamer_submit_jpeg_owned(
    const uint8_t *data,
    size_t data_len,
    video_streamer_input_release_cb_t release_cb,
    void *release_ctx);

/* 控制是否继续处理并上传视频帧。关闭时保留 WebSocket 控制通道，
 * 便于浏览器再次开启视频流；摄像头 USB 采集不会被重启。 */
void video_streamer_set_enabled(bool enabled);
bool video_streamer_is_enabled(void);

#endif /* LUMMISS_VIDEO_STREAMER_H */
