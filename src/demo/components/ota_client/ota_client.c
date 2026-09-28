#include "ota_client.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_app_desc.h"
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "device_identity.h"
#include "mcp_registry.h"   /* capability_manifest 从工具注册表生成 */

static const char *TAG = "OTA";

/* OTA 服务地址。用编译开关隔离两套后端，代码不删除、不互相污染：
 *
 *   CONFIG_XIAOZHI_USE_OFFICIAL_SERVER = y → 小智官方服务（临时验证）
 *   CONFIG_XIAOZHI_USE_OFFICIAL_SERVER = n → Lummiss 自有服务（正式环境）
 *
 * 注意：这里只改 OTA 地址，WebSocket 地址仍然由 OTA 响应里的
 * websocket.url / websocket.token 决定，不在客户端硬编码。 */
#if defined(CONFIG_XIAOZHI_USE_OFFICIAL_SERVER)
static const char *OTA_URL = "https://api.tenclass.net/xiaozhi/ota/";
#else
static const char *OTA_URL = "https://www.lummiss.com/lummiss/ota/";
#endif

#define OTA_HTTP_TIMEOUT_MS 15000
#define OTA_FW_PLACEHOLDER    "NOT_ACTIVATED_FIRMWARE"

static char ota_buffer[4096];
static size_t ota_buffer_len;

/* OTA 请求体。带 capability_manifest 后约 700 字节，用文件级 static
 * 而不是局部数组 —— main_task 栈默认只有 3.5 KB。 */
static char ota_body[1024];

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

/* 一次性转储 OTA 响应正文，回答"官方服务端到底给了这台设备什么"。
 *
 * 动机：官方服务器在 WebSocket 握手 101 成功后约 280 ms 就发一个**不带状态码的
 * CLOSE** 关掉连接，下行里一个字都不发（已用帧级日志证实）。设备端再也拿不到
 * 更多信息，那么剩下的观测面就只有 OTA 响应本身 —— 官方对这台设备的态度全写在
 * 里面。原来只诊断了"哪些字段存在"，而 473 字节里大约有 270 字节从没被看过。
 *
 * 安全：敏感字段（token / mqtt username / mqtt password）的值被逐字符替换
 * 成 '*'（长度保留），其余字段原样输出 —— 结构看得见，凭据不外泄。
 * 缓冲用文件级 static，不占 main_task 的栈（CONFIG_ESP_MAIN_TASK_STACK_SIZE
 * 默认只有 3.5 KB，凭空加 4 KB 局部数组会爆）。 */
static void mask_json_string_values(char *json, const char *key)
{
    char search[32];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const size_t search_len = strlen(search);
    char *pos = strstr(json, search);
    while (pos != NULL) {
        char *colon = strchr(pos + search_len, ':');
        if (colon == NULL) {
            return;
        }
        char *val = colon + 1;
        while (*val == ' ' || *val == '\t') {
            val++;
        }
        if (*val == '"') {
            char *close = strchr(val + 1, '"');
            for (char *p = val + 1; close != NULL && p < close; p++) {
                *p = '*';
            }
        }
        pos = strstr(pos + search_len, search);
    }
}

