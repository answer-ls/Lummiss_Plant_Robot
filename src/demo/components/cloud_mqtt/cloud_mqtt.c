#include "cloud_mqtt.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "mqtt_client.h"

#include "cloud_mcp.h"
#include "mcp_registry.h"
#include "video_streamer.h"

static const char *TAG = "CLOUD_MQTT";

/* 下行 JSON 累积缓冲上限。现行文档规定单条 MQTT JSON 最大 8 KiB；
 * WebRTC SDP/ICE 只走 WHIP HTTPS，不会进入 MQTT。当前 4 KiB 已覆盖 MCP
 * 控制报文，若服务端实际下发超过上限会明确打印并丢弃。 */
#define CLOUD_MQTT_DOWNLINK_MAX   4096

/* esp-mqtt 收发缓冲保持 4 KiB；较大的 QoS0 MCP 回执由 esp-mqtt 自动分片发送。 */
#define CLOUD_MQTT_BUFFER_SIZE    4096

#define CLOUD_MQTT_KEEPALIVE_SEC  120
#define CLOUD_MQTT_RECONNECT_MS   5000

/* MQTT 任务的栈与优先级。注意：IDF 5.5 的 esp-mqtt 配置结构里**没有 core_id**，
 * 任务由调度器自行分配核，无法钉核 —— 不要声称指定了核。 */
#define CLOUD_MQTT_TASK_STACK     6144
#define CLOUD_MQTT_TASK_PRIO      5

/* AI bridge hello（文档 §3.1）：version=3、transport=udp。
 * capability_manifest 从 mcp_registry 运行时生成 —— 与 tools/list、tools/call
 * 同源，未实现的工具（如 media.webrtc.*）不会对服务端宣称。
 * 在 cloud_mqtt_start() 里构建一次（此时工具注册已全部完成）。 */
#define CLOUD_MQTT_HELLO_MAX     1024
#define CLOUD_MQTT_MANIFEST_MAX  512
static char s_hello_v3[CLOUD_MQTT_HELLO_MAX];

static esp_mqtt_client_handle_t s_client;
static cloud_mqtt_config_t s_cfg;
static char s_uri[104];  /* "mqtt[s]://<endpoint>"，必须在客户端生命周期内有效 */
static cloud_state_t s_state = CLOUD_STATE_BOOT;

static cloud_mqtt_session_t s_session;
static bool s_session_ready;

static cloud_mqtt_text_cb_t s_text_cb;
static void *s_text_cb_ctx;

static char s_downlink[CLOUD_MQTT_DOWNLINK_MAX];
static size_t s_downlink_len;

static void set_state(cloud_state_t next)
{
    if (s_state == next) {
        return;
    }
    s_state = next;
    ESP_LOGI(TAG, "STATE -> %s", cloud_mqtt_state_name(next));
}

const char *cloud_mqtt_state_name(cloud_state_t state)
{
    switch (state) {
    case CLOUD_STATE_BOOT:             return "BOOT";
    case CLOUD_STATE_NTP_TIME_READY:   return "NTP_TIME_READY";
    case CLOUD_STATE_OTA_CONFIGURED:   return "OTA_CONFIGURED";
    case CLOUD_STATE_MQTT_CONNECTED:   return "MQTT_CONNECTED";
    case CLOUD_STATE_MQTT_SUBSCRIBED:  return "MQTT_SUBSCRIBED";
    case CLOUD_STATE_AI_HELLO_SENT:    return "AI_HELLO_SENT";
    case CLOUD_STATE_AI_SESSION_READY: return "AI_SESSION_READY";
    case CLOUD_STATE_IDLE:             return "IDLE";
    default:                           return "UNKNOWN";
    }
}

cloud_state_t cloud_mqtt_get_state(void) { return s_state; }

void cloud_mqtt_notify_ntp_ready(void)
{
    /* 由 main 在 SNTP 校时完成后调用，把状态机推进到 NTP_TIME_READY。
     * 文档 §2 要求时间同步先于 OTA 与 RTC 信令。
     * 只允许从 BOOT 前进：校时是异步的，万一它回来晚了（已经 OTA_CONFIGURED），
     * 不能把状态往回拨。 */
    if (s_state == CLOUD_STATE_BOOT) {
        set_state(CLOUD_STATE_NTP_TIME_READY);
    }
}

