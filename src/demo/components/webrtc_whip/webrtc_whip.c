#include "mem_contig.h"
#include "webrtc_whip.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_peer.h"
#include "esp_peer_default.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "network_manager.h"
#include "rtc_heap_diag.h"

#ifndef CONFIG_RTC_MEM_DIAG_640P
#define CONFIG_RTC_MEM_DIAG_640P 0
#endif

/* Hosted 组件由板级配置选配，弱符号避免无 Hosted 的测试工程产生链接依赖。 */
extern void esp_hosted_mempool_report(const char *stage) __attribute__((weak));
extern void esp_hosted_sdio_rx_dma_report(void) __attribute__((weak));

static void report_hosted_pool(const char *stage)
{
    if (esp_hosted_mempool_report != NULL) esp_hosted_mempool_report(stage);
    if (esp_hosted_sdio_rx_dma_report != NULL) esp_hosted_sdio_rx_dma_report();
}

/* UI 在测试档位中可能不存在，RTC 控制层只在链接到显示驱动时调节刷新。 */
extern void display_driver_set_rtc_preview(bool active) __attribute__((weak));
extern void expression_manager_set_rtc_active(bool active) __attribute__((weak));

static const char *TAG = "WEBRTC";

#define WHIP_SDP_CAPACITY       16384U
#define WHIP_LOCATION_CAPACITY  1024U
#define WHIP_HTTP_TIMEOUT_MS    15000
#define WHIP_GATHER_TIMEOUT_MS  5000
#define WHIP_GATHER_STABLE_MS   500
#define WHIP_CONNECT_TIMEOUT_MS 20000
#define WHIP_REPORT_PERIOD_US   5000000LL
#define WHIP_CREDENTIAL_TIMEOUT_MS 5000
#define WHIP_CREDENTIAL_BODY_CAPACITY 4096U
#define WHIP_API_BASE "https://www.lummiss.com/lummiss"
/* 当前凭据接口还没有下发 iceServers。公网联调阶段按照服务端文档显式
 * 使用该 STUN；收到服务端下发的 ICE server 列表后再改成动态配置。 */
#define WHIP_STUN_URL "stun:60.210.30.199:3478"

typedef enum { COMMAND_START, COMMAND_STOP } command_type_t;
typedef struct {
    command_type_t type;
    webrtc_whip_credential_t credential;
    char session_id[WEBRTC_WHIP_SESSION_MAX];
} control_command_t;

typedef struct {
    char *answer;
    size_t answer_len;
    size_t answer_capacity;
    char location[WHIP_LOCATION_CAPACITY];
    bool overflow;
} whip_response_t;

typedef struct {
    char *body;
    size_t length;
    size_t capacity;
    bool overflow;
} http_body_response_t;

typedef struct {
    unsigned total;
    unsigned host;
    unsigned srflx;
    unsigned relay;
    bool end_of_candidates;
} ice_candidate_summary_t;

static QueueHandle_t s_commands;
static StaticQueue_t s_commands_control;
static uint8_t *s_commands_storage;
static SemaphoreHandle_t s_guard;
static SemaphoreHandle_t s_offer_guard;
static TaskHandle_t s_task;
static esp_peer_handle_t s_peer;
static volatile webrtc_whip_state_t s_state = WEBRTC_WHIP_IDLE;
static volatile bool s_peer_connected;
static volatile bool s_peer_failed;
static char s_session_id[WEBRTC_WHIP_SESSION_MAX];
static char s_location[WHIP_LOCATION_CAPACITY];
/* WHIP POST/DELETE 使用同一 StreamKey 做 Bearer 鉴权；停止会话时立即清零。 */
static char s_stream_key[WEBRTC_WHIP_KEY_MAX];
static char *s_offer;
static size_t s_offer_len;
static bool s_offer_frozen;
static uint32_t s_offer_revision;
static int64_t s_offer_updated_us;
static volatile esp_peer_state_t s_peer_phase = ESP_PEER_STATE_CLOSED;
static volatile bool s_log_paired_addr;
static uint32_t s_previous_ufrag_hash;
static uint32_t s_previous_pwd_hash;
static size_t s_previous_ufrag_len;
static size_t s_previous_pwd_len;
static bool s_have_previous_ice_credentials;
static uint32_t s_h264_frames;
static uint32_t s_h264_bytes;
static uint32_t s_idr_count;
static uint32_t s_send_fail;
static uint32_t s_pli_received;
static bool s_camera_ready;
static bool s_storage_init_done;
static esp_err_t (*s_video_start)(void);
static esp_err_t (*s_video_stop)(void);
static void (*s_force_idr)(void);
static esp_err_t (*s_camera_start)(void);
static esp_err_t (*s_camera_stop)(void);
static bool (*s_camera_is_ready)(void);
static esp_err_t (*s_resource_prepare)(uint32_t timeout_ms);
static esp_err_t (*s_audio_control)(bool suspend);
static bool s_audio_suspended;

void webrtc_whip_set_audio_control(esp_err_t (*suspend)(bool))
{
    s_audio_control = suspend;
}

static const char *state_name(webrtc_whip_state_t state)
{
    switch (state) {
    case WEBRTC_WHIP_IDLE: return "IDLE";
    case WEBRTC_WHIP_STARTING: return "STARTING";
    case WEBRTC_WHIP_CONNECTING: return "CONNECTING";
    case WEBRTC_WHIP_DTLS_CONNECTED: return "DTLS_CONNECTED";
    case WEBRTC_WHIP_MEDIA_PREPARING: return "MEDIA_PREPARING";
    case WEBRTC_WHIP_H264_OPENING: return "H264_OPENING";
    case WEBRTC_WHIP_STREAMING: return "STREAMING";
    case WEBRTC_WHIP_STOPPING: return "STOPPING";
    default: return "UNKNOWN";
    }
}

static void set_state(webrtc_whip_state_t state)
{
    const webrtc_whip_state_t previous = s_state;
    s_state = state;
    if (previous != state) {
        ESP_LOGI(TAG, "state %s -> %s", state_name(previous), state_name(state));
        /* RTC 建链从 STARTING 就会占用 DMA 与 CPU；先降屏幕刷新频率，
         * 会话结束再恢复。测试档位可能没有 UI，因此使用可选弱符号。 */
        if (display_driver_set_rtc_preview != NULL) {
            if (previous == WEBRTC_WHIP_IDLE && state == WEBRTC_WHIP_STARTING) {
                display_driver_set_rtc_preview(true);
            } else if (state == WEBRTC_WHIP_IDLE) {
                display_driver_set_rtc_preview(false);
            }
        }
        if (state == WEBRTC_WHIP_IDLE && expression_manager_set_rtc_active != NULL) {
            expression_manager_set_rtc_active(false);
        }
    }
}

static size_t s_session_dma_min = SIZE_MAX;
static size_t s_session_largest_min = SIZE_MAX;
static size_t s_stream_dma_min = SIZE_MAX;
static size_t s_stream_largest_min = SIZE_MAX;
static int64_t s_stream_started_us;
static int64_t s_last_mem_sample_us;

/* 一秒采样最低值；不等于分配器逐笔最低值，日志明确标记 sampled。 */
static void sample_memory(void)
{
    const size_t free = heap_caps_get_free_size(MALLOC_CAP_DMA);
    const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_DMA);
    if (free < s_session_dma_min) s_session_dma_min = free;
    if (largest < s_session_largest_min) s_session_largest_min = largest;
    if (s_state == WEBRTC_WHIP_STREAMING) {
        if (free < s_stream_dma_min) s_stream_dma_min = free;
        if (largest < s_stream_largest_min) s_stream_largest_min = largest;
    }
}