static void ota_log_response_masked(const char *body)
{
    static char scratch[sizeof(ota_buffer)];
    const size_t len = strlen(body);
    if (len >= sizeof(scratch)) {
        ESP_LOGW(TAG, "OTA 响应正文 %u 字节，超出转储上限", (unsigned)len);
        return;
    }
    memcpy(scratch, body, len + 1);

    /* 2026-09-18 实测 Lummiss 后端开始在 OTA 响应里下发 MQTT 凭据，
     * 原实现只涂 token，把 mqtt username/password 明文打进了串口日志。
     * 用户要求：token / MQTT 用户名 / 密码都不许明文输出。 */
    mask_json_string_values(scratch, "token");
    mask_json_string_values(scratch, "username");
    mask_json_string_values(scratch, "password");

    ESP_LOGI(TAG, "OTA 响应正文（凭据已涂抹）：%s", scratch);
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

    /* 每次检查打印实际生效的服务端与地址，回切服务端时一眼可查。 */
#if defined(CONFIG_XIAOZHI_USE_OFFICIAL_SERVER)
    ESP_LOGW(TAG, "OTA URL=%s（官方测试模式）", OTA_URL);
#else
    ESP_LOGI(TAG, "OTA URL=%s", OTA_URL);
#endif

    /* 请求体按《ESP32-嵌入式三通道接入实施文档》§2.2 构造。
     * body 用文件级 static：main_task 栈默认只有 3.5 KB，放 1 KB 局部数组有风险。
     * application/board 沿用服务端已接受的形态（board.type 未改成文档里的
     * "ESP32-P4"，那会改变服务端识别结果，需先与后端确认，见交付说明）。 */
    char elf_sha256[65] = {0};
    esp_app_get_elf_sha256(elf_sha256, sizeof(elf_sha256));
    /* capability_manifest 从 mcp_registry 运行时生成 —— 与 tools/list、
     * tools/call、hello v3 同源；只包含已有同步或异步处理器的能力。 */
    char manifest[512];
    if (mcp_registry_build_capability_manifest(manifest, sizeof(manifest)) !=
        ESP_OK) {
        ESP_LOGE(TAG, "capability_manifest 生成失败，本次 OTA 检查中止");
        result->has_error = true;
        snprintf(result->error, sizeof(result->error),
                 "capability_manifest 生成失败");
        return ESP_ERR_NO_MEM;
    }
    snprintf(ota_body, sizeof(ota_body),
             "{\"application\":{\"name\":\"desktop-pet\",\"version\":\"1.0.0\","
             "\"elf_sha256\":\"%s\"},"
             "\"board\":{\"type\":\"DESKTOP_PET_V1\",\"mac\":\"%s\"},"
             "\"capability_manifest\":%s}",
             elf_sha256, id->device_id, manifest);

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
    esp_http_client_set_post_field(client, ota_body, (int)strlen(ota_body));

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

    /* OTA 响应含 WebSocket 动态 Token，所以正文只以**涂抹形式**转储：
     * token 的值逐字符换成 '*'（长度保留），其余字段原样输出。 */
    ESP_LOGI(TAG, "OTA 响应已接收（%d 字节）", (int)ota_buffer_len);
    ota_log_response_masked(ota_buffer);

    /* 只诊断响应结构是否包含关键对象，不输出 URL、Token 或其他敏感内容。 */
    ESP_LOGI(TAG,
             "OTA 响应结构：server_time=%s firmware=%s mqtt=%s websocket=%s "
             "activation=%s error=%s",
             ota_json_field_exists(ota_buffer, "server_time") ? "存在" : "缺少",
             ota_json_field_exists(ota_buffer, "firmware") ? "存在" : "缺少",
             ota_json_field_exists(ota_buffer, "mqtt") ? "存在" : "缺少",
             ota_json_field_exists(ota_buffer, "websocket") ? "存在" : "缺少",
             ota_json_field_exists(ota_buffer, "activation") ? "存在" : "缺少",
             ota_json_field_exists(ota_buffer, "error") ? "存在" : "缺少");

    /* 服务端业务错误也可能以 HTTP 200 + code/msg 返回。先识别该错误，
     * 避免把 Redis 等后端故障误报为设备端缺少 MQTT 配置。 */
    if (ota_json_field_exists(ota_buffer, "code")) {
        const int64_t service_code = ota_json_get_int64(ota_buffer, "code");
        if (service_code != 0) {
            char service_msg[96] = {0};
            (void)ota_json_get_string(ota_buffer, "msg", service_msg,
                                      sizeof(service_msg));
            snprintf(result->error, sizeof(result->error),
                     "OTA 服务端业务错误 code=%" PRId64 " msg=%.64s",
                     service_code, service_msg[0] ? service_msg : "未提供");
            result->has_error = true;
            ESP_LOGE(TAG, "%s", result->error);
            return ESP_FAIL;
        }
    }

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

    /* B. activation —— 必须排在 websocket 校验之前解析。
     *
     * 官方服务器（CONFIG_XIAOZHI_USE_OFFICIAL_SERVER）对**未激活**设备只回
     * server_time / firmware / activation，不带 websocket 段。原来的顺序是先做
     * websocket 校验、缺 url 就 return，于是激活码这条最关键的信息永远打不出来，
     * 排查时只能看到一句「OTA 响应中缺少 websocket.url」。这里把它提前。 */
    /* 限定读取 activation 对象，避免误取顶层状态 code 或其它对象中的字段。 */
    cJSON *activation_root = cJSON_Parse(ota_buffer);
    const cJSON *activation = cJSON_GetObjectItemCaseSensitive(activation_root, "activation");
    if (cJSON_IsObject(activation)) {
        result->has_activation = true;
        const cJSON *code = cJSON_GetObjectItemCaseSensitive(activation, "code");
        const cJSON *message = cJSON_GetObjectItemCaseSensitive(activation, "message");
        if (cJSON_IsString(code)) {
            /* 超长值不截断，交由显示层报告无有效绑定码。 */
            if (strlen(code->valuestring) < sizeof(result->activation_code))
                strcpy(result->activation_code, code->valuestring);
        } else if (cJSON_IsNumber(code) && code->valuedouble >= 100000 &&
                   code->valuedouble <= 999999 && code->valuedouble == code->valueint) {
            snprintf(result->activation_code, sizeof(result->activation_code), "%d", code->valueint);
        }
        if (cJSON_IsString(message))
            snprintf(result->activation_message, sizeof(result->activation_message), "%s", message->valuestring);
        ESP_LOGW(TAG, "设备未激活。激活码：%s，提示：%.64s",
                 result->activation_code, result->activation_message);
    }
    cJSON_Delete(activation_root);

    /* C1. websocket 段（旧协议通道）
     *
     * url 在 websocket 和 firmware 两个对象里都有，而响应中 firmware 排在
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
        result->has_websocket = result->websocket_url[0] != '\0';
    }

    /* C2. mqtt 六字段（三通道新协议的通道一，文档 §2.3）
     *
     * 六个字段原样保存：不拼接 client_id、不解析 username 内部结构、
     * 不重新计算 password。任何一项缺失都算凭据不完整 —— 半套凭据连不上，
     * 与其带着残缺值去连、不如在这里就说清楚缺哪个（只说字段名，不说值）。 */
    const char *mqtt_obj = strstr(ota_buffer, "\"mqtt\"");
    if (mqtt_obj != NULL) {
        ota_json_get_string(mqtt_obj, "endpoint",
                             result->mqtt_endpoint, sizeof(result->mqtt_endpoint));
        ota_json_get_string(mqtt_obj, "client_id",
                             result->mqtt_client_id, sizeof(result->mqtt_client_id));
        ota_json_get_string(mqtt_obj, "username",
                             result->mqtt_username, sizeof(result->mqtt_username));
        ota_json_get_string(mqtt_obj, "password",
                             result->mqtt_password, sizeof(result->mqtt_password));
        ota_json_get_string(mqtt_obj, "publish_topic",
                             result->mqtt_publish_topic,
                             sizeof(result->mqtt_publish_topic));
        ota_json_get_string(mqtt_obj, "subscribe_topic",
                             result->mqtt_subscribe_topic,
                             sizeof(result->mqtt_subscribe_topic));

        result->has_mqtt =
            result->mqtt_endpoint[0] != '\0' &&
            result->mqtt_client_id[0] != '\0' &&
            result->mqtt_username[0] != '\0' &&
            result->mqtt_password[0] != '\0' &&
            result->mqtt_publish_topic[0] != '\0' &&
            result->mqtt_subscribe_topic[0] != '\0';

        if (!result->has_mqtt) {
            ESP_LOGE(TAG,
                     "mqtt 段字段不全：endpoint=%s client_id=%s username=%s "
                     "password=%s publish_topic=%s subscribe_topic=%s"
                     "（存在=1 缺失=0）",
                     result->mqtt_endpoint[0] ? "1" : "0",
                     result->mqtt_client_id[0] ? "1" : "0",
                     result->mqtt_username[0] ? "1" : "0",
                     result->mqtt_password[0] ? "1" : "0",
                     result->mqtt_publish_topic[0] ? "1" : "0",
                     result->mqtt_subscribe_topic[0] ? "1" : "0");
        }
    }

#if defined(CONFIG_CLOUD_PROTOCOL_V3)
    /* 新协议只认 mqtt。缺 mqtt 段 = 服务端 MQTT 地址或签名密钥没配好，
     * 按文档 §2.3 必须**启动失败**，不许静默退回 WebSocket。 */
    if (!result->has_mqtt) {
        snprintf(result->error, sizeof(result->error),
                 "新协议要求 OTA 下发 mqtt 六字段，实际缺失");
        result->has_error = true;
        ESP_LOGE(TAG, "%s", result->error);
        ESP_LOGE(TAG,
                 "不退回旧 WebSocket。请确认服务端已配置 MQTT 地址与签名密钥");
        return ESP_FAIL;
    }
#else
    if (result->websocket_url[0] == '\0') {
#if defined(CONFIG_XIAOZHI_USE_OFFICIAL_SERVER)
        snprintf(result->error, sizeof(result->error),
                 "官方服务器未下发 websocket.url（设备未激活）");
        result->has_error = true;
        ESP_LOGE(TAG, "%s", result->error);
        /* 官方侧只有在设备已绑定到账号后才会下发 websocket 段，
         * 光换 OTA 地址拿不到 Token，必须先完成一次激活。 */
        ESP_LOGE(TAG,
                 "请先在官方控制台用上面的激活码添加该设备，再重启复检；"
                 "若连 activation 段都没有，说明官方侧不认这台设备");
#else
        snprintf(result->error, sizeof(result->error),
                 "OTA 响应中缺少 websocket.url");
        result->has_error = true;
        ESP_LOGE(TAG, "%s", result->error);
#endif
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
#endif /* CONFIG_CLOUD_PROTOCOL_V3 */

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

    /* 收尾日志只报"拿到了什么"，不报凭据本身（文档 §8：不得记录
     * MQTT password / deviceTicket / TURN credential / UDP key·nonce）。
     * username 也只报字节数 —— 它是 Base64 不透明凭据。
     * 固件段一并报出，方便一眼排除"服务端要求先升级固件"。 */
#if defined(CONFIG_CLOUD_PROTOCOL_V3)
    ESP_LOGI(TAG,
             "OTA 检查完成 → MQTT: %s, 凭据: %s"
             "（client_id %u 字节 / username %u 字节 / password 已获取），"
             "固件：%s",
             result->mqtt_endpoint,
             result->has_mqtt ? "六字段齐全" : "不完整",
             (unsigned)strlen(result->mqtt_client_id),
             (unsigned)strlen(result->mqtt_username),
             result->has_firmware ? result->firmware_version : "无升级任务");
#else
    ESP_LOGI(TAG, "OTA 检查完成 → WS: %s, Token: %s, 固件：%s",
             result->websocket_url,
             result->websocket_token[0] ? "已获取" : "空",
             result->has_firmware ? result->firmware_version : "无升级任务");
#endif

    return ESP_OK;
}