bool cloud_mqtt_is_session_ready(void)
{
    return __atomic_load_n(&s_session_ready, __ATOMIC_SEQ_CST);
}

void cloud_mqtt_set_text_callback(cloud_mqtt_text_cb_t cb, void *ctx)
{
    s_text_cb = cb;
    s_text_cb_ctx = ctx;
}

esp_err_t cloud_mqtt_reopen_session(void)
{
    if (s_client == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    /* 一次唤醒 = 一次新会话：清就绪标志并重新发布 hello v3。
     * 服务端开一个新 AI 会话并回新的 Server Hello（新 session_id + 新 UDP key），
     * MQTT 连接本身保持不动。旧会话的 UDP key 随之作废。 */
    __atomic_store_n(&s_session_ready, false, __ATOMIC_SEQ_CST);
    memset(&s_session, 0, sizeof(s_session));
    const int len = (int)strlen(s_hello_v3);
    if (esp_mqtt_client_publish(s_client, s_cfg.publish_topic, s_hello_v3,
                                len, 0, 0) < 0) {
        ESP_LOGW(TAG, "hello v3 重新发布失败");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "hello v3 已重新发布，等待新的 Server Hello");
    return ESP_OK;
}

esp_err_t cloud_mqtt_send_goodbye(void)
{
    if (s_client == NULL || s_session.session_id[0] == '\0') {
        return ESP_ERR_INVALID_STATE;   /* 无活动会话，空操作 */
    }
    /* 对齐上游 CloseAudioChannel(true)：客户端主动结束时告知服务端，
     * 让其立即释放会话而不是等空闲超时。 */
    char goodbye[128];
    snprintf(goodbye, sizeof(goodbye),
             "{\"session_id\":\"%s\",\"type\":\"goodbye\"}",
             s_session.session_id);
    const esp_err_t err = cloud_mqtt_publish_text(goodbye);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "已向服务端发送 goodbye（结束会话 %s）",
                 s_session.session_id);
    }
    return err;
}

bool cloud_mqtt_get_session(cloud_mqtt_session_t *out)
{
    if (out == NULL || !cloud_mqtt_is_session_ready()) {
        return false;
    }
    *out = s_session;
    return true;
}

