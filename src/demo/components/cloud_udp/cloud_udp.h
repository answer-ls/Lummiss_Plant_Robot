#ifndef LUMMISS_CLOUD_UDP_H
#define LUMMISS_CLOUD_UDP_H

/* 三通道新协议的通道二：UDP + AES-128-GCM 承载 AI 对话的 Opus 音频，
 * 同时保留 AES-128-CTR 迁移兼容路径。
 *
 * 依据《ESP32-嵌入式三通道接入实施文档》§4。上行只发、下行只收 Opus，
 * **不许把 JSON / MCP 塞进 UDP**（文档 §4.2 第 6 条）。
 *
 * GCM 报文格式（多字节字段大端）：
 *   16 字节头 + AES-GCM 密文 Opus + 16 字节认证标签
 *   0   type(1)=1 | 1 flags(1)=1 | 2 payload_length(2) | 4 connection_id(4)
 *   8   timestamp(4) | 12 sequence(4) | 16.. GCM 密文 | 尾部 Tag(16)
 *
 * GCM：key 为 Server Hello 的 16 字节 key；IV=header[4..15]（12 字节），
 * AAD=完整 16 字节 header，Tag 固定 16 字节。payload_length 是 Opus 明文长度，
 * 不包含 header 和 Tag。Tag 验证失败的下行包绝不送入 Opus。
 *
 * CTR 兼容：flags=0，完整 16 字节 header 作为 CTR IV，无尾部 Tag。 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "cloud_mqtt.h"   /* cloud_mqtt_session_t：UDP 参数来自 MQTT hello */

/* 下行 Opus 回调。在 UDP 接收任务上下文里被调用，必须快速返回。 */
typedef void (*cloud_udp_audio_cb_t)(const uint8_t *opus, size_t len, void *ctx);

/* 用 Server Hello 协商出的参数建立 UDP 通道（server/port/encryption/key）。
 * 幂等：已启动时直接返回 ESP_OK。支持 aes-128-gcm 和迁移期 aes-128-ctr；
 * 参数不合法或加密方式未知时返回错误，绝不以明文继续发送。 */
esp_err_t cloud_udp_start(const cloud_mqtt_session_t *session);

/* 关闭通道并释放 socket / 任务。幂等。 */
void cloud_udp_stop(void);

bool cloud_udp_is_ready(void);

/* 注册下行 Opus 回调（在 start 之前或之后都可以）。 */
void cloud_udp_set_audio_callback(cloud_udp_audio_cb_t cb, void *ctx);

/* 上行：发一帧 Opus。内部按协商结果完成 GCM/CTR 打包、加密与 sendto。
 * 未就绪返回 ESP_ERR_INVALID_STATE（调用方可据此计数，不算错误）。 */
esp_err_t cloud_udp_send_opus(const uint8_t *opus, size_t len);

#endif
