#ifndef LUMMISS_CLOUD_MQTT_H
#define LUMMISS_CLOUD_MQTT_H

/* 三通道新协议的通道一：MQTT 控制与信令。
 *
 * 依据《ESP32-嵌入式三通道接入实施文档》v1.0（2026-09-18）：
 *   - MQTT 只传 JSON 控制 / MCP / 状态 / WebRTC SDP·ICE 信令，
 *     **不传音频帧、不传视频帧**；
 *   - 日常 AI 语音走 UDP + Opus（阶段二），视频走 WebRTC + H.264（阶段四）；
 *   - 设备不再常驻 Agent WebSocket。
 *
 * 本组件当前实现的是**阶段一**：OTA 配置 → 连接 → 订阅 → AI hello v3 →
 * 收 Server Hello（session_id + UDP 参数）→ AI_SESSION_READY。
 * UDP 音频、MCP、WebRTC 尚未实现，因此也不得出现在 tools/list 里。 */

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/* MQTT 六字段，全部原样来自 OTA 响应（文档 §2.3）：
 * 不拼接 client_id、不解析 username 内部结构、不重新计算 password。 */
#define CLOUD_MQTT_ENDPOINT_MAX_LEN   64
#define CLOUD_MQTT_CLIENT_ID_MAX_LEN  128
#define CLOUD_MQTT_USERNAME_MAX_LEN   256
#define CLOUD_MQTT_PASSWORD_MAX_LEN   256
#define CLOUD_MQTT_TOPIC_MAX_LEN      128

typedef struct {
    char endpoint[CLOUD_MQTT_ENDPOINT_MAX_LEN];      /* "60.210.30.199:1883" */
    char client_id[CLOUD_MQTT_CLIENT_ID_MAX_LEN];
    char username[CLOUD_MQTT_USERNAME_MAX_LEN];
    char password[CLOUD_MQTT_PASSWORD_MAX_LEN];
    char publish_topic[CLOUD_MQTT_TOPIC_MAX_LEN];
    char subscribe_topic[CLOUD_MQTT_TOPIC_MAX_LEN];
} cloud_mqtt_config_t;

/* 启动状态机（文档 §2）。顺序即验收顺序，串口按这个顺序打印。 */
typedef enum {
    CLOUD_STATE_BOOT = 0,
    CLOUD_STATE_NTP_TIME_READY,
    CLOUD_STATE_OTA_CONFIGURED,
    CLOUD_STATE_MQTT_CONNECTED,
    CLOUD_STATE_MQTT_SUBSCRIBED,
    CLOUD_STATE_AI_HELLO_SENT,
    CLOUD_STATE_AI_SESSION_READY,
    CLOUD_STATE_IDLE,
} cloud_state_t;

/* UDP 协商结果（Server Hello 下发），阶段二使用。
 * key/nonce 已按十六进制解码成字节；**不打印其数值**。 */
typedef struct {
    char session_id[64];
    char udp_server[64];
    uint16_t udp_port;
    char udp_encryption[24];
    bool has_key;
    uint8_t key[16];
    uint32_t key_hex_len;      /* 只用于校验/日志，不输出内容 */
    bool has_nonce;
    uint8_t nonce[16];
    uint32_t nonce_hex_len;
    uint16_t downlink_sample_rate;
    uint8_t downlink_channels;
    uint16_t downlink_frame_duration;
} cloud_mqtt_session_t;

/* 用 OTA 拿到的六字段启动 MQTT。返回后连接是异步建立的。 */
esp_err_t cloud_mqtt_start(const cloud_mqtt_config_t *config);

/* 重新发布 hello v3 开一个**新会话**（新 session_id + 新 UDP key），
 * 用于「一次唤醒 = 一次会话」的生命周期：服务端空闲超时后旧会话作废，
 * 下次唤醒必须重新协商。MQTT 连接本身保持不动。
 * 调用后 session_ready 清零，收到新的 Server Hello 后重新置位。 */
esp_err_t cloud_mqtt_reopen_session(void);

/* 设备主动结束会话时向服务端回发 goodbye（对齐上游 CloseAudioChannel(true)）。
 * 无活动会话时为空操作。注意：收到**服务端**的 goodbye 时不要调用本函数
 * 回发 —— 会造成乒乓（上游注释明确警告）。 */
esp_err_t cloud_mqtt_send_goodbye(void);

/* 由 main 在 SNTP 校时完成后调用，把状态机推进到 NTP_TIME_READY。
 * 文档 §2 的启动顺序：WiFi → NTP → OTA → MQTT。 */
void cloud_mqtt_notify_ntp_ready(void);

/* 下行业务文本（stt / tts / llm / listen / system / custom …）回调。
 * hello 由本组件自己处理（会话参数），mcp 交给 cloud_mcp；
 * 其余类型原样转给这里 —— 语音业务的状态机在 xiaozhi_audio。
 * 回调在 MQTT 任务上下文执行，只许做解析和状态更新，不许做耗时操作。 */
typedef void (*cloud_mqtt_text_cb_t)(const char *json, size_t len, void *ctx);
void cloud_mqtt_set_text_callback(cloud_mqtt_text_cb_t cb, void *ctx);

cloud_state_t cloud_mqtt_get_state(void);
const char *cloud_mqtt_state_name(cloud_state_t state);

/* 收到 Server Hello（含 session_id 与 UDP 参数）后为 true。
 * 在此之前**不能**认为小智在线，也不允许开始发送音频。 */
bool cloud_mqtt_is_session_ready(void);

/* 取一份 Server Hello 协商结果快照。未就绪时返回 false。 */
bool cloud_mqtt_get_session(cloud_mqtt_session_t *out);

/* 发布到 OTA 的 publish_topic。JSON 文本，QoS 0，retain=false。 */
esp_err_t cloud_mqtt_publish_text(const char *text);

#endif