esp_err_t cloud_mqtt_publish_text(const char *text)
{
    if (s_client == NULL || text == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    const int len = (int)strlen(text);
    /* QoS 0、retain=false（文档 §2.3）。 */
    const int msg_id = esp_mqtt_client_publish(s_client, s_cfg.publish_topic,
                                               text, len, 0, 0);
    if (msg_id < 0) {
        ESP_LOGW(TAG, "发布失败（%d 字节）", len);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* 十六进制串解码。长度必须正好是 out_len*2，且全部是合法十六进制字符；
 * 不满足就返回 false —— UDP key/nonce 解错比解失败更危险，宁可判失败。 */
static bool hex_to_bytes(const char *hex, size_t hex_len,
                         uint8_t *out, size_t out_len)
{
    if (hex == NULL || hex_len != out_len * 2) {
        return false;
    }
    for (size_t i = 0; i < out_len; i++) {
        unsigned value = 0;
        for (int nibble = 0; nibble < 2; nibble++) {
            const char c = hex[i * 2 + (size_t)nibble];
            unsigned digit;
            if (c >= '0' && c <= '9') {
                digit = (unsigned)(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                digit = (unsigned)(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                digit = (unsigned)(c - 'A' + 10);
            } else {
                return false;
            }
            value = (value << 4) | digit;
        }
        out[i] = (uint8_t)value;
    }
    return true;
}

static void copy_json_string(const cJSON *obj, const char *key,
                             char *out, size_t out_size)
{
    const cJSON *item = cJSON_GetObjectItem(obj, key);
    if (cJSON_IsString(item) && item->valuestring != NULL) {
        snprintf(out, out_size, "%s", item->valuestring);
    }
}

static int json_int(const cJSON *obj, const char *key, int fallback)
{
    const cJSON *item = cJSON_GetObjectItem(obj, key);
    return cJSON_IsNumber(item) ? item->valueint : fallback;
}

/* Server Hello（文档 §4.1）。只报"有没有拿到、长度多少"，
 * **绝不输出 key / nonce 的数值**（文档 §8）。 */
static void parse_server_hello(cJSON *root)
{
    copy_json_string(root, "session_id", s_session.session_id,
                     sizeof(s_session.session_id));

    const cJSON *udp = cJSON_GetObjectItem(root, "udp");
    if (cJSON_IsObject(udp)) {
        copy_json_string(udp, "server", s_session.udp_server,
                         sizeof(s_session.udp_server));
        s_session.udp_port = (uint16_t)json_int(udp, "port", 0);
        copy_json_string(udp, "encryption", s_session.udp_encryption,
                         sizeof(s_session.udp_encryption));

        const cJSON *key = cJSON_GetObjectItem(udp, "key");
        if (cJSON_IsString(key) && key->valuestring != NULL) {
            s_session.key_hex_len = (uint32_t)strlen(key->valuestring);
            s_session.has_key = hex_to_bytes(key->valuestring,
                                             s_session.key_hex_len,
                                             s_session.key,
                                             sizeof(s_session.key));
        }
        const cJSON *nonce = cJSON_GetObjectItem(udp, "nonce");
        if (cJSON_IsString(nonce) && nonce->valuestring != NULL) {
            s_session.nonce_hex_len = (uint32_t)strlen(nonce->valuestring);
            /* ponytail: nonce 按文档 §4.2 只作协议兼容保留，
             * 当前网关用 16 字节包头本身当 AES-CTR IV。 */
            s_session.has_nonce = hex_to_bytes(nonce->valuestring,
                                               s_session.nonce_hex_len,
                                               s_session.nonce,
                                               sizeof(s_session.nonce));
        }
    }

    const cJSON *audio = cJSON_GetObjectItem(root, "audio_params");
    if (cJSON_IsObject(audio)) {
        s_session.downlink_sample_rate =
            (uint16_t)json_int(audio, "sample_rate", 0);
        s_session.downlink_channels = (uint8_t)json_int(audio, "channels", 0);
        s_session.downlink_frame_duration =
            (uint16_t)json_int(audio, "frame_duration", 0);
    }

    ESP_LOGI("VOICE_TRACE", "us=%lld session_id=%s event=SERVER_HELLO_RX",
             (long long)esp_timer_get_time(), s_session.session_id);
    ESP_LOGI(TAG, "收到 Server Hello：session_id=%s",
             s_session.session_id[0] ? s_session.session_id : "(缺失)");
    ESP_LOGI(TAG,
             "  udp：server=%s port=%u encryption=%s "
             "key=%s(%u hex) nonce=%s(%u hex)",
             s_session.udp_server[0] ? s_session.udp_server : "(缺失)",
             (unsigned)s_session.udp_port,
             s_session.udp_encryption[0] ? s_session.udp_encryption : "(缺失)",
             s_session.has_key ? "已获取" : "缺失/非法", s_session.key_hex_len,
             s_session.has_nonce ? "已获取" : "缺失/非法",
             s_session.nonce_hex_len);
    ESP_LOGI(TAG, "  audio_params（下行）：%u Hz / %u ch / %u ms",
             (unsigned)s_session.downlink_sample_rate,
             (unsigned)s_session.downlink_channels,
             (unsigned)s_session.downlink_frame_duration);

    /* 到这里才算 AI bridge 建立。UDP 音频是阶段二，MCP 是阶段三。 */
    __atomic_store_n(&s_session_ready, true, __ATOMIC_SEQ_CST);
    set_state(CLOUD_STATE_AI_SESSION_READY);
    set_state(CLOUD_STATE_IDLE);
    /* 此处只表示服务端协商结果；UDP socket 与 GCM 上下文由音频任务随后建立，
     * 不能在尚未调用 cloud_udp_start() 时提前宣称运行态 READY。 */
    ESP_LOGI(TAG, "===== 固件云端能力状态 =====");
    ESP_LOGI(TAG, "MQTT/AI Hello        READY");
    ESP_LOGI(TAG, "MCP basic handshake  READY（已实现工具 %u 个）",
             (unsigned)mcp_registry_ready_count());
    ESP_LOGI(TAG, "UDP Opus             NEGOTIATED（%s，等待音频任务启动）",
             s_session.udp_encryption[0] ? s_session.udp_encryption : "参数缺失");
    ESP_LOGI(TAG, "WebRTC WHIP          READY（%ux%u@20 单向 H264，待服务端实机联调）",
             VIDEO_STREAM_WIDTH, VIDEO_STREAM_HEIGHT);
}

/* 下行消息分类。回调里只做长度检查 / JSON 解析 / 分类 —— 不做 HTTP、
 * 不初始化摄像头、不建 PeerConnection、不控电机（文档 §6.2）。 */
static void handle_downlink(const char *json, size_t len)
{
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (root == NULL) {
        ESP_LOGW(TAG, "下行不是合法 JSON（%u 字节），已忽略", (unsigned)len);
        return;
    }
    const cJSON *type = cJSON_GetObjectItem(root, "type");
    if (!cJSON_IsString(type) || type->valuestring == NULL) {
        ESP_LOGW(TAG, "下行缺少 type 字段（%u 字节），已忽略", (unsigned)len);
        cJSON_Delete(root);
        return;
    }

    if (strcmp(type->valuestring, "hello") == 0) {
        parse_server_hello(root);
    } else if (strcmp(type->valuestring, "mcp") == 0) {
        /* 只入队、立刻返回：工具执行（含电机控制）必须留给 mcp_task，
         * 文档 §6.2 明令禁止在 MQTT 回调里做这些。传原始 json/len，
         * 信封解析由 cloud_mcp 自己做。 */
        cloud_mcp_submit(json, len);
    } else if (s_text_cb != NULL) {
        /* stt / tts / llm / listen / system …：语音业务状态机在
         * xiaozhi_audio（与旧协议的 server_text_callback 同一个函数）。
         * 不转发的话服务端的回复就没人处理 —— 实测表现为：
         * STT/TTS/LLM 消息到了、日志打"暂未处理"、然后什么也不发生。 */
        s_text_cb(json, len, s_text_cb_ctx);
    } else {
        /* 以后 rtc_signal / rtc_event 等类型会到这里，一眼能看出
         * "收到了但还没实现"，而不是误判成没收到。 */
        ESP_LOGI(TAG, "暂未处理的下行类型：%s（%u 字节）",
                 type->valuestring, (unsigned)len);
    }
    cJSON_Delete(root);
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    (void)handler_args;
    (void)base;
    const esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        set_state(CLOUD_STATE_MQTT_CONNECTED);
        /* 连接成功 ≠ 小智在线：必须先订阅再发 hello，
         * 收到 Server Hello 才算 AI_SESSION_READY（文档 §2）。 */
        if (esp_mqtt_client_subscribe(event->client, s_cfg.subscribe_topic,
                                      0) < 0) {
            ESP_LOGE(TAG, "订阅 %s 失败", s_cfg.subscribe_topic);
        }
        break;

    case MQTT_EVENT_SUBSCRIBED: {
        const bool failed = event->error_handle != NULL &&
                            event->error_handle->error_type !=
                                MQTT_ERROR_TYPE_NONE;
        if (failed) {
            ESP_LOGE(TAG, "订阅未成功（error_type=%d），不发送 hello",
                     (int)event->error_handle->error_type);
            break;
        }
        set_state(CLOUD_STATE_MQTT_SUBSCRIBED);
        const int len = (int)strlen(s_hello_v3);
        const int msg_id = esp_mqtt_client_publish(event->client,
                                                    s_cfg.publish_topic,
                                                    s_hello_v3, len, 0, 0);
        if (msg_id < 0) {
            ESP_LOGE(TAG, "AI hello 发送失败");
            break;
        }
        set_state(CLOUD_STATE_AI_HELLO_SENT);
        ESP_LOGI(TAG, "AI hello v3 已发布到 %s：%s",
                 s_cfg.publish_topic, s_hello_v3);
        break;
    }

    case MQTT_EVENT_DATA: {
        /* 分片累积：esp-mqtt 对大 payload 会分多次回调。 */
        if (event->current_data_offset == 0) {
            s_downlink_len = 0;
        }
        if (s_downlink_len + (size_t)event->data_len >= sizeof(s_downlink)) {
            ESP_LOGW(TAG, "下行消息超过 %u 字节上限，已丢弃",
                     (unsigned)CLOUD_MQTT_DOWNLINK_MAX);
            s_downlink_len = 0;
            break;
        }
        memcpy(s_downlink + s_downlink_len, event->data, (size_t)event->data_len);
        s_downlink_len += (size_t)event->data_len;
        if (event->current_data_offset + event->data_len <
            event->total_data_len) {
            break;   /* 还有后续分片 */
        }
        s_downlink[s_downlink_len] = '\0';
        /* 只报长度，不报正文：后续 SDP / 凭据类消息不能进日志（文档 §8）。
         * topic 用我们订阅的那个（本就没打印过 event->topic，避免非 NUL 结尾）。 */
        ESP_LOGI(TAG, "下行消息：topic=%s（%u 字节）",
                 s_cfg.subscribe_topic, (unsigned)s_downlink_len);
        handle_downlink(s_downlink, s_downlink_len);
        s_downlink_len = 0;
        break;
    }

    case MQTT_EVENT_DISCONNECTED:
        /* 断开即作废本次会话：重新 hello 后服务端会下发**新的** UDP key，
         * 旧 key 不能复用（文档 §4.1）。 */
        __atomic_store_n(&s_session_ready, false, __ATOMIC_SEQ_CST);
        memset(&s_session, 0, sizeof(s_session));
        ESP_LOGW(TAG, "MQTT 已断开，等待自动重连（会话参数已作废）");
        set_state(CLOUD_STATE_OTA_CONFIGURED);
        break;

    case MQTT_EVENT_ERROR:
        /* 只打错误种类与返回码，不打凭据。
         * 注意：error_handle 里的 `disconnect_return_code` 属于 MQTT 5.0
         * （被 #ifdef CONFIG_MQTT_PROTOCOL_5 包着），本工程用的是 3.1.1，
         * 该字段不存在，不要去读它。 */
        if (event->error_handle != NULL) {
            ESP_LOGE(TAG,
                     "MQTT 错误：type=%d connect_rc=%d sock_errno=%d "
                     "tls_esp=0x%x tls_stack=0x%x cert_flags=0x%x",
                     (int)event->error_handle->error_type,
                     (int)event->error_handle->connect_return_code,
                     event->error_handle->esp_transport_sock_errno,
                     (unsigned)event->error_handle->esp_tls_last_esp_err,
                     (unsigned)event->error_handle->esp_tls_stack_err,
                     (unsigned)event->error_handle->esp_tls_cert_verify_flags);
        } else {
            ESP_LOGE(TAG, "MQTT 错误（无 error_handle）");
        }
        break;

    default:
        break;
    }
}

esp_err_t cloud_mqtt_start(const cloud_mqtt_config_t *config)
{
    if (config == NULL || config->endpoint[0] == '\0' ||
        config->client_id[0] == '\0' || config->username[0] == '\0' ||
        config->password[0] == '\0' || config->publish_topic[0] == '\0' ||
        config->subscribe_topic[0] == '\0') {
        ESP_LOGE(TAG, "MQTT 配置不完整，拒绝启动");
        return ESP_ERR_INVALID_ARG;
    }
    if (s_client != NULL) {
        ESP_LOGW(TAG, "MQTT 客户端已启动，忽略重复调用");
        return ESP_OK;
    }

    s_cfg = *config;
    /* 地址一律来自 OTA，固件里不硬编码任何公网 IP（文档 §1）。
     * 生产端口 8883 必须使用 TLS；旧代码固定拼 mqtt://，会把明文 MQTT
     * 发到 TLS 端口，服务端只能立即 EOF。若服务端将来直接下发 URI，原样使用。 */
    const bool endpoint_has_scheme = strstr(s_cfg.endpoint, "://") != NULL;
    const size_t endpoint_len = strlen(s_cfg.endpoint);
    const bool mqtt_tls = endpoint_has_scheme ?
        strncmp(s_cfg.endpoint, "mqtts://", 8) == 0 :
        (endpoint_len >= 5 && strcmp(s_cfg.endpoint + endpoint_len - 5, ":8883") == 0);
    if (endpoint_has_scheme) {
        snprintf(s_uri, sizeof(s_uri), "%s", s_cfg.endpoint);
    } else {
        snprintf(s_uri, sizeof(s_uri), "%s://%s",
                 mqtt_tls ? "mqtts" : "mqtt", s_cfg.endpoint);
    }

    /* MCP 队列与任务必须在 OTA（capability_manifest 从注册表生成）和连上之前
     * 就绪：一旦 MQTT_CONNECTED，服务端可能立刻下发 initialize，此时队列还不
     * 存在就会丢消息。幂等：app_main 里已提前调用过的话这里是空操作。 */
    if (cloud_mcp_init() != ESP_OK) {
        ESP_LOGE(TAG, "MCP 初始化失败，工具调用将不可用");
    }

    /* hello v3 的 capability_manifest 从 mcp_registry 运行时生成 ——
     * 与 tools/list、tools/call、OTA 请求体同源，未实现的工具不会对服务端
     * 宣称 READY。 */
    char manifest[CLOUD_MQTT_MANIFEST_MAX];
    if (mcp_registry_build_capability_manifest(manifest, sizeof(manifest)) !=
        ESP_OK) {
        ESP_LOGE(TAG, "capability_manifest 生成失败（缓冲不足？）");
        return ESP_ERR_NO_MEM;
    }
    snprintf(s_hello_v3, sizeof(s_hello_v3),
             "{\"type\":\"hello\",\"version\":3,\"transport\":\"udp\","
             /* 正式协议声明支持 AEAD；cloud_udp 已实现 AES-128-GCM 主路径，
              * 并保留服务端迁移期返回 AES-128-CTR 时的兼容路径。 */
             "\"features\":{\"mcp\":true,\"aec\":true,\"udp_aead\":true},"
             "\"audio_params\":{\"format\":\"opus\",\"sample_rate\":16000,"
             "\"channels\":1,\"frame_duration\":60},"
             "\"capability_manifest\":%s}",
             manifest);

    const esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = s_uri,
        /* MQTTS 复用 ESP-IDF 官方证书包并校验 www.lummiss.com 域名；
         * 明文迁移端口不会使用该字段。 */
        .broker.verification.crt_bundle_attach = mqtt_tls ? esp_crt_bundle_attach : NULL,
        .credentials.client_id = s_cfg.client_id,
        .credentials.username = s_cfg.username,
        .credentials.authentication.password = s_cfg.password,
        .session.keepalive = CLOUD_MQTT_KEEPALIVE_SEC,
        .buffer.size = CLOUD_MQTT_BUFFER_SIZE,
        /* 出向只有我们自己的小 JSON（hello ~400 B、MCP 回执、listen），1 KB 足够。
         * 不设的话 esp-mqtt 会按 buffer.size 再配一份 16 KB 出向缓冲 —— 内部 RAM
         * 紧张，2026-09-18 实机把 LVGL 的显示缓冲挤到分配失败并 abort 重启。 */
        .buffer.out_size = 1024,
        .network.reconnect_timeout_ms = CLOUD_MQTT_RECONNECT_MS,
        .network.disable_auto_reconnect = false,
        .task.stack_size = CLOUD_MQTT_TASK_STACK,
        .task.priority = CLOUD_MQTT_TASK_PRIO,
    };

    s_client = esp_mqtt_client_init(&mqtt_cfg);
    if (s_client == NULL) {
        ESP_LOGE(TAG, "创建 MQTT 客户端失败");
        return ESP_FAIL;
    }
    if (esp_mqtt_client_register_event(s_client, MQTT_EVENT_ANY,
                                       mqtt_event_handler, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "注册 MQTT 事件失败");
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
        return ESP_FAIL;
    }

    /* 只打 endpoint 与凭据长度，不打 client_id / username / password 正文。 */
    ESP_LOGI(TAG,
             "MQTT 启动：endpoint=%s client_id=%u 字节 username=%u 字节 "
             "password 已设置，publish=%s subscribe=%s，keepalive=%d s，"
             "transport=%s，buffer=%d 字节，task 栈=%d 优先级=%d（esp-mqtt 不支持指定核）",
             s_cfg.endpoint, (unsigned)strlen(s_cfg.client_id),
             (unsigned)strlen(s_cfg.username), s_cfg.publish_topic,
             s_cfg.subscribe_topic, CLOUD_MQTT_KEEPALIVE_SEC,
             mqtt_tls ? "MQTTS" : "MQTT", CLOUD_MQTT_BUFFER_SIZE, CLOUD_MQTT_TASK_STACK,
             CLOUD_MQTT_TASK_PRIO);

    set_state(CLOUD_STATE_OTA_CONFIGURED);
    if (esp_mqtt_client_start(s_client) != ESP_OK) {
        ESP_LOGE(TAG, "启动 MQTT 客户端失败");
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
        return ESP_FAIL;
    }
    return ESP_OK;
}
