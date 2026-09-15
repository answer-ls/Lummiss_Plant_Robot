#include "ota_client.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "device_identity.h"

static const char *TAG = "OTA";
static const char *OTA_URL = "https://www.lummiss.com/lummiss/ota/";
#define OTA_HTTP_TIMEOUT_MS 15000
#define OTA_FW_PLACEHOLDER    "NOT_ACTIVATED_FIRMWARE"

static char ota_buffer[4096];
static size_t ota_buffer_len;

static esp_err_t ota_http_event_handler(esp_http_client_event_t *ev)
{
    switch (ev->event_id) {
    case HTTP_EVENT_ON_DATA:
        if (ota_buffer_len + ev->data_len < sizeof(ota_buffer)) {
            memcpy(ota_buffer + ota_buffer_len, ev->data, ev->data_len);
            ota_buffer_len += ev->data_len;
            ota_buffer[ota_buffer_len] = '\0';
        }
        break;
    default:
        break;
    }
    return ESP_OK;
}

static const char *ota_json_get_string(const char *json, const char *key,
                                        char *out, size_t out_size)
{
    char search[128];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *key_pos = strstr(json, search);
    if (key_pos == NULL) {
        return NULL;
    }
    const char *colon = strchr(key_pos + strlen(search), ':');
    if (colon == NULL) {
        return NULL;
    }
    while (*(++colon) == ' ' || *colon == '\t');
    if (*colon != '"') {
        if (*colon == 'n' && strlen(colon) >= 4 &&
            memcmp(colon, "null", 4) == 0) {
            return NULL;
        }
        return NULL;
    }
    const char *close = strchr(colon + 1, '"');
    if (close == NULL) {
        return NULL;
    }
    size_t len = (size_t)(close - colon - 1);
    if (len >= out_size) {
        len = out_size - 1;
    }
    memcpy(out, colon + 1, len);
    out[len] = '\0';
    return out;
}

static int64_t ota_json_get_int64(const char *json, const char *key)
{
    char search[128];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *key_pos = strstr(json, search);
    if (key_pos == NULL) {
        return 0;
    }
    const char *colon = strchr(key_pos + strlen(search), ':');
    if (colon == NULL) {
        return 0;
    }
    const char *val = colon + 1;
    while (*val == ' ' || *val == '\t') {
        val++;
    }
    return strtoll(val, NULL, 10);
}

static bool ota_json_field_exists(const char *json, const char *key)
{
    char search[128];
    snprintf(search, sizeof(search), "\"%s\"", key);
    return strstr(json, search) != NULL;
}

esp_err_t ota_client_init(void)
{
    return ESP_OK;
}

