#ifndef LUMMISS_OTA_CLIENT_H
#define LUMMISS_OTA_CLIENT_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define OTA_WS_URL_MAX_LEN    256
#define OTA_TOKEN_MAX_LEN     512
#define OTA_FW_URL_MAX_LEN    512
#define OTA_ACT_MSG_MAX_LEN   256

/* MQTT 六字段（三通道新协议，见《ESP32-嵌入式三通道接入实施文档》§2.3）。
 * 全部**原样使用** OTA 返回值：不做拼接、不重新计算、不解析 username 内部
 * 结构。client_id / username / password 属于凭据，禁止写入日志。 */
/* MQTT 六字段（三通道新协议，见《ESP32-嵌入式三通道接入实施文档》§2.3）。
 * 全部**原样使用** OTA 返回值：不做拼接、不重新计算、不解析 username 内部
 * 结构。client_id / username / password 属于凭据，禁止写入日志。 */
#define OTA_MQTT_ENDPOINT_MAX_LEN   64
#define OTA_MQTT_CLIENT_ID_MAX_LEN  128
#define OTA_MQTT_USERNAME_MAX_LEN   256
#define OTA_MQTT_PASSWORD_MAX_LEN   256
#define OTA_MQTT_TOPIC_MAX_LEN      128

/* capability_manifest 已迁移到 components/mcp_registry 运行时生成
 * （与 tools/list、tools/call、hello v3 同源），此处不再保留静态宏 ——
 * 静态清单会与注册表漂移，2026-09-19 起废止。 */

typedef struct {
    char server_time[48];
    char timezone[48];
    int32_t timezone_offset;

    char websocket_url[OTA_WS_URL_MAX_LEN];
    char websocket_token[OTA_TOKEN_MAX_LEN];
    bool has_websocket;

    char mqtt_endpoint[OTA_MQTT_ENDPOINT_MAX_LEN];
    char mqtt_client_id[OTA_MQTT_CLIENT_ID_MAX_LEN];
    char mqtt_username[OTA_MQTT_USERNAME_MAX_LEN];
    char mqtt_password[OTA_MQTT_PASSWORD_MAX_LEN];
    char mqtt_publish_topic[OTA_MQTT_TOPIC_MAX_LEN];
    char mqtt_subscribe_topic[OTA_MQTT_TOPIC_MAX_LEN];
    bool has_mqtt;

    char firmware_url[OTA_FW_URL_MAX_LEN];
    char firmware_version[32];

    char activation_code[16];
    char activation_message[OTA_ACT_MSG_MAX_LEN];
    bool has_activation;
    bool has_firmware;

    char error[128];
    bool has_error;
} ota_result_t;

esp_err_t ota_client_init(void);

esp_err_t ota_client_check(ota_result_t *result);

#endif