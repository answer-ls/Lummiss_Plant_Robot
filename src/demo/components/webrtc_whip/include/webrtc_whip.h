#ifndef LUMMISS_WEBRTC_WHIP_H
#define LUMMISS_WEBRTC_WHIP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define WEBRTC_WHIP_URL_MAX      768
#define WEBRTC_WHIP_KEY_MAX      256
#define WEBRTC_WHIP_ROOM_MAX     128
#define WEBRTC_WHIP_SESSION_MAX  80
#define WEBRTC_WHIP_CREDENTIAL_PATH_MAX  384
#define WEBRTC_WHIP_DEVICE_TICKET_MAX    512

typedef enum {
    WEBRTC_WHIP_IDLE = 0,
    WEBRTC_WHIP_STARTING,
    WEBRTC_WHIP_CONNECTING,
    WEBRTC_WHIP_DTLS_CONNECTED,
    WEBRTC_WHIP_MEDIA_PREPARING,
    WEBRTC_WHIP_H264_OPENING,
    WEBRTC_WHIP_STREAMING,
    WEBRTC_WHIP_STOPPING,
} webrtc_whip_state_t;

typedef struct {
    /* MCP start 只携带一次性取证参数；publish_url 等字段由后台 HTTPS GET 填充。 */
    char credential_path[WEBRTC_WHIP_CREDENTIAL_PATH_MAX];
    char device_ticket[WEBRTC_WHIP_DEVICE_TICKET_MAX];
    char publish_url[WEBRTC_WHIP_URL_MAX];
    char stream_key[WEBRTC_WHIP_KEY_MAX];
    char room_name[WEBRTC_WHIP_ROOM_MAX];
    char session_id[WEBRTC_WHIP_SESSION_MAX];
    uint16_t width;
    uint16_t height;
    uint8_t fps;
    bool video;
    bool audio;
} webrtc_whip_credential_t;

/* 初始化控制任务，不打开摄像头或发起网络请求。 */
esp_err_t webrtc_whip_init(void);
/* 通过回调协调共享 AFE 生命周期，避免组件循环依赖。 */
void webrtc_whip_set_audio_control(esp_err_t (*suspend)(bool));

/* RTC 建链前置条件。摄像头条件随 UVC 流失效/恢复而变化；
 * 存储条件在初始化完成后保持成立。通知不分配内存、不启动网络操作。 */
void webrtc_whip_notify_camera_ready(void);
void webrtc_whip_notify_camera_unready(void);
void webrtc_whip_notify_storage_init_done(void);

/* MCP 线程只复制凭据并投递命令；HTTP、ICE、DTLS 均由后台任务处理。 */
esp_err_t webrtc_whip_request_start(const webrtc_whip_credential_t *credential);
esp_err_t webrtc_whip_request_stop(const char *session_id);
webrtc_whip_state_t webrtc_whip_get_state(void);
/* 仅停止预览后分块导出诊断，不在发送期间打印码流。 */
void webrtc_whip_export_h264_probe_step(void);

/* 由 video_upload 任务提交纯 Annex-B 帧；内部 esp_peer 负责 RTP/SRTP。 */
esp_err_t webrtc_whip_send_h264(const uint8_t *annex_b, size_t length,
                                uint32_t pts_ms, bool idr);

/* 控制任务在 ICE+DTLS 就绪时切换编码门控，停止时只停编码、不停 UVC。 */
void webrtc_whip_set_video_control(esp_err_t (*start)(void),
                                   esp_err_t (*stop)(void),
                                   void (*force_idr)(void));

/* 按需摄像头：收到预览请求后启动 UVC；控制任务轮询首帧就绪状态。
 * 未注册时沿用开机启动摄像头并主动通知 CAMERA_READY 的旧流程。 */
void webrtc_whip_set_camera_control(esp_err_t (*start)(void),
                                    bool (*is_ready)(void));

/* PeerConnection 创建前回收动画等非实时临时资源。回调在 WHIP 控制任务执行。 */
void webrtc_whip_set_resource_prepare(
    esp_err_t (*prepare)(uint32_t timeout_ms));

#endif