static void log_memory(const char *stage)
{
    sample_memory();
    const size_t dma_largest = heap_caps_get_largest_free_block(MALLOC_CAP_DMA);
    ESP_LOGI(TAG, "MEM[%s] DMA=%u/%u INT=%u/%u PSRAM=%u/%u bytes", stage,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
             (unsigned)dma_largest,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
    /* 约 1.7 KiB 的 Hosted 单块和 LCD SPI 临时块需要连续 DMA 堆。
     * 8 KiB 是调试预警余量，2 KiB 是运行期保护线；初始化阶段的瞬态
     * 会单独记录，但仅在稳定推流阶段触发主动停止。 */
    if (dma_largest < 8192 &&
        (strcmp(stage, "STREAMING") == 0 ||
         strcmp(stage, "STREAMING_5S") == 0)) {
        ESP_LOGW(TAG, "DMA_BUDGET_WARNING stage=%s largest=%u reserve=8192",
                 stage, (unsigned)dma_largest);
    }
}

/* 服务端错误正文通常只有一小段 JSON/文本。这里仅输出可打印字符，
 * 并在正文疑似包含 URL、票据或令牌时整段隐藏，避免调试日志泄露凭据。 */
static bool text_contains_ignore_case(const char *text, const char *word)
{
    if (text == NULL || word == NULL || word[0] == '\0') return false;
    const size_t word_len = strlen(word);
    for (const char *cursor = text; *cursor != '\0'; ++cursor) {
        if (strncasecmp(cursor, word, word_len) == 0) return true;
    }
    return false;
}

static void log_whip_error_body(int http_status, const char *body,
                                size_t body_len)
{
    if (body == NULL || body_len == 0) {
        ESP_LOGE(TAG, "WHIP error HTTP=%d：响应正文为空", http_status);
        return;
    }

    /* JSON 错误只提取描述字段，不打印整个对象，避免服务端顺带回显凭据。 */
    cJSON *root = cJSON_ParseWithLength(body, body_len);
    const cJSON *code = root ? cJSON_GetObjectItemCaseSensitive(root, "code") : NULL;
    const cJSON *reason = NULL;
    static const char *const reason_fields[] = {
        "message", "msg", "error", "detail",
    };
    for (size_t i = 0; root != NULL &&
         i < sizeof(reason_fields) / sizeof(reason_fields[0]); ++i) {
        const cJSON *candidate =
            cJSON_GetObjectItemCaseSensitive(root, reason_fields[i]);
        if (cJSON_IsString(candidate) && candidate->valuestring != NULL) {
            reason = candidate;
            break;
        }
    }

    const char *source = reason != NULL ? reason->valuestring : body;
    const size_t source_len = reason != NULL ? strlen(source) : body_len;
    char safe[193];
    const size_t copy_len = source_len < sizeof(safe) - 1U
                                ? source_len
                                : sizeof(safe) - 1U;
    for (size_t i = 0; i < copy_len; ++i) {
        const unsigned char ch = (unsigned char)source[i];
        safe[i] = (ch >= 0x20U && ch <= 0x7eU) ? (char)ch : ' ';
    }
    safe[copy_len] = '\0';

    static const char *const sensitive_words[] = {
        "authorization:", "bearer ", "deviceticket=", "streamkey=",
        "publishurl=", "viewertoken=", "https://", "http://",
    };
    for (size_t i = 0;
         i < sizeof(sensitive_words) / sizeof(sensitive_words[0]); ++i) {
        if (text_contains_ignore_case(safe, sensitive_words[i])) {
            ESP_LOGE(TAG,
                     "WHIP error HTTP=%d：响应正文已隐藏（疑似包含敏感字段，bytes=%u）",
                     http_status, (unsigned)body_len);
            cJSON_Delete(root);
            return;
        }
    }

    ESP_LOGE(TAG, "WHIP error HTTP=%d code=%d：%s%s", http_status,
             cJSON_IsNumber(code) ? code->valueint : -1, safe,
             source_len > copy_len ? "..." : "");
    cJSON_Delete(root);
}

/* 当前 esp_peer 默认实现会在发送端 Offer 中固定声明 4d001f
 * （Main Profile），但 ESP32-P4 硬件编码器实际输出 Baseline。LiveKit 当前
 * 关闭转码并要求 Constrained Baseline 42e01f，因此在 POST 前只原位修改
 * profile-level-id 的 6 个字符，不改变 SDP 长度、ICE 凭据或候选内容。 */
static unsigned normalize_h264_offer_profile(char *sdp)
{
    static const char field[] = "profile-level-id=";
    static const char required_profile[] = "42e01f";
    unsigned changed = 0;
    if (sdp == NULL) return 0;

    char *cursor = sdp;
    while ((cursor = strstr(cursor, field)) != NULL) {
        char *value = cursor + sizeof(field) - 1U;
        if (strlen(value) < sizeof(required_profile) - 1U) break;
        if (memcmp(value, required_profile, sizeof(required_profile) - 1U) != 0) {
            memcpy(value, required_profile, sizeof(required_profile) - 1U);
            changed++;
        }
        cursor = value + sizeof(required_profile) - 1U;
    }
    return changed;
}

static const char *peer_state_name(esp_peer_state_t state)
{
    switch (state) {
    case ESP_PEER_STATE_CLOSED: return "CLOSED";
    case ESP_PEER_STATE_DISCONNECTED: return "DISCONNECTED";
    case ESP_PEER_STATE_NEW_CONNECTION: return "NEW_CONNECTION";
    case ESP_PEER_STATE_CANDIDATE_GATHERING: return "CANDIDATE_GATHERING";
    case ESP_PEER_STATE_PAIRING: return "PAIRING";
    case ESP_PEER_STATE_PAIRED: return "PAIRED";
    case ESP_PEER_STATE_CONNECTING: return "DTLS_CONNECTING";
    case ESP_PEER_STATE_CONNECTED: return "DTLS_CONNECTED";
    case ESP_PEER_STATE_CONNECT_FAILED: return "CONNECT_FAILED";
    case ESP_PEER_STATE_VIDEO_PLI_RECEIVED: return "VIDEO_PLI_RECEIVED";
    default: return "OTHER";
    }
}

/* 解析 SDP candidate 时只读取标准字段，不记录 ICE 密码、指纹等敏感内容。 */
static ice_candidate_summary_t summarize_ice_candidates(const char *sdp,
                                                        bool print_candidates)
{
    ice_candidate_summary_t summary = {0};
    if (sdp == NULL) return summary;

    summary.end_of_candidates = strstr(sdp, "a=end-of-candidates") != NULL;
    const char *line = sdp;
    while ((line = strstr(line, "a=candidate:")) != NULL) {
        const char *line_end = strstr(line, "\r\n");
        if (line_end == NULL) line_end = strchr(line, '\n');
        if (line_end == NULL) line_end = line + strlen(line);

        char candidate[256];
        size_t line_len = (size_t)(line_end - line);
        if (line_len >= sizeof(candidate)) line_len = sizeof(candidate) - 1U;
        memcpy(candidate, line, line_len);
        candidate[line_len] = '\0';

        char foundation[32] = {0};
        char transport[12] = {0};
        char address[64] = {0};
        char type[16] = {0};
        unsigned component = 0;
        unsigned priority = 0;
        unsigned port = 0;
        const int fields = sscanf(candidate,
                                  "a=candidate:%31s %u %11s %u %63s %u typ %15s",
                                  foundation, &component, transport, &priority,
                                  address, &port, type);
        if (fields == 7) {
            summary.total++;
            if (strcmp(type, "host") == 0) summary.host++;
            else if (strcmp(type, "srflx") == 0) summary.srflx++;
            else if (strcmp(type, "relay") == 0) summary.relay++;
            if (print_candidates) {
                ESP_LOGI(TAG, "ICE candidate[%u] type=%s addr=%s:%u transport=%s",
                         summary.total, type, address, port, transport);
            }
        }
        line = *line_end != '\0' ? line_end + 1 : line_end;
    }
    return summary;
}

static bool offer_has_public_candidate(const ice_candidate_summary_t *summary)
{
    return summary != NULL && (summary->srflx > 0U || summary->relay > 0U);
}

/* esp_peer 当前公开 API 没有独立的 gathering-complete 回调。公网候选出现且 SDP
 * 连续一段时间没有更新后，视为本轮单 STUN 收集完成，并明确结束 non-trickle。 */
static bool append_end_of_candidates(char *sdp, size_t *length)
{
    static const char marker[] = "a=end-of-candidates\r\n";
    if (sdp == NULL || length == NULL) return false;
    if (strstr(sdp, "a=end-of-candidates") != NULL) return true;

    const bool has_crlf = *length >= 2U &&
                          sdp[*length - 2U] == '\r' && sdp[*length - 1U] == '\n';
    const size_t extra = sizeof(marker) - 1U + (has_crlf ? 0U : 2U);
    if (*length + extra >= WHIP_SDP_CAPACITY) return false;
    if (!has_crlf) {
        memcpy(sdp + *length, "\r\n", 2U);
        *length += 2U;
    }
    memcpy(sdp + *length, marker, sizeof(marker));
    *length += sizeof(marker) - 1U;
    return true;
}

static bool find_sdp_attribute(const char *sdp, const char *attribute,
                               const char **value, size_t *value_len)
{
    if (sdp == NULL || attribute == NULL || value == NULL || value_len == NULL) {
        return false;
    }
    const char *start = strstr(sdp, attribute);
    if (start == NULL) return false;
    start += strlen(attribute);
    const char *end = strpbrk(start, "\r\n");
    if (end == NULL) end = start + strlen(start);
    if (end == start) return false;
    *value = start;
    *value_len = (size_t)(end - start);
    return true;
}

static uint32_t hash_private_value(const char *value, size_t length)
{
    uint32_t hash = 2166136261U;
    for (size_t i = 0; i < length; ++i) {
        hash ^= (uint8_t)value[i];
        hash *= 16777619U;
    }
    return hash;
}

/* 只在指定媒体段内查找 a=mid，不依赖 peer_default 的内部 SDP 解析器。
 * 该结果用于和 PEER_DEF 的 video_mid/audio_mid 日志做 A/B 对照。 */
static bool find_media_mid(const char *sdp, const char *media,
                           char *mid, size_t mid_capacity)
{
    if (sdp == NULL || media == NULL || mid == NULL || mid_capacity < 2U) {
        return false;
    }

    char marker[24];
    const int marker_len = snprintf(marker, sizeof(marker), "m=%s ", media);
    if (marker_len <= 0 || marker_len >= (int)sizeof(marker)) return false;

    const char *section = strstr(sdp, marker);
    if (section == NULL) return false;
    const char *section_end = strstr(section + (size_t)marker_len, "\nm=");
    if (section_end == NULL) section_end = sdp + strlen(sdp);

    const char *value = strstr(section, "a=mid:");
    if (value == NULL || value >= section_end) return false;
    value += strlen("a=mid:");
    const char *end = strpbrk(value, "\r\n");
    if (end == NULL || end > section_end) end = section_end;
    if (end <= value) return false;

    size_t length = (size_t)(end - value);
    if (length >= mid_capacity) length = mid_capacity - 1U;
    memcpy(mid, value, length);
    mid[length] = '\0';
    return true;
}

/* 不输出远端 ICE 密码原文，只打印长度与哈希。这样既能确认 peer_default
 * 收到的是哪组凭据，也不会把凭据或完整 SDP 暴露到串口日志。 */
static void log_remote_sdp_summary(const char *sdp)
{
    char audio_mid[16] = {0};
    char video_mid[16] = {0};
    const bool audio_mid_ok = find_media_mid(sdp, "audio", audio_mid,
                                              sizeof(audio_mid));
    const bool video_mid_ok = find_media_mid(sdp, "video", video_mid,
                                              sizeof(video_mid));

    const char *ufrag = NULL;
    const char *pwd = NULL;
    size_t ufrag_len = 0;
    size_t pwd_len = 0;
    const bool ufrag_ok = find_sdp_attribute(sdp, "a=ice-ufrag:",
                                              &ufrag, &ufrag_len);
    const bool pwd_ok = find_sdp_attribute(sdp, "a=ice-pwd:", &pwd, &pwd_len);
    const ice_candidate_summary_t remote_candidates =
        summarize_ice_candidates(sdp, true);

    ESP_LOGI(TAG,
             "REMOTE_SDP app_parse video_mid=%s audio_mid=%s "
             "candidate_count=%u host=%u srflx=%u relay=%u",
             video_mid_ok ? video_mid : "missing",
             audio_mid_ok ? audio_mid : "none",
             remote_candidates.total, remote_candidates.host,
             remote_candidates.srflx, remote_candidates.relay);
    /* 只打印媒体协商结果，不输出包含 ICE 凭据的完整 SDP。 */
    ESP_LOGI(TAG,
             "REMOTE_VIDEO h264=%u profile42e01f=%u recvonly=%u inactive=%u rejected=%u twcc=%u",
             strstr(sdp, "H264/90000") != NULL,
             strstr(sdp, "profile-level-id=42e01f") != NULL,
             strstr(sdp, "a=recvonly") != NULL,
             strstr(sdp, "a=inactive") != NULL,
             strstr(sdp, "m=video 0 ") != NULL,
             strstr(sdp, "transport-cc") != NULL);
    ESP_LOGI(TAG,
             "REMOTE_ICE ufrag_len=%u hash=%08" PRIx32
             " pwd_len=%u hash=%08" PRIx32 " remote_ice_lite=%s",
             (unsigned)ufrag_len,
             ufrag_ok ? hash_private_value(ufrag, ufrag_len) : 0U,
             (unsigned)pwd_len,
             pwd_ok ? hash_private_value(pwd, pwd_len) : 0U,
             strstr(sdp, "a=ice-lite") != NULL ? "yes" : "no");
    ESP_LOGI(TAG,
             "ICE role=CONTROLLING；binding request/response 计数由 "
             "peer_default 内部 AGENT 日志提供，公开 API 未暴露独立计数器");
}

/* 不输出完整凭据。发现跨会话复用时直接拒绝建连，避免把不符合 ICE
 * 生命周期要求的 Offer 继续发给生产服务器。 */
static bool validate_fresh_ice_credentials(const char *sdp)
{
    const char *ufrag = NULL;
    const char *pwd = NULL;
    size_t ufrag_len = 0;
    size_t pwd_len = 0;
    if (!find_sdp_attribute(sdp, "a=ice-ufrag:", &ufrag, &ufrag_len) ||
        !find_sdp_attribute(sdp, "a=ice-pwd:", &pwd, &pwd_len)) {
        ESP_LOGE(TAG, "ICE_CREDENTIAL_MISSING：Offer 缺少 ufrag/pwd");
        return false;
    }

    const uint32_t ufrag_hash = hash_private_value(ufrag, ufrag_len);
    const uint32_t pwd_hash = hash_private_value(pwd, pwd_len);
    ESP_LOGI(TAG, "ICE credentials：ufrag_len=%u hash=%08" PRIx32
                  " pwd_len=%u hash=%08" PRIx32,
             (unsigned)ufrag_len, ufrag_hash, (unsigned)pwd_len, pwd_hash);

    if (s_have_previous_ice_credentials &&
        ufrag_len == s_previous_ufrag_len && pwd_len == s_previous_pwd_len &&
        ufrag_hash == s_previous_ufrag_hash && pwd_hash == s_previous_pwd_hash) {
        ESP_LOGE(TAG, "ICE_CREDENTIAL_REUSED：新会话复用了上一会话的 ufrag/pwd");
        return false;
    }

    s_previous_ufrag_hash = ufrag_hash;
    s_previous_pwd_hash = pwd_hash;
    s_previous_ufrag_len = ufrag_len;
    s_previous_pwd_len = pwd_len;
    s_have_previous_ice_credentials = true;
    return true;
}

webrtc_whip_state_t webrtc_whip_get_state(void)
{
    return s_state;
}

void webrtc_whip_set_video_control(esp_err_t (*start)(void),
                                   esp_err_t (*stop)(void),
                                   void (*force_idr)(void))
{
    s_video_start = start;
    s_video_stop = stop;
    s_force_idr = force_idr;
}

void webrtc_whip_set_camera_control(esp_err_t (*start)(void),
                                    esp_err_t (*stop)(void),
                                    bool (*is_ready)(void))
{
    s_camera_start = start;
    s_camera_stop = stop;
    s_camera_is_ready = is_ready;
}

void webrtc_whip_set_resource_prepare(
    esp_err_t (*prepare)(uint32_t timeout_ms))
{
    s_resource_prepare = prepare;
}

/* 回调只复制最新的完整 SDP。esp_peer 收集到新 candidate 时可再次回调；
 * 控制任务等到公网候选出现并稳定后才 POST，绝不发送 trickle PATCH。 */
static int peer_message(esp_peer_msg_t *message, void *ctx)
{
    (void)ctx;
    if (message == NULL || message->data == NULL ||
        message->type != ESP_PEER_MSG_TYPE_SDP || message->size <= 0 ||
        message->size >= (int)WHIP_SDP_CAPACITY || s_offer == NULL) {
        return 0;
    }
    if (xSemaphoreTake(s_offer_guard, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (!s_offer_frozen) {
            memcpy(s_offer, message->data, (size_t)message->size);
            s_offer[(size_t)message->size] = '\0';
            s_offer_len = (size_t)message->size;
            s_offer_revision++;
            s_offer_updated_us = esp_timer_get_time();
        }
        xSemaphoreGive(s_offer_guard);
    }
    return 0;
}

static int peer_state(esp_peer_state_t state, void *ctx)
{
    (void)ctx;
    if (state == ESP_PEER_STATE_VIDEO_PLI_RECEIVED) {
        /* PLI 是一次性请求，不是连接状态；保留 DTLS_CONNECTED，并要求
         * 编码任务在下一张可用画面输出 IDR。低帧率时不能等自然 GOP。 */
        __atomic_add_fetch(&s_pli_received, 1U, __ATOMIC_RELAXED);
        if (s_force_idr != NULL) s_force_idr();
        return 0;
    }
    const esp_peer_state_t previous = s_peer_phase;
    s_peer_phase = state;
    if (previous != state) {
        ESP_LOGI(TAG, "Peer state %s -> %s", peer_state_name(previous),
                 peer_state_name(state));
    }
    if (state == ESP_PEER_STATE_CONNECTED) {
        s_peer_connected = true;
        ESP_LOGI(TAG, "DTLS state=CONNECTED，SRTP keys ready");
        log_memory("AFTER_DTLS");
        report_hosted_pool("AFTER_DTLS");
    } else if (state == ESP_PEER_STATE_PAIRED) {
        s_log_paired_addr = true;
    } else if (state == ESP_PEER_STATE_CONNECT_FAILED ||
               state == ESP_PEER_STATE_DISCONNECTED) {
        s_peer_connected = false;
        s_peer_failed = true;
        ESP_LOGW(TAG, "Peer connection state=%s", peer_state_name(state));
    }
    return 0;
}

static void log_selected_pair(void)
{
    if (!s_log_paired_addr || s_peer == NULL) return;
    s_log_paired_addr = false;
    esp_peer_addr_t address = {0};
    const int result = esp_peer_get_paired_addr(s_peer, &address);
    if (result == ESP_PEER_ERR_NONE) {
        ESP_LOGI(TAG, "ICE_SELECTED remote=%u.%u.%u.%u:%u",
                 address.ipv4[0], address.ipv4[1], address.ipv4[2],
                 address.ipv4[3], address.port);
    } else {
        ESP_LOGW(TAG, "ICE pair 已建立，但读取 selected pair 失败：%d", result);
    }
}

static esp_err_t begin_peer(const webrtc_whip_credential_t *credential)
{
    static esp_peer_ice_server_cfg_t ice_server = {
        .stun_url = WHIP_STUN_URL,
    };
    s_offer = heap_caps_malloc(WHIP_SDP_CAPACITY, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_offer == NULL) {
        return ESP_ERR_NO_MEM;
    }
    s_offer_len = 0;
    s_offer_frozen = false;
    s_offer_revision = 0;
    s_offer_updated_us = 0;
    s_peer_connected = false;
    s_peer_failed = false;
    s_peer_phase = ESP_PEER_STATE_CLOSED;
    s_log_paired_addr = false;

    esp_peer_default_cfg_t peer_options = {
        .agent_recv_timeout = 100,
        /* 当前设备只需要 host/srflx 等少量候选。默认值 16 会在 ICE
         * 收集期间持续消耗紧缺的内部 RAM，4 个已覆盖当前 WHIP 场景。 */
        .max_candidates = 4,
        .rtp_cfg = {
            /* 128 KiB/64 项在连续 PLI 触发的 IDR 突发下可能放不下完整帧。
             * 恢复 esp_peer 文档的默认容量，保留重传所需的 RTP 包。 */
            .send_pool_size = 400 * 1024,
            .send_queue_num = 256,
        },
    };
    esp_peer_cfg_t peer_cfg = {
        .server_lists = &ice_server,
        .server_num = 1,
        .role = ESP_PEER_ROLE_CONTROLLING,
        .ice_trans_policy = ESP_PEER_ICE_TRANS_POLICY_ALL,
        .video_info = {
            .codec = ESP_PEER_VIDEO_CODEC_H264,
            .width = CONFIG_RTC_MEM_DIAG_640P ? 640 : credential->width,
            .height = CONFIG_RTC_MEM_DIAG_640P ? 480 : credential->height,
            .fps = CONFIG_RTC_MEM_DIAG_640P ? 15 : credential->fps,
        },
        .video_dir = ESP_PEER_MEDIA_DIR_SEND_ONLY,
        .audio_dir = ESP_PEER_MEDIA_DIR_NONE,
        .no_auto_reconnect = true,
        .extra_cfg = &peer_options,
        .extra_size = sizeof(peer_options),
        .on_state = peer_state,
        .on_msg = peer_message,
    };
    ESP_LOGI(TAG, "ICE gathering：STUN=%s，policy=ALL，timeout=%u ms",
             WHIP_STUN_URL, (unsigned)WHIP_GATHER_TIMEOUT_MS);
    ESP_LOGI(TAG,
             "esp_peer=1.5.5 A/B：video=sendonly audio=none data_channel=off "
             "ICE role=CONTROLLING");
    log_memory("BEFORE_PEER");
    report_hosted_pool("BEFORE_PEER");
    rtc_heap_diag_peer_begin();
    const int ret = esp_peer_open(&peer_cfg, esp_peer_get_default_impl(), &s_peer);
    rtc_heap_diag_peer_end();
    if (ret != ESP_PEER_ERR_NONE) {
        ESP_LOGE(TAG, "PeerConnection 创建失败：%d", ret);
        return ESP_FAIL;
    }
    log_memory("AFTER_PEER_OPEN");
    report_hosted_pool("AFTER_PEER_OPEN");
    /* esp_peer 在 open 阶段按 video_info/video_dir 创建 send-only Track；
     * transformer 安装完成后即可把这一阶段视为 Track 配置完成。 */
    log_memory("AFTER_TRACK");
    if (esp_peer_new_connection(s_peer) != ESP_PEER_ERR_NONE) {
        ESP_LOGE(TAG, "PeerConnection createOffer 失败");
        return ESP_FAIL;
    }
    log_memory("AFTER_OFFER");
    return ESP_OK;
}

/* Location 可以是绝对 URL、根相对路径或相对路径；不自行拼 publishUrl。 */
static bool resolve_location(const char *base, const char *location,
                             char *resolved, size_t capacity)
{
    if (location[0] == '\0') return false;
    if (strncmp(location, "https://", 8) == 0) {
        return snprintf(resolved, capacity, "%s", location) < (int)capacity;
    }
    if (location[0] == '/' && location[1] == '/') {
        return snprintf(resolved, capacity, "https:%s", location) < (int)capacity;
    }
    const char *authority = strstr(base, "://");
    if (authority == NULL) return false;
    authority += 3;
    const char *path = strchr(authority, '/');
    if (location[0] == '/') {
        const size_t origin_len = path ? (size_t)(path - base) : strlen(base);
        return snprintf(resolved, capacity, "%.*s%s", (int)origin_len, base,
                        location) < (int)capacity;
    }
    const char *end = base + strlen(base);
    const char *slash = end;
    while (slash > authority && slash[-1] != '/') --slash;
    const size_t prefix_len = (size_t)(slash - base);
    return snprintf(resolved, capacity, "%.*s%s", (int)prefix_len, base,
                    location) < (int)capacity;
}

static esp_err_t http_event(esp_http_client_event_t *event)
{
    whip_response_t *response = event->user_data;
    if (response == NULL) return ESP_OK;
    if (event->event_id == HTTP_EVENT_ON_HEADER && event->header_key &&
        event->header_value && strcasecmp(event->header_key, "Location") == 0) {
        if (snprintf(response->location, sizeof(response->location), "%s",
                     event->header_value) >= (int)sizeof(response->location)) {
            response->overflow = true;
        }
    } else if (event->event_id == HTTP_EVENT_ON_DATA && event->data_len > 0) {
        const size_t length = (size_t)event->data_len;
        if (response->answer_len + length >= response->answer_capacity) {
            response->overflow = true;
        } else {
            memcpy(response->answer + response->answer_len, event->data, length);
            response->answer_len += length;
            response->answer[response->answer_len] = '\0';
        }
    }
    return ESP_OK;
}

/* WHIP 凭据 GET 的响应只收 JSON 正文，不记录 URL、ticket 或响应全文。 */
static esp_err_t credential_http_event(esp_http_client_event_t *event)
{
    http_body_response_t *response = event->user_data;
    if (response == NULL || event->event_id != HTTP_EVENT_ON_DATA ||
        event->data_len <= 0) {
        return ESP_OK;
    }
    const size_t length = (size_t)event->data_len;
    if (response->length + length >= response->capacity) {
        response->overflow = true;
        return ESP_OK;
    }
    memcpy(response->body + response->length, event->data, length);
    response->length += length;
    response->body[response->length] = '\0';
    return ESP_OK;
}

static bool url_encode_ticket(const char *input, char *output, size_t capacity)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t used = 0;
    for (const unsigned char *p = (const unsigned char *)input; *p != 0; ++p) {
        const bool unreserved = (*p >= 'A' && *p <= 'Z') ||
                                (*p >= 'a' && *p <= 'z') ||
                                (*p >= '0' && *p <= '9') ||
                                *p == '-' || *p == '_' || *p == '.' || *p == '~';
        const size_t needed = unreserved ? 1U : 3U;
        if (used + needed >= capacity) return false;
        if (unreserved) {
            output[used++] = (char)*p;
        } else {
            output[used++] = '%';
            output[used++] = hex[*p >> 4];
            output[used++] = hex[*p & 0x0f];
        }
    }
    output[used] = '\0';
    return true;
}

static bool copy_json_text(const cJSON *object, const char *name,
                           char *output, size_t capacity)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    if (!cJSON_IsString(item) || item->valuestring == NULL ||
        strlen(item->valuestring) >= capacity) {
        return false;
    }
    snprintf(output, capacity, "%s", item->valuestring);
    return true;
}