esp_err_t ota_client_check(ota_result_t *result)
{
    if (result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(result, 0, sizeof(*result));

    const device_identity_t *id = device_identity_get();
    if (id == NULL || id->device_id[0] == '\0' || id->client_id[0] == '\0') {
        snprintf(result->error, sizeof(result->error),
                 "设备身份未初始化");
        result->has_error = true;
        return ESP_ERR_INVALID_STATE;
    }

    ota_buffer_len = 0;
    memset(ota_buffer, 0, sizeof(ota_buffer));

    char body[512];
    snprintf(body, sizeof(body),
             "{\"application\":{\"name\":\"desktop-pet\",\"version\":\"1.0.0\"},"
             "\"board\":{\"type\":\"DESKTOP_PET_V1\","
             "\"mac\":\"%s\"}}",
             id->device_id);

    esp_http_client_config_t config = {
        .url = OTA_URL,
        .method = HTTP_METHOD_POST,
        .timeout_ms = OTA_HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = ota_http_event_handler,
        .buffer_size = 2048,
        .disable_auto_redirect = false,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        snprintf(result->error, sizeof(result->error),
                 "创建 HTTP 客户端失败");
        result->has_error = true;
        return ESP_FAIL;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Accept", "application/json");
    esp_http_client_set_header(client, "Device-Id", id->device_id);
    esp_http_client_set_header(client, "Client-Id", id->client_id);
    esp_http_client_set_post_field(client, body, (int)strlen(body));

    esp_err_t err = esp_http_client_perform(client);
    int status_code = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        snprintf(result->error, sizeof(result->error),
                 "OTA 请求失败：%s（HTTP %d）",
                 esp_err_to_name(err), status_code);
        result->has_error = true;
        ESP_LOGE(TAG, "%s", result->error);
        return err;
    }

    if (status_code != 200) {
        snprintf(result->error, sizeof(result->error),
                 "OTA 返回 HTTP %d", status_code);
        result->has_error = true;
        ESP_LOGE(TAG, "%s", result->error);
        return ESP_FAIL;
    }

    /* OTA 响应包含 WebSocket 动态 Token，串口只能记录长度，不能输出正文。 */
    ESP_LOGI(TAG, "OTA 响应已接收（%d 字节）", (int)ota_buffer_len);

    if (ota_json_field_exists(ota_buffer, "error")) {
        char err_msg[256] = {0};
        if (ota_json_get_string(ota_buffer, "error",
                                err_msg, sizeof(err_msg)) != NULL) {
            /* 必须给 %s 加精度：err_msg 有 256 字节而 result->error 只有 128，
             * 不设上限的话 -Werror=format-truncation 直接判编译失败。前缀
             * 「服务端错误：」占 18 字节，留 100 给消息本身。 */
            snprintf(result->error, sizeof(result->error),
                     "服务端错误：%.100s", err_msg);
        } else {
            snprintf(result->error, sizeof(result->error),
                     "服务端返回未知错误");
        }
        result->has_error = true;
        ESP_LOGE(TAG, "%s", result->error);
        return ESP_FAIL;
    }

    /* A. server_time */
    if (ota_json_field_exists(ota_buffer, "server_time")) {
        /* 粗略定位到 server_time 对象 */
        const char *st = strstr(ota_buffer, "\"server_time\"");
        if (st != NULL) {
            snprintf(result->server_time, sizeof(result->server_time),
                     "%" PRId64, ota_json_get_int64(ota_buffer, "timestamp"));
            ota_json_get_string(ota_buffer, "timeZone",
                                result->timezone, sizeof(result->timezone));
            result->timezone_offset = (int32_t)ota_json_get_int64(
                ota_buffer, "timezone_offset");
        }
    }

    /* B. websocket */
    /* url 在 websocket 和 firmware 两个对象里都有，而响应中 firmware 排在
     * websocket 前面（接口文档 3.2/3.3），所以在整份 buffer 里搜 "url" 会先命中
     * 固件下载地址。那样一来 websocket 对象整个缺失时（文档 3.4 要求"记录错误并
     * 停止进入 WS 流程"）也会拿到一个非空 URL，下面的空值检查就失效了。只允许在
     * websocket 对象内部取。 */
    const char *ws_obj = strstr(ota_buffer, "\"websocket\"");
    if (ws_obj != NULL) {
        ota_json_get_string(ws_obj, "url",
                             result->websocket_url, sizeof(result->websocket_url));
        ota_json_get_string(ws_obj, "token",
                             result->websocket_token, sizeof(result->websocket_token));
    }

    if (result->websocket_url[0] == '\0') {
        snprintf(result->error, sizeof(result->error),
                 "OTA 响应中缺少 websocket.url");
        result->has_error = true;
        ESP_LOGE(TAG, "%s", result->error);
        return ESP_FAIL;
    }
    if (strncmp(result->websocket_url, "wss://", 6) != 0) {
        snprintf(result->error, sizeof(result->error),
                 "OTA 返回了非 WSS WebSocket 地址");
        result->has_error = true;
        ESP_LOGE(TAG, "%s", result->error);
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (result->websocket_token[0] == '\0') {
        snprintf(result->error, sizeof(result->error),
                 "OTA 响应中 websocket.token 为空");
        result->has_error = true;
        ESP_LOGE(TAG, "%s", result->error);
        return ESP_ERR_INVALID_RESPONSE;
    }

    /* C. activation */
    if (ota_json_field_exists(ota_buffer, "activation")) {
        result->has_activation = true;
        ota_json_get_string(ota_buffer, "code",
                             result->activation_code, sizeof(result->activation_code));
        ota_json_get_string(ota_buffer, "message",
                             result->activation_message,
                             sizeof(result->activation_message));
        ESP_LOGW(TAG, "设备未激活。激活码：%s",
                 result->activation_code);
    }

    /* D. firmware */
    if (ota_json_field_exists(ota_buffer, "firmware")) {
        const char *fw_obj = strstr(ota_buffer, "\"firmware\"");
        if (fw_obj != NULL) {
            ota_json_get_string(fw_obj, "url",
                                 result->firmware_url, sizeof(result->firmware_url));
            ota_json_get_string(fw_obj, "version",
                                 result->firmware_version,
                                 sizeof(result->firmware_version));
        }
        if (result->firmware_url[0] != '\0' &&
            strstr(result->firmware_url, OTA_FW_PLACEHOLDER) == NULL) {
            result->has_firmware = true;
        }
    }

    ESP_LOGI(TAG, "OTA 检查完成 → WS: %s, Token: %s...",
             result->websocket_url,
             result->websocket_token[0] ? "已获取" : "空");

    return ESP_OK;
}
