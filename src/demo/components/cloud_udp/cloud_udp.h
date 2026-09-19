#ifndef LUMMISS_CLOUD_UDP_H
#define LUMMISS_CLOUD_UDP_H

/* 三通道新协议的通道二：UDP + AES-128-CTR 承载 AI 对话的 Opus 音频。
 *
 * 依据《ESP32-嵌入式三通道接入实施文档》§4。上行只发、下行只收 Opus，
 * **不许把 JSON / MCP 塞进 UDP**（文档 §4.2 第 6 条）。
 *
 * 报文格式（16 字节头 + 加密后的 Opus，多字节字段大端）：
 *   0   type(1)=1 | 1 flags(1)=0 | 2 payload_length(2) | 4 connection_id(4)
 *   8   timestamp(4) | 12 sequence(4) | 16.. AES-CTR 加密的 Opus
 *
 * 加密：key 十六进制解码成 16 字节 AES key；**当前网关用本数据包 16 字节头
 * 本身作为 AES-CTR 的 IV**（文档 §4.2 第 2 条），nonce 字段只作协议兼容保留，
 * 不要拿它替换包头当 IV。 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "cloud_mqtt.h"   /* cloud_mqtt_session_t：UDP 参数来自 MQTT hello */

/* 下行 Opus 回调。在 UDP 接收任务上下文里被调用，必须快速返回。 */
typedef void (*cloud_udp_audio_cb_t)(const uint8_t *opus, size_t len, void *ctx);

/* 用 Server Hello 协商出的参数建立 UDP 通道（server/port/encryption/key）。
 * 幂等：已启动时直接返回 ESP_OK。参数不合法（缺 key、非 aes-128-ctr）返回错误，
 * 绝不以明文继续发送。 */
esp_err_t cloud_udp_start(const cloud_mqtt_session_t *session);

/* 关闭通道并释放 socket / 任务。幂等。 */
void cloud_udp_stop(void);

bool cloud_udp_is_ready(void);

/* 注册下行 Opus 回调（在 start 之前或之后都可以）。 */
void cloud_udp_set_audio_callback(cloud_udp_audio_cb_t cb, void *ctx);

/* 上行：发一帧 Opus。内部完成 打包 → AES-128-CTR → sendto。
 * 未就绪返回 ESP_ERR_INVALID_STATE（调用方可据此计数，不算错误）。 */
esp_err_t cloud_udp_send_opus(const uint8_t *opus, size_t len);

#endif