/* 使用 MCP 下发的一次性 ticket 获取正式 publishUrl。耗时操作只在 WHIP
 * 控制任务执行；MCP 回调在命令入队后已经立即返回 accepted。 */
static esp_err_t fetch_whip_credential(webrtc_whip_credential_t *credential)
{
    if (credential == NULL || credential->credential_path[0] != '/' ||
        credential->device_ticket[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t url_capacity = strlen(WHIP_API_BASE) +
                                strlen(credential->credential_path) +
                                strlen(credential->device_ticket) * 3U + 32U;
    char *url = heap_caps_malloc(url_capacity,
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    char *body = heap_caps_calloc(1, WHIP_CREDENTIAL_BODY_CAPACITY,
                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (url == NULL || body == NULL) {
        heap_caps_free(url);
        heap_caps_free(body);
        return ESP_ERR_NO_MEM;
    }

    int prefix = snprintf(url, url_capacity, "%s%s?deviceTicket=",
                          WHIP_API_BASE, credential->credential_path);
    if (prefix <= 0 || (size_t)prefix >= url_capacity ||
        !url_encode_ticket(credential->device_ticket, url + prefix,
                           url_capacity - (size_t)prefix)) {
        memset(url, 0, url_capacity);
        heap_caps_free(url);
        heap_caps_free(body);
        return ESP_ERR_INVALID_SIZE;
    }

    http_body_response_t response = {
        .body = body,
        .capacity = WHIP_CREDENTIAL_BODY_CAPACITY,
    };
    const esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = WHIP_CREDENTIAL_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = credential_http_event,
        .user_data = &response,
        .buffer_size = 1024,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
        memset(url, 0, url_capacity);
        heap_caps_free(url);
        heap_caps_free(body);
        return ESP_ERR_NO_MEM;
    }
    esp_http_client_set_header(client, "Accept", "application/json");
    ESP_LOGI(TAG, "WHIP credential GET start（path 已校验，ticket 已隐藏）");
    const esp_err_t request_err = esp_http_client_perform(client);
    const int http_status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    memset(url, 0, url_capacity);
    heap_caps_free(url);

    if (request_err != ESP_OK || http_status != 200 || response.overflow ||
        response.length == 0) {
        ESP_LOGE(TAG, "WHIP credential GET 失败：HTTP=%d err=%s overflow=%d bytes=%u",
                 http_status, esp_err_to_name(request_err), response.overflow,
                 (unsigned)response.length);
        memset(body, 0, WHIP_CREDENTIAL_BODY_CAPACITY);
        heap_caps_free(body);
        return request_err == ESP_OK ? ESP_ERR_INVALID_RESPONSE : request_err;
    }

    esp_err_t result = ESP_ERR_INVALID_RESPONSE;
    cJSON *root = cJSON_ParseWithLength(body, response.length);
    const cJSON *code = root ? cJSON_GetObjectItemCaseSensitive(root, "code") : NULL;
    const cJSON *data = root ? cJSON_GetObjectItemCaseSensitive(root, "data") : NULL;
    const cJSON *video = cJSON_IsObject(data) ?
        cJSON_GetObjectItemCaseSensitive(data, "video") : NULL;
    const cJSON *width = cJSON_IsObject(video) ?
        cJSON_GetObjectItemCaseSensitive(video, "width") : NULL;
    const cJSON *height = cJSON_IsObject(video) ?
        cJSON_GetObjectItemCaseSensitive(video, "height") : NULL;
    const cJSON *fps = cJSON_IsObject(video) ?
        cJSON_GetObjectItemCaseSensitive(video, "fps") : NULL;
    char response_session[WEBRTC_WHIP_SESSION_MAX] = {0};

    if (cJSON_IsNumber(code) && code->valueint == 0 && cJSON_IsObject(data) &&
        copy_json_text(data, "sessionId", response_session,
                       sizeof(response_session)) &&
        strcmp(response_session, credential->session_id) == 0 &&
        copy_json_text(data, "publishUrl", credential->publish_url,
                       sizeof(credential->publish_url)) &&
        strncmp(credential->publish_url, "https://", 8) == 0 &&
        copy_json_text(data, "streamKey", credential->stream_key,
                       sizeof(credential->stream_key)) &&
        copy_json_text(data, "roomName", credential->room_name,
                       sizeof(credential->room_name)) &&
        cJSON_IsNumber(width) && cJSON_IsNumber(height) && cJSON_IsNumber(fps) &&
        width->valueint == credential->width &&
        height->valueint == credential->height &&
        fps->valueint == credential->fps) {
        ESP_LOGI(TAG,
                 "WHIP credential GET OK：session 匹配，video=%ux%u@%u，publishUrl 已隐藏",
                 credential->width, credential->height, credential->fps);
        result = ESP_OK;
    } else {
        ESP_LOGE(TAG,
                 "WHIP credential 响应无效：code=%d session=%s video=%s",
                 cJSON_IsNumber(code) ? code->valueint : -1,
                 response_session[0] ? "匹配检查失败" : "缺失",
                 cJSON_IsObject(video) ? "参数不匹配" : "缺失");
    }
    cJSON_Delete(root);
    memset(body, 0, WHIP_CREDENTIAL_BODY_CAPACITY);
    heap_caps_free(body);
    memset(credential->device_ticket, 0, sizeof(credential->device_ticket));
    return result;
}

static esp_err_t whip_post(const char *url, const char *stream_key,
                           const char *offer, size_t offer_len, char *answer,
                           size_t answer_capacity)
{
    whip_response_t response = {
        .answer = answer,
        .answer_capacity = answer_capacity,
    };
    const esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = WHIP_HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = http_event,
        .user_data = &response,
        .buffer_size = 2048,
        .buffer_size_tx = 2048,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) return ESP_ERR_NO_MEM;
    char authorization[WEBRTC_WHIP_KEY_MAX + 8U];
    if (stream_key == NULL || stream_key[0] == '\0' ||
        snprintf(authorization, sizeof(authorization), "Bearer %s", stream_key) >=
            (int)sizeof(authorization)) {
        esp_http_client_cleanup(client);
        return ESP_ERR_INVALID_ARG;
    }
    esp_http_client_set_header(client, "Content-Type", "application/sdp");
    esp_http_client_set_header(client, "Accept", "application/sdp");
    esp_http_client_set_header(client, "Authorization", authorization);
    esp_http_client_set_post_field(client, offer, (int)offer_len);
    ESP_LOGI(TAG,
             "WHIP POST start；SDP offer bytes=%u，Bearer StreamKey 已设置（值隐藏）",
             (unsigned)offer_len);
    const esp_err_t err = esp_http_client_perform(client);
    const int http_status = esp_http_client_get_status_code(client);
    /* POST 的 TLS/HTTP 对象只服务于本次 Offer/Answer。这里同步销毁，
     * 后续 STOP 需要 DELETE 时重新创建 client，避免整个媒体会话期间占用
     * 连续 INTERNAL 内存。 */
    const esp_err_t cleanup_err = esp_http_client_cleanup(client);
    client = NULL;
    if (cleanup_err != ESP_OK) {
        ESP_LOGW(TAG, "WHIP POST HTTP client 清理异常：%s",
                 esp_err_to_name(cleanup_err));
    }
    memset(authorization, 0, sizeof(authorization));
    ESP_LOGI(TAG, "WHIP response HTTP=%d；SDP answer bytes=%u", http_status,
             (unsigned)response.answer_len);
    if (err == ESP_OK && http_status != 201) {
        log_whip_error_body(http_status, response.answer,
                            response.answer_len);
    }
    /* 只要服务端已创建会话就先保存 Location；后续校验失败也能 DELETE 回收。 */
    if (err == ESP_OK && http_status == 201 && !response.overflow &&
        response.location[0] != '\0') {
        if (!resolve_location(url, response.location, s_location,
                              sizeof(s_location))) return ESP_ERR_INVALID_RESPONSE;
    }
    if (err != ESP_OK || http_status != 201 || response.overflow ||
        response.answer_len == 0 || s_location[0] == '\0') {
        return err == ESP_OK ? ESP_ERR_INVALID_RESPONSE : err;
    }
    return ESP_OK;
}

static void whip_delete(void)
{
    if (s_location[0] == '\0') return;
    const esp_http_client_config_t cfg = {
        .url = s_location,
        .method = HTTP_METHOD_DELETE,
        .timeout_ms = 5000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client != NULL) {
        char authorization[WEBRTC_WHIP_KEY_MAX + 8U];
        if (s_stream_key[0] != '\0' &&
            snprintf(authorization, sizeof(authorization), "Bearer %s",
                     s_stream_key) < (int)sizeof(authorization)) {
            esp_http_client_set_header(client, "Authorization", authorization);
        }
        const esp_err_t err = esp_http_client_perform(client);
        const int code = esp_http_client_get_status_code(client);
        ESP_LOGI(TAG, "WHIP DELETE HTTP=%d err=%s", code, esp_err_to_name(err));
        esp_http_client_cleanup(client);
        memset(authorization, 0, sizeof(authorization));
    }
    memset(s_location, 0, sizeof(s_location));
}

static void stop_session(void)
{
    if (s_state == WEBRTC_WHIP_IDLE) return;
    log_memory("RTC_STOP");
    ESP_LOGI(TAG, "RTC_MEMORY_MIN sampled_1s session=%u/%u stream=%u/%u duration_s=%lld",
             (unsigned)s_session_dma_min, (unsigned)s_session_largest_min,
             (unsigned)(s_stream_dma_min == SIZE_MAX ? 0 : s_stream_dma_min),
             (unsigned)(s_stream_largest_min == SIZE_MAX ? 0 : s_stream_largest_min),
             s_stream_started_us ? (esp_timer_get_time() - s_stream_started_us) / 1000000LL : 0LL);
    set_state(WEBRTC_WHIP_STOPPING);
    s_peer_connected = false;
    if (s_video_stop) (void)s_video_stop();
    whip_delete();
    xSemaphoreTake(s_guard, portMAX_DELAY);
    if (s_peer != NULL) {
        ESP_LOGI(TAG, "RTC_STOP peer_close begin stack_high_water=%u",
                 (unsigned)uxTaskGetStackHighWaterMark(NULL));
        const int close_result = esp_peer_close(s_peer);
        s_peer = NULL;
        ESP_LOGI(TAG, "RTC_STOP peer_close end result=%d", close_result);
    }
    xSemaphoreGive(s_guard);
    if (s_camera_stop != NULL) {
        const esp_err_t camera_err = s_camera_stop();
        if (camera_err != ESP_OK) {
            ESP_LOGW(TAG, "停止 RTC 摄像头/UVC stream 失败：%s",
                     esp_err_to_name(camera_err));
        }
    }
    heap_caps_free(s_offer);
    s_offer = NULL;
    s_offer_len = 0;
    memset(s_session_id, 0, sizeof(s_session_id));
    memset(s_stream_key, 0, sizeof(s_stream_key));
    set_state(WEBRTC_WHIP_IDLE);
    /* 视频停止、Peer 释放后再恢复 AFE；恢复失败明确报错。 */
    if (s_audio_suspended && s_audio_control) {
        esp_err_t audio_err = s_audio_control(false);
        s_audio_suspended = audio_err != ESP_OK;
        if (audio_err != ESP_OK) ESP_LOGE(TAG, "恢复本地语音失败：%s", esp_err_to_name(audio_err));
    }
    log_memory("STOPPED");
}

static esp_err_t start_session(webrtc_whip_credential_t *credential)
{
    if (!network_manager_is_connected() || s_video_start == NULL ||
        s_video_stop == NULL || s_state != WEBRTC_WHIP_STARTING) {
        return ESP_ERR_INVALID_STATE;
    }
    set_state(WEBRTC_WHIP_STARTING);
    log_memory("RTC_START");
    report_hosted_pool("RTC_START");
    log_memory("PEER_BEFORE");
    snprintf(s_session_id, sizeof(s_session_id), "%s", credential->session_id);
    s_location[0] = '\0';
    /* 现行协议先用 credentialPath + deviceTicket 换取完整 publishUrl。
     * publishUrl 随后原样作为 WHIP POST 目标，不用 streamKey 重拼 URL。 */
    esp_err_t credential_err = fetch_whip_credential(credential);
    if (credential_err != ESP_OK) return credential_err;
    snprintf(s_stream_key, sizeof(s_stream_key), "%s", credential->stream_key);

    log_memory("H264_READY_BEFORE_PEER");

    log_memory("WHIP_BEFORE_BEGIN_PEER");
    if (begin_peer(credential) != ESP_OK) {
        return ESP_FAIL;
    }
    log_memory("PEER_OPEN");

    /* 公网 non-trickle 模式必须先获得 srflx/relay。只有私网 host candidate
     * 时继续 POST 虽然也可能得到 HTTP 201，但后续一定无法形成公网 pair。 */
    const int64_t started_us = esp_timer_get_time();
    bool gathered = false;
    ice_candidate_summary_t ice_summary = {0};
    while ((esp_timer_get_time() - started_us) / 1000 < WHIP_GATHER_TIMEOUT_MS) {
        control_command_t pending = {0};
        if (xQueuePeek(s_commands, &pending, 0) == pdTRUE &&
            pending.type == COMMAND_STOP) {
            ESP_LOGI(TAG, "ICE 收集期间收到停止命令");
            return ESP_ERR_INVALID_STATE;
        }
        esp_peer_main_loop(s_peer);
        xSemaphoreTake(s_offer_guard, portMAX_DELAY);
        ice_summary = summarize_ice_candidates(s_offer, false);
        const int64_t quiet_ms = s_offer_updated_us > 0
                                     ? (esp_timer_get_time() - s_offer_updated_us) / 1000
                                     : 0;
        gathered = offer_has_public_candidate(&ice_summary) &&
                   (ice_summary.end_of_candidates ||
                    quiet_ms >= WHIP_GATHER_STABLE_MS);
        xSemaphoreGive(s_offer_guard);
        if (gathered) break;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (!gathered) {
        xSemaphoreTake(s_offer_guard, portMAX_DELAY);
        ice_summary = summarize_ice_candidates(s_offer, false);
        xSemaphoreGive(s_offer_guard);
        ESP_LOGE(TAG,
                 "ICE_GATHER_NO_PUBLIC_CANDIDATE：timeout=%u ms total=%u "
                 "host=%u srflx=%u relay=%u；拒绝发送 Offer",
                 (unsigned)WHIP_GATHER_TIMEOUT_MS, ice_summary.total,
                 ice_summary.host, ice_summary.srflx, ice_summary.relay);
        return ESP_ERR_TIMEOUT;
    }

    xSemaphoreTake(s_offer_guard, portMAX_DELAY);
    s_offer_frozen = true;
    if (!append_end_of_candidates(s_offer, &s_offer_len)) {
        xSemaphoreGive(s_offer_guard);
        ESP_LOGE(TAG, "ICE SDP 空间不足，无法追加 end-of-candidates");
        return ESP_ERR_NO_MEM;
    }
    ice_summary = summarize_ice_candidates(s_offer, true);
    const bool fresh_credentials = validate_fresh_ice_credentials(s_offer);
    const unsigned normalized_profiles = normalize_h264_offer_profile(s_offer);
    xSemaphoreGive(s_offer_guard);
    if (!fresh_credentials) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGI(TAG,
             "ICE gathering complete：elapsed=%lld ms total=%u host=%u "
             "srflx=%u relay=%u end_of_candidates=%s revision=%" PRIu32,
             (long long)((esp_timer_get_time() - started_us) / 1000),
             ice_summary.total, ice_summary.host, ice_summary.srflx,
             ice_summary.relay, ice_summary.end_of_candidates ? "yes" : "no",
             s_offer_revision);
    log_memory("AFTER_ICE");
    report_hosted_pool("AFTER_ICE");
    if (normalized_profiles > 0) {
        ESP_LOGI(TAG, "SDP H264 profile 已规范化为 42e01f（%u 处）",
                 normalized_profiles);
    }
    /* 只输出协商摘要，不打印含 ICE 凭据/指纹的完整 SDP。 */
    ESP_LOGI(TAG, "SDP H264=%s profile42e01f=%s packetization1=%s public_candidate=%s",
             strstr(s_offer, "H264/90000") ? "yes" : "no",
             strstr(s_offer, "profile-level-id=42e01f") ? "yes" : "no",
             strstr(s_offer, "packetization-mode=1") ? "yes" : "no",
             offer_has_public_candidate(&ice_summary) ? "yes" : "no");

    /* HTTP 期间 SDP 不再变化：PeerConnection 的网络轮询在 Answer 后继续。 */
    char *answer = heap_caps_malloc(WHIP_SDP_CAPACITY,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (answer == NULL) {
        return ESP_ERR_NO_MEM;
    }

    /* 从这一行开始 HTTPS 可以使用刚释放的连续内部块。日志用于确认
     * socket 创建前确实恢复了足够的 DMA/internal heap。 */
    log_memory("WHIP_HTTP_READY");
    const esp_err_t post_err = whip_post(credential->publish_url,
                                        credential->stream_key, s_offer,
                                        s_offer_len, answer, WHIP_SDP_CAPACITY);
    if (post_err == ESP_OK) {
        log_remote_sdp_summary(answer);
        esp_peer_msg_t remote = {
            .type = ESP_PEER_MSG_TYPE_SDP,
            .data = (uint8_t *)answer,
            .size = (int)strlen(answer),
        };
        if (esp_peer_send_msg(s_peer, &remote) != ESP_PEER_ERR_NONE) {
            heap_caps_free(answer);
            return ESP_FAIL;
        }
        log_memory("AFTER_REMOTE_SDP");
    }
    heap_caps_free(answer);
    answer = NULL;
    if (post_err != ESP_OK) return post_err;

    /* set_remote_description 已同步解析 Answer，本地 Offer 也不会再变化。
     * 两块 SDP 都位于 PSRAM，但此处明确结束它们的生命周期，保证会话长期
     * 只保留 Location 与 STOP/DELETE 必需的鉴权字符串。 */
    xSemaphoreTake(s_offer_guard, portMAX_DELAY);
    heap_caps_free(s_offer);
    s_offer = NULL;
    s_offer_len = 0;
    xSemaphoreGive(s_offer_guard);
    log_memory("AFTER_WHIP_HTTP_FREE");
    set_state(WEBRTC_WHIP_CONNECTING);
    return ESP_OK;
}

static void report_stats(void)
{
    static uint32_t previous_frames, previous_bytes;
    const uint32_t frames = __atomic_load_n(&s_h264_frames, __ATOMIC_RELAXED);
    const uint32_t bytes = __atomic_load_n(&s_h264_bytes, __ATOMIC_RELAXED);
    ESP_LOGI(TAG, "[WEBRTC] state=%s peer=%s h264_fps=%" PRIu32
             " bitrate=%" PRIu32
             " kbps idr=%" PRIu32 " pli=%" PRIu32 " send_fail=%" PRIu32,
             state_name(s_state), peer_state_name(s_peer_phase),
             (frames - previous_frames) / 5U,
             (bytes - previous_bytes) * 8U / 5000U,
             __atomic_load_n(&s_idr_count, __ATOMIC_RELAXED),
             __atomic_load_n(&s_pli_received, __ATOMIC_RELAXED),
             __atomic_load_n(&s_send_fail, __ATOMIC_RELAXED));
    previous_frames = frames;
    previous_bytes = bytes;
    if (s_state == WEBRTC_WHIP_STREAMING) {
        log_memory("STREAMING_5S");
        if (esp_hosted_sdio_rx_dma_report != NULL) esp_hosted_sdio_rx_dma_report();
        ESP_LOGI(TAG, "STREAMING_MIN sampled_1s DMA=%u/%u duration_s=%lld",
                 (unsigned)s_stream_dma_min, (unsigned)s_stream_largest_min,
                 (esp_timer_get_time() - s_stream_started_us) / 1000000LL);
    }
#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
    /* 以 1 MHz ESP Timer 计数器的差值计算每个核/任务的 5 秒占用率。
     * FreeRTOS 非 SMP 内核仅提供全任务快照；PSRAM 存一份固定上限的
     * 诊断数组，每 5 秒读取一次，不在热路径逐帧扫描。 */
    static TaskStatus_t *snapshot;
    static uint64_t previous_us;
    static uint64_t previous_idle[2];
    static uint64_t previous_task[5];
    static TaskHandle_t previous_task_handle[5];
    static const char *const task_names[5] = {
        "video_codec", "taskLVGL", "anim_player", "video_upload", "xiaozhi_dec"
    };
    const uint64_t now_us = (uint64_t)esp_timer_get_time();
    if (snapshot == NULL) {
        snapshot = heap_caps_calloc(64, sizeof(*snapshot),
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (snapshot != NULL) {
    const UBaseType_t task_count = uxTaskGetSystemState(snapshot, 64, NULL);
    uint64_t idle[2] = {0};
    uint64_t task_runtime[5] = {0};
    TaskHandle_t task_handle[5] = {0};
    const TaskHandle_t idle_handle[2] = {
        xTaskGetIdleTaskHandleForCore(0), xTaskGetIdleTaskHandleForCore(1)
    };
    if (task_count == 0) {
        ESP_LOGW(TAG, "TASK_LOAD_5S 快照容量64不足，未输出占用率");
    }
    for (UBaseType_t j = 0; j < task_count; ++j) {
        if (snapshot[j].xHandle == idle_handle[0]) idle[0] = snapshot[j].ulRunTimeCounter;
        if (snapshot[j].xHandle == idle_handle[1]) idle[1] = snapshot[j].ulRunTimeCounter;
        for (size_t i = 0; i < 5; ++i) {
            if (strcmp(snapshot[j].pcTaskName, task_names[i]) == 0) {
                task_runtime[i] = snapshot[j].ulRunTimeCounter;
                task_handle[i] = snapshot[j].xHandle;
            }
        }
    }
    if (task_count != 0 && previous_us != 0 && now_us > previous_us) {
        const uint64_t elapsed = now_us - previous_us;
        const uint64_t idle0_pct = idle[0] >= previous_idle[0] ?
            (idle[0] - previous_idle[0]) * 100U / elapsed : 0U;
        const uint64_t idle1_pct = idle[1] >= previous_idle[1] ?
            (idle[1] - previous_idle[1]) * 100U / elapsed : 0U;
        const unsigned busy0 = idle0_pct < 100U ? 100U - (unsigned)idle0_pct : 0U;
        const unsigned busy1 = idle1_pct < 100U ? 100U - (unsigned)idle1_pct : 0U;
        unsigned pct[5] = {0};
        for (size_t i = 0; i < 5; ++i) {
            if (task_handle[i] != NULL && task_handle[i] == previous_task_handle[i] &&
                task_runtime[i] >= previous_task[i]) {
                const uint64_t usage = (task_runtime[i] - previous_task[i]) * 100U / elapsed;
                pct[i] = usage < 100U ? (unsigned)usage : 100U;
            }
        }
        ESP_LOGI(TAG, "TASK_LOAD_5S CPU0=%u%% CPU1=%u%% codec=%u%% LVGL=%u%% "
                      "anim=%u%% upload=%u%% audio_dec=%u%%",
                 busy0, busy1, pct[0], pct[1], pct[2], pct[3], pct[4]);
    }
    previous_us = now_us;
    memcpy(previous_idle, idle, sizeof(idle));
    memcpy(previous_task, task_runtime, sizeof(task_runtime));
    memcpy(previous_task_handle, task_handle, sizeof(task_handle));
    }
#endif
    rtc_heap_diag_report_first_failure();
}

static void controller_task(void *arg)
{
    (void)arg;
    int64_t last_report_us = esp_timer_get_time();
    int64_t connect_started_us = 0;
    bool start_pending = false;
    bool camera_start_requested = false;
    bool resources_prepared = false;
    int64_t camera_wait_started_us = 0;
    webrtc_whip_credential_t pending_credential = {0};
    for (;;) {
        control_command_t command = {0};
        if (xQueueReceive(s_commands, &command, pdMS_TO_TICKS(20)) == pdTRUE) {
            if (command.type == COMMAND_STOP) {
                const bool session_matches = command.session_id[0] == '\0' ||
                    s_session_id[0] == '\0' ||
                    strcmp(command.session_id, s_session_id) == 0;
                if (start_pending && session_matches) {
                    ESP_LOGI(TAG, "RTC 待启动请求已取消，尚未创建 PeerConnection");
                    memset(&pending_credential, 0, sizeof(pending_credential));
                    memset(s_session_id, 0, sizeof(s_session_id));
                    start_pending = false;
                    resources_prepared = false;
                    camera_start_requested = false;
                    stop_session();
                } else if (session_matches) {
                    stop_session();
                }
            } else if (s_state == WEBRTC_WHIP_IDLE && !start_pending) {
                pending_credential = command.credential;
                start_pending = true;
                camera_start_requested = false;
                camera_wait_started_us = 0;
                resources_prepared = false;
                ESP_LOGI(TAG,
                         "RTC 请求已入队，等待前置条件：CAMERA_READY=%d SD_INIT_DONE=%d",
                         __atomic_load_n(&s_camera_ready, __ATOMIC_ACQUIRE),
                         __atomic_load_n(&s_storage_init_done, __ATOMIC_ACQUIRE));
            }
            memset(&command, 0, sizeof(command));
        }

        /* 先分配实际 H264 资源，再让 UVC/Peer 小块分配进入堆；失败统一回滚。 */
        if (start_pending && !resources_prepared &&
            __atomic_load_n(&s_storage_init_done, __ATOMIC_ACQUIRE)) {
            s_session_dma_min = s_session_largest_min = SIZE_MAX;
            s_stream_dma_min = s_stream_largest_min = SIZE_MAX;
            s_stream_started_us = 0;
            set_state(WEBRTC_WHIP_STARTING);
            log_memory("RTC_START_BEFORE_RESOURCES");
            mem_contig_log("RTC_START");
            ESP_LOGI(TAG, "RTC_RESOURCE_MODE=no_guard 720p encoder_before_peer=1 STA_TX_limit=2 wait_ms=20");
            /* 先确认 AFE 已释放，再申请 H264/UVC/Peer 的运行资源。 */
            esp_err_t err = s_audio_control ? s_audio_control(true) : ESP_OK;
            if (err == ESP_OK && s_audio_control) s_audio_suspended = true;
            if (err == ESP_OK && s_resource_prepare) err = s_resource_prepare(1500);
            if (err == ESP_OK) err = s_video_start ? s_video_start() : ESP_ERR_INVALID_STATE;
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "RTC 实际资源准备失败：%s", esp_err_to_name(err));
                start_pending = false;
                memset(&pending_credential, 0, sizeof(pending_credential));
                stop_session();
            } else {
                resources_prepared = true;
                log_memory(__atomic_load_n(&s_camera_ready, __ATOMIC_ACQUIRE) ?
                           "H264_READY_CAMERA_RUNNING" : "H264_READY_BEFORE_CAMERA");
            }
        }

        if (start_pending &&
            __atomic_load_n(&s_storage_init_done, __ATOMIC_ACQUIRE) &&
            !__atomic_load_n(&s_camera_ready, __ATOMIC_ACQUIRE) &&
            s_camera_start != NULL && s_camera_is_ready != NULL) {
            if (!camera_start_requested) {
                /* SD/UI 初始化完成后才启动摄像头，避免开机持续占用 USB 与 PSRAM。 */
                const esp_err_t camera_err = s_camera_start();
                if (camera_err != ESP_OK) {
                    ESP_LOGE(TAG, "RTC 请求启动摄像头失败：%s",
                             esp_err_to_name(camera_err));
                    memset(&pending_credential, 0, sizeof(pending_credential));
                    memset(s_session_id, 0, sizeof(s_session_id));
                    start_pending = false;
                    resources_prepared = false;
                    stop_session();
                } else {
                    camera_start_requested = true;
                    camera_wait_started_us = esp_timer_get_time();
                    ESP_LOGI(TAG, "RTC 已请求摄像头预热，等待首帧 CAMERA_READY");
                }
            } else if (esp_timer_get_time() - camera_wait_started_us > 60000000LL) {
                ESP_LOGE(TAG, "RTC 等待摄像头首帧超时（60 s），取消本次预览");
                memset(&pending_credential, 0, sizeof(pending_credential));
                memset(s_session_id, 0, sizeof(s_session_id));
                start_pending = false;
                camera_start_requested = false;
                resources_prepared = false;
                stop_session();
            }
        }

        if (start_pending &&
            __atomic_load_n(&s_camera_ready, __ATOMIC_ACQUIRE) &&
            __atomic_load_n(&s_storage_init_done, __ATOMIC_ACQUIRE)) {
            ESP_LOGI(TAG, "RTC 前置条件已满足，开始创建 PeerConnection");
            const esp_err_t err = start_session(&pending_credential);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "WHIP 启动失败：%s", esp_err_to_name(err));
                stop_session();
            } else {
                connect_started_us = esp_timer_get_time();
            }
            memset(&pending_credential, 0, sizeof(pending_credential));
            start_pending = false;
        }
        if (s_peer != NULL) {
            esp_peer_main_loop(s_peer);
            log_selected_pair();
        }
        if (s_state == WEBRTC_WHIP_CONNECTING && s_peer_connected) {
            set_state(WEBRTC_WHIP_DTLS_CONNECTED);
            log_memory("AFTER_DTLS");
            set_state(WEBRTC_WHIP_MEDIA_PREPARING);
            if (s_force_idr) s_force_idr();
            set_state(WEBRTC_WHIP_H264_OPENING);
            /* 编码器已在 UVC/Peer 之前打开；此时才允许输入帧通过。 */
            if (resources_prepared) {
                s_stream_started_us = esp_timer_get_time();
                set_state(WEBRTC_WHIP_STREAMING);
                log_memory("STREAMING");
            } else {
                ESP_LOGE(TAG, "H264 编码链启动失败");
                stop_session();
            }
        } else if (s_state == WEBRTC_WHIP_CONNECTING &&
                   (s_peer_failed || esp_timer_get_time() - connect_started_us >
                    WHIP_CONNECT_TIMEOUT_MS * 1000LL)) {
            ESP_LOGE(TAG, "ICE/DTLS 连接失败或超时");
            stop_session();
        } else if (s_state == WEBRTC_WHIP_STREAMING && s_peer_failed) {
            ESP_LOGW(TAG, "ICE/DTLS 断线，停止本次直播");
            stop_session();
        }
        if (s_state != WEBRTC_WHIP_IDLE && esp_timer_get_time() - s_last_mem_sample_us >= 1000000LL) {
            sample_memory();
            s_last_mem_sample_us = esp_timer_get_time();
        }
        if (esp_timer_get_time() - last_report_us >= WHIP_REPORT_PERIOD_US) {
            report_stats();
            last_report_us = esp_timer_get_time();
            if (s_state == WEBRTC_WHIP_STREAMING &&
                heap_caps_get_largest_free_block(MALLOC_CAP_DMA) < 2048) {
                /* 不在 DMA 最大连续块不足一个 Hosted 包时继续保持全部模块运行。
                 * 由控制任务正常关闭本次 RTC，保留完整内存现场。 */
                ESP_LOGE(TAG, "DMA_BUDGET_CRITICAL largest<2048，停止本次 RTC");
                stop_session();
            }
        }
        if (s_state == WEBRTC_WHIP_IDLE && !start_pending) {
            xSemaphoreTake(s_guard, portMAX_DELAY);
            if (uxQueueMessagesWaiting(s_commands) == 0) {
                s_task = NULL;
                xSemaphoreGive(s_guard);
                break;
            }
            xSemaphoreGive(s_guard);
        }
    }
    vTaskDeleteWithCaps(NULL);
}

esp_err_t webrtc_whip_init(void)
{
    if (s_commands != NULL) return ESP_OK;
    const esp_err_t heap_diag_err = rtc_heap_diag_init();
    if (heap_diag_err != ESP_OK) {
        ESP_LOGW(TAG, "RTC heap failure callback registration=%s", esp_err_to_name(heap_diag_err));
    }
    /* esp_peer 默认实现会按 RTP 包输出 PEER_DEF INFO；串口刷屏会抢占
     * UVC 与视频任务。保留 WARN/ERROR 和本模块的阶段统计即可。 */
    esp_log_level_set("PEER_DEF", ESP_LOG_WARN);
    s_guard = xSemaphoreCreateMutex();
    s_offer_guard = xSemaphoreCreateMutex();
    /* 控制命令包含一次性 ticket 和 credentialPath，单项较大；队列数据不参与
     * DMA，明确放 PSRAM，避免在摄像头启动前切割 H.264 所需的内部连续块。 */
    s_commands_storage = heap_caps_malloc(2U * sizeof(control_command_t),
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_commands_storage != NULL) {
        s_commands = xQueueCreateStatic(2, sizeof(control_command_t),
                                        s_commands_storage,
                                        &s_commands_control);
    }
    if (s_guard == NULL || s_offer_guard == NULL || s_commands == NULL) {
        if (s_commands) vQueueDelete(s_commands);
        if (s_guard) vSemaphoreDelete(s_guard);
        if (s_offer_guard) vSemaphoreDelete(s_offer_guard);
        heap_caps_free(s_commands_storage);
        s_commands_storage = NULL;
        s_commands = NULL;
        s_guard = s_offer_guard = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void webrtc_whip_notify_camera_ready(void)
{
    const bool was_ready = __atomic_exchange_n(&s_camera_ready, true,
                                                __ATOMIC_ACQ_REL);
    if (!was_ready) {
        ESP_LOGI(TAG, "RTC 前置条件 CAMERA_READY 已满足");
    }
}

void webrtc_whip_notify_camera_unready(void)
{
    const bool was_ready = __atomic_exchange_n(&s_camera_ready, false,
                                                __ATOMIC_ACQ_REL);
    if (was_ready) {
        ESP_LOGW(TAG, "RTC 前置条件 CAMERA_READY 已撤销：等待 UVC 新流首帧");
        /* 正在建链或推流的旧会话已无可用视频源；只向控制队列投递停止，
         * 不在 UVC 回调中同步销毁 PeerConnection。新流就绪后可重新 start。 */
        if (s_state != WEBRTC_WHIP_STOPPING &&
            (s_task != NULL || s_state != WEBRTC_WHIP_IDLE)) {
            const esp_err_t err = webrtc_whip_request_stop(NULL);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "UVC 失效后投递 RTC 停止失败：%s",
                         esp_err_to_name(err));
            }
        }
    }
}

void webrtc_whip_notify_storage_init_done(void)
{
    const bool was_done = __atomic_exchange_n(&s_storage_init_done, true,
                                               __ATOMIC_ACQ_REL);
    if (!was_done) {
        ESP_LOGI(TAG, "RTC 前置条件 SD_INIT_DONE 已满足");
    }
}

esp_err_t webrtc_whip_request_start(const webrtc_whip_credential_t *credential)
{
    if (credential == NULL || s_commands == NULL ||
        credential->credential_path[0] != '/' ||
        credential->device_ticket[0] == '\0' ||
        credential->session_id[0] == '\0' || !credential->video ||
        credential->width == 0 || credential->height == 0 || credential->fps == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_guard, portMAX_DELAY);
    if (s_state != WEBRTC_WHIP_IDLE || s_task != NULL) {
        const bool same = strcmp(s_session_id, credential->session_id) == 0;
        xSemaphoreGive(s_guard);
        return same ? ESP_OK : ESP_ERR_INVALID_STATE;
    }
    control_command_t command = { .type = COMMAND_START, .credential = *credential };
    if (xQueueSend(s_commands, &command, 0) != pdTRUE) {
        xSemaphoreGive(s_guard);
        return ESP_ERR_NO_MEM;
    }
    snprintf(s_session_id, sizeof(s_session_id), "%s", credential->session_id);
    if (xTaskCreatePinnedToCoreWithCaps(controller_task, "webrtc_whip", 12288,
                                        NULL, 4, &s_task, 0,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        xQueueReset(s_commands);
        memset(s_session_id, 0, sizeof(s_session_id));
        xSemaphoreGive(s_guard);
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreGive(s_guard);
    memset(&command, 0, sizeof(command));
    return ESP_OK;
}

esp_err_t webrtc_whip_request_stop(const char *session_id)
{
    if (s_commands == NULL) return ESP_ERR_INVALID_STATE;
    /* 停止正在执行时，重复的服务端命令不再占用仅有的控制队列。 */
    if (s_state == WEBRTC_WHIP_STOPPING ||
        (s_state == WEBRTC_WHIP_IDLE && s_task == NULL)) return ESP_OK;
    control_command_t command = { .type = COMMAND_STOP };
    if (session_id) snprintf(command.session_id, sizeof(command.session_id), "%s", session_id);
    return xQueueSend(s_commands, &command, 0) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t webrtc_whip_send_h264(const uint8_t *annex_b, size_t length,
                                uint32_t pts_ms, bool idr)
{
    if (annex_b == NULL || length == 0 || length > INT32_MAX) return ESP_ERR_INVALID_ARG;
    if (s_state != WEBRTC_WHIP_STREAMING || !s_peer_connected) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_guard, 0) != pdTRUE) return ESP_ERR_TIMEOUT;
    esp_err_t err = ESP_ERR_INVALID_STATE;
    if (s_peer != NULL && s_state == WEBRTC_WHIP_STREAMING) {
        esp_peer_video_frame_t frame = {
            .pts = pts_ms,
            .data = (uint8_t *)annex_b,
            .size = (int)length,
        };
        const int peer_result = esp_peer_send_video(s_peer, &frame);
        err = peer_result == ESP_PEER_ERR_NONE ? ESP_OK : ESP_FAIL;
    }
    xSemaphoreGive(s_guard);
    if (err == ESP_OK) {
        __atomic_add_fetch(&s_h264_frames, 1U, __ATOMIC_RELAXED);
        __atomic_add_fetch(&s_h264_bytes, (uint32_t)length, __ATOMIC_RELAXED);
        if (idr) __atomic_add_fetch(&s_idr_count, 1U, __ATOMIC_RELAXED);
    } else {
        __atomic_add_fetch(&s_send_fail, 1U, __ATOMIC_RELAXED);
    }
    return err;
}
