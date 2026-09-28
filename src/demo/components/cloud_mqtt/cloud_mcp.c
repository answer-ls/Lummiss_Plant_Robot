#include "cloud_mcp.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "ambient_led.h"
#include "board_pins.h"
#include "camera_photo.h"
#include "cloud_mqtt.h"
#include "mcp_registry.h"
#include "stepper_motor.h"
#include "video_streamer.h"
#include "webrtc_whip.h"

static const char *TAG = "CLOUD_MCP";

/* WHIP 凭据随 MCP 发送，SDP 不经 MQTT。队列存储放 PSRAM，给 USB 保留内部 RAM。 */
#define CLOUD_MCP_QUEUE_DEPTH  4
#define CLOUD_MCP_MSG_MAX      3072

#define CLOUD_MCP_TASK_STACK   8192
#define CLOUD_MCP_TASK_PRIO    4
#define CLOUD_MCP_TASK_CORE    0

typedef struct {
    char text[CLOUD_MCP_MSG_MAX];
} mcp_message_t;

typedef struct {
    bool is_error;
    char text[512];   /* 工具返回体，是一段 JSON 文本 */
} mcp_tool_result_t;

static QueueHandle_t s_queue;
static StaticQueue_t s_queue_control;
static uint8_t *s_queue_storage;
static bool s_initialized;

/* ---------------- JSON-RPC 回执 ---------------- */

static void publish_reply(cJSON *id, cJSON *result)
{
    cJSON *payload = cJSON_CreateObject();
    cJSON *env = cJSON_CreateObject();
    if (payload == NULL || env == NULL) {
        cJSON_Delete(payload);
        cJSON_Delete(env);
        cJSON_Delete(result);
        return;
    }
    cJSON_AddStringToObject(payload, "jsonrpc", "2.0");
    /* id 可能是数字也可能是字符串，原样回显才符合 JSON-RPC 对 id 的要求。 */
    if (id != NULL) {
        cJSON_AddItemToObject(payload, "id", cJSON_Duplicate(id, true));
    }
    if (result != NULL) {
        cJSON_AddItemToObject(payload, "result", result);
    }
    cJSON_AddStringToObject(env, "type", "mcp");
    cJSON_AddItemToObject(env, "payload", payload);

    char *text = cJSON_PrintUnformatted(env);
    if (text != NULL) {
        if (cloud_mqtt_publish_text(text) != ESP_OK) {
            ESP_LOGW(TAG, "MCP 回执发布失败");
        }
        cJSON_free(text);
    }
    cJSON_Delete(env);
}

static void publish_error(cJSON *id, int code, const char *message)
{
    cJSON *payload = cJSON_CreateObject();
    cJSON *env = cJSON_CreateObject();
    cJSON *error = cJSON_CreateObject();
    if (payload == NULL || env == NULL || error == NULL) {
        cJSON_Delete(payload);
        cJSON_Delete(env);
        cJSON_Delete(error);
        return;
    }
    cJSON_AddNumberToObject(error, "code", code);
    cJSON_AddStringToObject(error, "message", message);
    cJSON_AddStringToObject(payload, "jsonrpc", "2.0");
    if (id != NULL) {
        cJSON_AddItemToObject(payload, "id", cJSON_Duplicate(id, true));
    }
    cJSON_AddItemToObject(payload, "error", error);
    cJSON_AddStringToObject(env, "type", "mcp");
    cJSON_AddItemToObject(env, "payload", payload);

    char *text = cJSON_PrintUnformatted(env);
    if (text != NULL) {
        (void)cloud_mqtt_publish_text(text);
        cJSON_free(text);
    }
    cJSON_Delete(env);
}

/* 把工具返回体包成 MCP 的 result：{"content":[{"type":"text","text":"<JSON>"}],"isError":...} */
static void publish_tool_result(cJSON *id, const mcp_tool_result_t *res)
{
    cJSON *result = cJSON_CreateObject();
    cJSON *content = cJSON_CreateArray();
    cJSON *item = cJSON_CreateObject();
    if (result == NULL || content == NULL || item == NULL) {
        cJSON_Delete(result);
        cJSON_Delete(content);
        cJSON_Delete(item);
        return;
    }
    cJSON_AddStringToObject(item, "type", "text");
    cJSON_AddStringToObject(item, "text", res->text);
    cJSON_AddItemToArray(content, item);
    cJSON_AddItemToObject(result, "content", content);
    cJSON_AddBoolToObject(result, "isError", res->is_error);
    publish_reply(id, result);
}

/* 拍照请求必须跨越 cloud_mcp 与 photo_task 两个任务。这里只保存序列化后的
 * JSON-RPC id，避免持有 dispatch_message 即将释放的 cJSON 节点。 */
typedef struct {
    char request_id_json[128];
} photo_mcp_context_t;

static void photo_publish_result(const camera_photo_result_t *photo, void *arg)
{
    photo_mcp_context_t *ctx = (photo_mcp_context_t *)arg;
    if (photo == NULL || ctx == NULL) {
        heap_caps_free(ctx);
        return;
    }

    cJSON *id = cJSON_Parse(ctx->request_id_json);
    mcp_tool_result_t result = {0};
    if (photo->status == CAMERA_PHOTO_OK) {
        /* 第一阶段后端尚未提供上传接口，因此明确返回本地验证文件。
         * 接入 HTTPS 上传后只需把 format/data 改为 URL，不改抓图链路。 */
        snprintf(result.text, sizeof(result.text),
                 "{\"format\":\"LOCAL_FILE\",\"data\":\"%s\","
                 "\"mimeType\":\"image/jpeg\",\"jpegSize\":%u,"
                 "\"captureMs\":%u,\"totalMs\":%u}",
                 photo->path != NULL ? photo->path : CAMERA_PHOTO_TEST_PATH,
                 (unsigned)photo->jpeg_size, (unsigned)photo->capture_ms,
                 (unsigned)photo->total_ms);
        result.is_error = false;
    } else {
        snprintf(result.text, sizeof(result.text),
                 "{\"error\":\"%s\",\"captureMs\":%u,"
                 "\"totalMs\":%u}",
                 camera_photo_status_name(photo->status),
                 (unsigned)photo->capture_ms, (unsigned)photo->total_ms);
        result.is_error = true;
    }

    ESP_LOGI(TAG, "tools/call self.camera.take_photo -> %s", result.text);
    publish_tool_result(id, &result);
    cJSON_Delete(id);
    memset(ctx, 0, sizeof(*ctx));
    heap_caps_free(ctx);
}

static void tool_camera_take_photo_async(const cJSON *args,
                                         const cJSON *request_id)
{
    mcp_tool_result_t result = { .is_error = true };

    /* 工具定义为无参数；空 arguments 对象仍然合法。 */
    if (args != NULL && args->child != NULL) {
        snprintf(result.text, sizeof(result.text),
                 "{\"error\":\"INVALID_ARGUMENTS\","
                 "\"detail\":\"self.camera.take_photo takes no arguments\"}");
        publish_tool_result((cJSON *)request_id, &result);
        return;
    }

    photo_mcp_context_t *ctx = heap_caps_calloc(
        1, sizeof(*ctx), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ctx == NULL) {
        snprintf(result.text, sizeof(result.text),
                 "{\"error\":\"PHOTO_NO_MEMORY\"}");
        publish_tool_result((cJSON *)request_id, &result);
        return;
    }

    char *id_text = request_id != NULL
                        ? cJSON_PrintUnformatted((cJSON *)request_id)
                        : NULL;
    if (id_text == NULL || strlen(id_text) >= sizeof(ctx->request_id_json)) {
        cJSON_free(id_text);
        heap_caps_free(ctx);
        snprintf(result.text, sizeof(result.text),
                 "{\"error\":\"INVALID_REQUEST_ID\"}");
        publish_tool_result((cJSON *)request_id, &result);
        return;
    }
    memcpy(ctx->request_id_json, id_text, strlen(id_text) + 1U);
    cJSON_free(id_text);

    const esp_err_t err = camera_photo_request(photo_publish_result, ctx);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "self.camera.take_photo 已进入异步 photo_task");
        return;
    }

    const char *error_name = !camera_photo_is_initialized()
                                 ? "PHOTO_NOT_READY"
                                 : "PHOTO_BUSY";
    snprintf(result.text, sizeof(result.text),
             "{\"error\":\"%s\"}", error_name);
    ESP_LOGW(TAG, "拒绝 self.camera.take_photo：%s", error_name);
    publish_tool_result((cJSON *)request_id, &result);
    memset(ctx, 0, sizeof(*ctx));
    heap_caps_free(ctx);
}

/* ---------------- 内置工具：运动与灯光 ----------------
 * 这两个工具的执行体在 stepper_motor / ambient_led 组件里，
 * 由本模块在 init 时自注册。新工具请用 mcp_registry_register 登记，
 * 不要往 dispatch 里硬编码。 */

#if BOARD_HAS_STEPPER
typedef struct {
    uint32_t steps;
    uint32_t interval_us;
    stepper_direction_t direction;
    cJSON *request_id; /* 独立副本，由工作任务在最终回执后释放。 */
} motion_job_t;

static bool s_motion_busy;

static void motion_worker(void *arg)
{
    const motion_job_t job = *(motion_job_t *)arg;
    heap_caps_free(arg);
    ESP_LOGI(TAG, "motion.move_steps 开始：steps=%u direction=%s interval_us=%u",
             (unsigned)job.steps, job.direction == STEPPER_DIR_CW ? "cw" : "ccw",
             (unsigned)job.interval_us);
    /* 电机只在收到工具调用时初始化和上电，动作结束后关闭线圈。 */
    esp_err_t err = stepper_motor_init();
    if (err == ESP_OK) {
        err = stepper_motor_enable();
    }
    if (err == ESP_OK) {
        err = stepper_motor_move_steps(job.steps, job.direction, job.interval_us);
    }
    const esp_err_t disable_err = stepper_motor_disable();
    if (err == ESP_OK) {
        err = disable_err;
    }
    mcp_tool_result_t reply = { .is_error = err != ESP_OK };
    /* 这里只确认驱动执行结果；没有编码器，不能声称已验证机械位移。 */
    snprintf(reply.text, sizeof(reply.text),
             "{\"completed\":%s,\"requestedSteps\":%u,\"direction\":\"%s\",\"result\":\"%s\"}",
             err == ESP_OK ? "true" : "false", (unsigned)job.steps,
             job.direction == STEPPER_DIR_CW ? "cw" : "ccw", esp_err_to_name(err));
    /* 线圈已关闭，先结束忙状态，再回执，避免后续指令被误报 MOTOR_BUSY。 */
    __atomic_store_n(&s_motion_busy, false, __ATOMIC_RELEASE);
    publish_tool_result(job.request_id, &reply);
    cJSON_Delete(job.request_id);
    ESP_LOGI(TAG, "motion.move_steps 完成：steps=%u direction=%s result=%s",
             (unsigned)job.steps,
             job.direction == STEPPER_DIR_CW ? "cw" : "ccw",
             esp_err_to_name(err));
    vTaskDelete(NULL);
}

static void tool_motion_move_steps(const cJSON *args, const cJSON *request_id)
{
    mcp_tool_result_t reply = { .is_error = true };
    char *result = reply.text;
    const size_t result_size = sizeof(reply.text);
    const cJSON *steps = cJSON_GetObjectItem(args, "steps");
    const cJSON *direction = cJSON_GetObjectItem(args, "direction");
    const cJSON *interval = cJSON_GetObjectItem(args, "stepIntervalUs");
    if (!cJSON_IsNumber(steps) || steps->valuedouble < 1 ||
        steps->valuedouble > 200 || steps->valuedouble != steps->valueint ||
        !cJSON_IsString(direction) || direction->valuestring == NULL ||
        (strcmp(direction->valuestring, "cw") != 0 &&
         strcmp(direction->valuestring, "ccw") != 0) ||
        (interval != NULL && (!cJSON_IsNumber(interval) ||
          interval->valuedouble < 10000 || interval->valuedouble > 100000 ||
          interval->valuedouble != interval->valueint))) {
        snprintf(result, result_size, "{\"error\":\"INVALID_MOTION_ARGUMENTS\"}");
        publish_tool_result((cJSON *)request_id, &reply);
        return;
    }
    bool expected = false;
    if (!__atomic_compare_exchange_n(&s_motion_busy, &expected, true, false,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        snprintf(result, result_size, "{\"error\":\"MOTOR_BUSY\"}");
        publish_tool_result((cJSON *)request_id, &reply);
        return;
    }
    /* 电机任务参数由 CPU 读取，不交给 DMA；失败走现有错误返回。 */
    motion_job_t *job = heap_caps_malloc(sizeof(*job), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (job == NULL) {
        __atomic_store_n(&s_motion_busy, false, __ATOMIC_RELEASE);
        snprintf(result, result_size, "{\"error\":\"NO_MEMORY\"}");
        publish_tool_result((cJSON *)request_id, &reply);
        return;
    }
    cJSON *id_copy = request_id != NULL ? cJSON_Duplicate(request_id, true) : NULL;
    if (id_copy == NULL) {
        heap_caps_free(job);
        __atomic_store_n(&s_motion_busy, false, __ATOMIC_RELEASE);
        snprintf(result, result_size, "{\"error\":\"REQUEST_ID_COPY_FAILED\"}");
        publish_tool_result((cJSON *)request_id, &reply);
        return;
    }
    *job = (motion_job_t){
        .request_id = id_copy,
        .steps = (uint32_t)steps->valueint,
        .direction = strcmp(direction->valuestring, "cw") == 0
                         ? STEPPER_DIR_CW : STEPPER_DIR_CCW,
        /* 使用实测范围10～100ms，默认20ms；不自动提高到未验证的步速。 */
        .interval_us = interval == NULL ? 20000U : (uint32_t)interval->valueint,
    };
    if (xTaskCreatePinnedToCore(motion_worker, "mcp_stepper", 3072, job,
                                4, NULL, 1) != pdPASS) {
        cJSON_Delete(id_copy);
        heap_caps_free(job);
        __atomic_store_n(&s_motion_busy, false, __ATOMIC_RELEASE);
        snprintf(result, result_size, "{\"error\":\"TASK_CREATE_FAILED\"}");
        publish_tool_result((cJSON *)request_id, &reply);
        return;
    }
    /* 不提前回 accepted，最终成功/失败由 motion_worker 统一回执。
     * MCP 分发任务仍可继续接收 motion.stop。 */
}

#endif

static void tool_motion_stop(const cJSON *args, char *result,
                             size_t result_size, bool *is_error)
{
    (void)args;
    stepper_motor_stop();
    snprintf(result, result_size,
             "{\"stopped\":true,\"fault\":%s}",
             stepper_motor_is_fault() ? "true" : "false");
    *is_error = false;
}

static void tool_motion_get_state(const cJSON *args, char *result,
                                  size_t result_size, bool *is_error)
{
    (void)args;
    /* 如实回报：驱动只提供"是否故障"，没有编码器也没有限位开关，
     * 因此**不能**回报绝对角度。不要为了字段好看而编一个位置值。 */
    snprintf(result, result_size,
             "{\"fault\":%s,\"moving\":%s,\"positionAvailable\":false,"
             "\"note\":\"no encoder or limit switch; absolute angle unavailable\"}",
             stepper_motor_is_fault() ? "true" : "false",
#if BOARD_HAS_STEPPER
             __atomic_load_n(&s_motion_busy, __ATOMIC_ACQUIRE) ? "true" : "false");
#else
             "false");
#endif
    *is_error = false;
}

static void tool_light_pulse(const cJSON *args, char *result,
                             size_t result_size, bool *is_error)
{
    /* MCP 线程只做校验和投递，真正的 RMT 刷新由 ambient_led 任务完成。 */
    const cJSON *enabled = cJSON_GetObjectItem(args, "enabled");
    if (cJSON_IsBool(enabled) && !cJSON_IsTrue(enabled)) {
        const esp_err_t err = ambient_led_off();
        *is_error = err != ESP_OK;
        snprintf(result, result_size, err == ESP_OK
                    ? "{\"enabled\":false}"
                    : "{\"error\":\"NOT_AVAILABLE\",\"detail\":\"%s\"}",
                 esp_err_to_name(err));
        return;
    }

    ambient_led_target_t target = AMBIENT_LED_TARGET_BOTH;
    const cJSON *target_json = cJSON_GetObjectItem(args, "target");
    if (cJSON_IsString(target_json) && target_json->valuestring != NULL) {
        if (strcasecmp(target_json->valuestring, "a") == 0) {
            target = AMBIENT_LED_TARGET_A;
        } else if (strcasecmp(target_json->valuestring, "b") == 0) {
            target = AMBIENT_LED_TARGET_B;
        } else if (strcasecmp(target_json->valuestring, "both") != 0) {
            *is_error = true;
            snprintf(result, result_size,
                     "{\"error\":\"invalid_argument\",\"detail\":\"target must be a, b, or both\"}");
            return;
        }
    }

    unsigned rr = 255, gg = 255, bb = 255;
    const cJSON *color = cJSON_GetObjectItem(args, "color");
    if (cJSON_IsString(color) && color->valuestring != NULL &&
        color->valuestring[0] == '#') {
        if (sscanf(color->valuestring, "#%2x%2x%2x", &rr, &gg, &bb) != 3) {
            *is_error = true;
            snprintf(result, result_size,
                     "{\"error\":\"invalid_argument\",\"detail\":\"color must be #RRGGBB\"}");
            return;
        }
    }
    const cJSON *r = cJSON_GetObjectItem(args, "r");
    const cJSON *g = cJSON_GetObjectItem(args, "g");
    const cJSON *b = cJSON_GetObjectItem(args, "b");
    if (cJSON_IsNumber(r) || cJSON_IsNumber(g) || cJSON_IsNumber(b)) {
        if (!cJSON_IsNumber(r) || !cJSON_IsNumber(g) || !cJSON_IsNumber(b) ||
            r->valueint < 0 || r->valueint > 255 ||
            g->valueint < 0 || g->valueint > 255 ||
            b->valueint < 0 || b->valueint > 255) {
            *is_error = true;
            snprintf(result, result_size,
                     "{\"error\":\"invalid_argument\",\"detail\":\"r/g/b must be 0..255\"}");
            return;
        }
        rr = (unsigned)r->valueint;
        gg = (unsigned)g->valueint;
        bb = (unsigned)b->valueint;
    }

    unsigned level = 255;
    const cJSON *brightness = cJSON_GetObjectItem(args, "brightness");
    if (cJSON_IsNumber(brightness)) {
        if (brightness->valueint < 0 || brightness->valueint > 255) {
            *is_error = true;
            snprintf(result, result_size,
                     "{\"error\":\"invalid_argument\",\"detail\":\"brightness must be 0..255\"}");
            return;
        }
        level = (unsigned)brightness->valueint;
    }
    unsigned duration = 1000;
    const cJSON *duration_json = cJSON_GetObjectItem(args, "duration_ms");
    if (cJSON_IsNumber(duration_json)) {
        if (duration_json->valueint < 0 || duration_json->valueint > 3600000) {
            *is_error = true;
            snprintf(result, result_size,
                     "{\"error\":\"invalid_argument\",\"detail\":\"duration_ms must be 0..3600000\"}");
            return;
        }
        duration = (unsigned)duration_json->valueint;
    }

    esp_err_t err = ambient_led_pulse(target, (uint8_t)rr, (uint8_t)gg,
                                      (uint8_t)bb, (uint8_t)level, duration);

    if (err != ESP_OK) {
        /* 灯带尚未初始化时，如实返回不可用，避免服务端误判为参数错误。 */
        *is_error = true;
        snprintf(result, result_size,
                 "{\"error\":\"NOT_AVAILABLE\",\"detail\":\"ambient LED not initialized; %s\"}",
                 esp_err_to_name(err));
        return;
    }

    snprintf(result, result_size,
             "{\"enabled\":true,\"target\":\"%s\",\"r\":%u,\"g\":%u,\"b\":%u,\"brightness\":%u,\"duration_ms\":%u,\"effect\":\"BREATH\"}",
             target == AMBIENT_LED_TARGET_A ? "a" :
             target == AMBIENT_LED_TARGET_B ? "b" : "both",
             rr, gg, bb, level, duration);
    *is_error = false;
}

/* 内置工具在 init 时自注册；同步工具与异步工具都由同一注册表发布。
 * 只有 handler/async_handler 都为空的能力（当前为 motion.rotate_to）
 * 才作为未实现占位，不进入 tools/list 和 capability_manifest。 */
static bool copy_json_string(const cJSON *object, const char *key,
                             char *output, size_t capacity)
{
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsString(value) || value->valuestring == NULL) return false;
    const size_t length = strlen(value->valuestring);
    if (length == 0 || length >= capacity) return false;
    memcpy(output, value->valuestring, length + 1);
    return true;
}

static bool valid_session_id(const char *id)
{
    for (const unsigned char *p = (const unsigned char *)id; *p != 0; ++p) {
        if (!((*p >= '0' && *p <= '9') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= 'a' && *p <= 'z') || *p == '-' || *p == '_')) return false;
    }
    return id[0] != '\0';
}

/* 仅校验并入队。HTTPS/ICE/DTLS 与编解码启动均由 WHIP 控制任务完成。 */
static void tool_webrtc_start(const cJSON *args, char *result,
                              size_t result_size, bool *is_error)
{
    webrtc_whip_credential_t *credential = heap_caps_calloc(
        1, sizeof(*credential), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (credential == NULL) {
        snprintf(result, result_size, "{\"error\":\"NO_MEMORY\"}");
        *is_error = true;
        return;
    }
    const cJSON *video = cJSON_GetObjectItemCaseSensitive(args, "video");
    const cJSON *audio = cJSON_GetObjectItemCaseSensitive(args, "audio");
    const cJSON *width = cJSON_GetObjectItemCaseSensitive(video, "width");
    const cJSON *height = cJSON_GetObjectItemCaseSensitive(video, "height");
    const cJSON *fps = cJSON_GetObjectItemCaseSensitive(video, "fps");
    const cJSON *codec = cJSON_GetObjectItemCaseSensitive(video, "codec");
    const cJSON *mode = cJSON_GetObjectItemCaseSensitive(args, "mode");
    /* 分开校验控制字段与视频规格。此前两类错误统一返回
     * UNSUPPORTED_VIDEO_MODE，无法判断是服务端参数不匹配还是一次性凭据字段异常。 */
    const bool path_ok = copy_json_string(args, "credentialPath",
                                          credential->credential_path,
                                          sizeof(credential->credential_path)) &&
                         credential->credential_path[0] == '/' &&
                         strstr(credential->credential_path, "://") == NULL &&
                         strstr(credential->credential_path, "..") == NULL;
    const bool ticket_ok = copy_json_string(args, "deviceTicket",
                                            credential->device_ticket,
                                            sizeof(credential->device_ticket));
    const bool session_ok = copy_json_string(args, "sessionId",
                                             credential->session_id,
                                             sizeof(credential->session_id)) &&
                            valid_session_id(credential->session_id);
    /* 新版三通道协议规定 VIEW 与 TALK 都使用同一条 WHIP 单向视频链路。
     * TALK 的手机语音仍走既有 UDP 下行，本阶段不向 WebRTC 加入音轨。 */
    const bool mode_ok = cJSON_IsString(mode) && mode->valuestring != NULL &&
                         (strcasecmp(mode->valuestring, "VIEW") == 0 ||
                          strcasecmp(mode->valuestring, "TALK") == 0);
    if (!path_ok || !ticket_ok || !session_ok || !mode_ok) {
        /* 只打印字段状态和非敏感 mode；严禁打印 ticket、完整路径或会话凭据。 */
        ESP_LOGW(TAG,
                 "拒绝 media.webrtc.start：控制字段无效 "
                 "credentialPath=%s deviceTicket=%s sessionId=%s mode=%s",
                 path_ok ? "OK" : "缺失/非法/过长",
                 ticket_ok ? "OK" : "缺失/过长",
                 session_ok ? "OK" : "缺失/非法/过长",
                 cJSON_IsString(mode) && mode->valuestring != NULL
                     ? mode->valuestring : "缺失/非字符串");
        snprintf(result, result_size, "{\"error\":\"INVALID_ARGUMENTS\"}");
        *is_error = true;
        memset(credential, 0, sizeof(*credential));
        heap_caps_free(credential);
        return;
    }

    const bool video_object_ok = cJSON_IsObject(video);
    const bool dimensions_ok = video_object_ok && cJSON_IsNumber(width) &&
                               cJSON_IsNumber(height) && cJSON_IsNumber(fps);
    const bool codec_ok = codec == NULL ||
                          (cJSON_IsString(codec) && codec->valuestring != NULL &&
                           strcasecmp(codec->valuestring, "H264") == 0);
    const bool video_mode_ok = dimensions_ok && codec_ok &&
                               width->valueint == (CONFIG_RTC_MEM_DIAG_640P ? 1280 : VIDEO_STREAM_WIDTH) &&
                               height->valueint == (CONFIG_RTC_MEM_DIAG_640P ? 720 : VIDEO_STREAM_HEIGHT) &&
                               fps->valueint == 20;
    if (!video_mode_ok) {
        /* 视频字段不是凭据，不含密钥，可安全输出用于服务端联调。 */
        ESP_LOGW(TAG,
                 "拒绝 media.webrtc.start：视频规格不匹配，"
                 "received=%dx%d@%d codec=%s；supported=%ux%u@20 H264",
                 cJSON_IsNumber(width) ? width->valueint : -1,
                 cJSON_IsNumber(height) ? height->valueint : -1,
                 cJSON_IsNumber(fps) ? fps->valueint : -1,
                 cJSON_IsString(codec) && codec->valuestring != NULL
                     ? codec->valuestring : (codec == NULL ? "缺失(允许)" : "非字符串"),
                 VIDEO_STREAM_WIDTH, VIDEO_STREAM_HEIGHT);
        /* 服务端下发的尺寸必须与当前编译的 H.264 编码器一致，避免无转码时黑屏。 */
        snprintf(result, result_size,
                 "{\"error\":\"UNSUPPORTED_VIDEO_MODE\",\"supported\":\"%ux%u@20 H264\"}",
                 VIDEO_STREAM_WIDTH, VIDEO_STREAM_HEIGHT);
        *is_error = true;
        memset(credential, 0, sizeof(*credential));
        heap_caps_free(credential);
        return;
    }
    /* A/B 诊断档接受云端既有 720P 命令，凭据保持云端规格；
     * 实际 Peer/编码器在 RTC 层改用 640P，以便同一入口比较内存。 */
    credential->width = CONFIG_RTC_MEM_DIAG_640P ? 1280 : VIDEO_STREAM_WIDTH;
    credential->height = CONFIG_RTC_MEM_DIAG_640P ? 720 : VIDEO_STREAM_HEIGHT;
    credential->fps = 20;
    credential->video = true;
    credential->audio = cJSON_IsTrue(audio) || cJSON_IsObject(audio);
#if CONFIG_RTC_MEM_DIAG_640P
    ESP_LOGI(TAG, "RTC_AB profile=B cloud_request=1280x720@20 local_encoder=640x480@15 bitrate=900kbps");
#endif
    const esp_err_t err = webrtc_whip_request_start(credential);
    if (err == ESP_OK) {
        snprintf(result, result_size,
                 "{\"accepted\":true,\"sessionId\":\"%s\",\"video\":\"%ux%u@%u\",\"audio\":false}",
                 credential->session_id, VIDEO_STREAM_WIDTH, VIDEO_STREAM_HEIGHT,
                 CONFIG_RTC_MEM_DIAG_640P ? 15U : 20U);
        *is_error = false;
    } else {
        snprintf(result, result_size, "{\"error\":\"%s\"}",
                 err == ESP_ERR_INVALID_STATE ? "DEVICE_BUSY" : "START_REJECTED");
        *is_error = true;
    }
    memset(credential, 0, sizeof(*credential));
    heap_caps_free(credential);
}

static void tool_webrtc_stop(const cJSON *args, char *result,
                             size_t result_size, bool *is_error)
{
    char session_id[WEBRTC_WHIP_SESSION_MAX] = {0};
    if (!copy_json_string(args, "sessionId", session_id, sizeof(session_id)) ||
        !valid_session_id(session_id)) {
        snprintf(result, result_size, "{\"error\":\"sessionId required\"}");
        *is_error = true;
        return;
    }
    const esp_err_t err = webrtc_whip_request_stop(session_id);
    if (err == ESP_OK) {
        snprintf(result, result_size,
                 "{\"accepted\":true,\"sessionId\":\"%s\"}", session_id);
    } else {
        snprintf(result, result_size, "{\"error\":\"STOP_REJECTED\"}");
    }
    *is_error = err != ESP_OK;
}

static void register_builtin_tools(void)
{
    static const mcp_tool_t tools[] = {
#if BOARD_HAS_STEPPER
        { .name = "motion.move_steps",
          .description = "Move the stepper motor by a relative number of steps. "
                         "Clockwise or counterclockwise; no absolute position sensor. "
                         "Use motion.stop to interrupt movement. Default step interval is 20000 us. "
                         "Returns the driver result after movement and coil shutdown; "
                         "physical position is not measured.",
          .input_schema =
              "{\"type\":\"object\",\"additionalProperties\":false,"
              "\"required\":[\"steps\",\"direction\"],\"properties\":{"
              "\"steps\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":200},"
              "\"direction\":{\"type\":\"string\",\"enum\":[\"cw\",\"ccw\"]},"
              "\"stepIntervalUs\":{\"type\":\"integer\",\"minimum\":10000,\"maximum\":100000,\"default\":20000}}}",
          .async_handler = tool_motion_move_steps },
#endif
        { .name = "motion.stop",
          .description = "Immediately stop the stepper motor.",
          .input_schema = NULL,
          .handler = tool_motion_stop },
        { .name = "motion.get_state",
          .description = "Report motor state. Absolute angle is NOT available: "
                         "the driver has no encoder or limit switch.",
          .input_schema = NULL,
          .handler = tool_motion_get_state },
        { .name = "light.pulse",
          .description = "Set RGB ambient LED breathing for A, B, or both. "
                         "The temporary effect restores the current state/emotion.",
          .input_schema =
              "{\"type\":\"object\",\"properties\":{"
              "\"enabled\":{\"type\":\"boolean\"},"
              "\"target\":{\"type\":\"string\",\"enum\":[\"a\",\"b\",\"both\"]},"
              "\"color\":{\"type\":\"string\",\"description\":\"#RRGGBB\"},"
              "\"r\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":255},"
              "\"g\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":255},"
              "\"b\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":255},"
              "\"brightness\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":255},"
              "\"duration_ms\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":3600000}}}",
          .handler = tool_light_pulse },
        /* ---- 以下为已声明未实现的占位（handler = NULL）---- */
        { .name = "motion.rotate_to",
          .description = "Rotate to an absolute angle. Requires encoder/limit "
                         "switch hardware; not implemented yet.",
          .input_schema = NULL,
          .handler = NULL },
        { .name = "self.camera.take_photo",
          .description = "Capture the next complete 1280x720 MJPEG frame. "
                         "The current validation build stores it on the SD card.",
          .input_schema =
              "{\"type\":\"object\",\"additionalProperties\":false}",
          .handler = NULL,
          .async_handler = tool_camera_take_photo_async },
        { .name = "media.webrtc.start",
          .description = "Start a video-only LiveKit WHIP stream at 1280x720 20 fps for VIEW or TALK mode.",
          .input_schema = "{\"type\":\"object\",\"required\":[\"sessionId\",\"mode\",\"credentialPath\",\"deviceTicket\",\"video\"],\"properties\":{\"sessionId\":{\"type\":\"string\"},\"mode\":{\"type\":\"string\",\"enum\":[\"VIEW\",\"TALK\"]},\"credentialPath\":{\"type\":\"string\"},\"deviceTicket\":{\"type\":\"string\"},\"video\":{\"type\":\"object\"},\"audio\":{\"type\":[\"object\",\"boolean\"]}}}",
          .handler = tool_webrtc_start },
        { .name = "media.webrtc.stop",
          .description = "Stop the current LiveKit WHIP stream.",
          .input_schema = "{\"type\":\"object\",\"required\":[\"sessionId\"],\"properties\":{\"sessionId\":{\"type\":\"string\"}}}",
          .handler = tool_webrtc_stop },
    };
    for (size_t i = 0; i < sizeof(tools) / sizeof(tools[0]); i++) {
        (void)mcp_registry_register(&tools[i]);
    }
}

/* ---------------- JSON-RPC 方法分发 ---------------- */

static void handle_initialize(cJSON *id, const cJSON *params)
{
    cJSON *result = cJSON_CreateObject();
    if (result == NULL) {
        return;
    }

    /* 回应客户端请求的协议版本；没给就用 MCP 现行日期版本号。 */
    const cJSON *version = cJSON_GetObjectItem(params, "protocolVersion");
    cJSON_AddStringToObject(result, "protocolVersion",
                            (cJSON_IsString(version) && version->valuestring != NULL)
                                ? version->valuestring
                                : "2024-11-05");

    cJSON *capabilities = cJSON_CreateObject();
    cJSON *tools = cJSON_CreateObject();
    if (capabilities != NULL && tools != NULL) {
        cJSON_AddBoolToObject(tools, "listChanged", false);
        cJSON_AddItemToObject(capabilities, "tools", tools);
        cJSON_AddItemToObject(result, "capabilities", capabilities);
    } else {
        cJSON_Delete(capabilities);
        cJSON_Delete(tools);
    }

    cJSON *server_info = cJSON_CreateObject();
    if (server_info != NULL) {
        cJSON_AddStringToObject(server_info, "name", "lummiss-desktop-pet");
        cJSON_AddStringToObject(server_info, "version", "1.0.0");
        cJSON_AddItemToObject(result, "serverInfo", server_info);
    }
    publish_reply(id, result);
}

static void handle_tools_list(cJSON *id)
{
    cJSON *result = cJSON_CreateObject();
    cJSON *tools = cJSON_CreateArray();
    if (result == NULL || tools == NULL) {
        cJSON_Delete(result);
        cJSON_Delete(tools);
        return;
    }

    for (size_t i = 0; i < mcp_registry_count(); i++) {
        const mcp_tool_t *entry = mcp_registry_get(i);
        /* 未实现的占位不进 tools/list（不对服务端宣称 READY） */
        if (entry == NULL ||
            (entry->handler == NULL && entry->async_handler == NULL)) {
            continue;
        }
        cJSON *tool = cJSON_CreateObject();
        cJSON *schema = cJSON_Parse(entry->input_schema != NULL
                                        ? entry->input_schema
                                        : "{\"type\":\"object\"}");
        if (tool == NULL) {
            cJSON_Delete(schema);
            continue;
        }
        cJSON_AddStringToObject(tool, "name", entry->name);
        cJSON_AddStringToObject(tool, "description", entry->description);
        if (schema != NULL) {
            cJSON_AddItemToObject(tool, "inputSchema", schema);
        }
        cJSON_AddItemToArray(tools, tool);
    }

    cJSON_AddItemToObject(result, "tools", tools);
    publish_reply(id, result);
}

static void handle_tools_call(cJSON *id, const cJSON *params)
{
    const cJSON *name = cJSON_GetObjectItem(params, "name");
    if (!cJSON_IsString(name) || name->valuestring == NULL) {
        publish_error(id, -32602, "tools/call requires params.name");
        return;
    }
    const cJSON *args = cJSON_GetObjectItem(params, "arguments");
    if (!cJSON_IsObject(args)) {
        args = NULL;
    }

    const mcp_tool_t *tool = NULL;
    for (size_t i = 0; i < mcp_registry_count(); i++) {
        const mcp_tool_t *entry = mcp_registry_get(i);
        if (entry != NULL && strcmp(entry->name, name->valuestring) == 0) {
            tool = entry;
            break;
        }
    }

    mcp_tool_result_t res = {0};
    if (tool == NULL) {
        /* 只公布过哪些工具存在；完全未知的名字回"未知工具" */
        char detail[96];
        snprintf(detail, sizeof(detail), "Unknown tool: %.64s",
                 name->valuestring);
        publish_error(id, -32602, detail);
        return;
    }
    if (tool->handler == NULL && tool->async_handler == NULL) {
        /* 已声明未实现的占位（如 media.webrtc.*）：明确回未实现，
         * 不假装受理。 */
        res.is_error = true;
        snprintf(res.text, sizeof(res.text),
                 "{\"error\":\"NOT_IMPLEMENTED\",\"tool\":\"%.64s\"}",
                 tool->name);
        publish_tool_result(id, &res);
        return;
    }

    if (tool->async_handler != NULL) {
        /* 异步处理器必须复制 id/arguments，并在后台完成后自行回执。 */
        tool->async_handler(args, id);
        return;
    }

    tool->handler(args, res.text, sizeof(res.text), &res.is_error);
    ESP_LOGI(TAG, "tools/call %s -> %s", name->valuestring, res.text);
    publish_tool_result(id, &res);
}

static void dispatch_message(const char *json, size_t len)
{
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (root == NULL) {
        ESP_LOGW(TAG, "MCP 消息不是合法 JSON（%u 字节）", (unsigned)len);
        return;
    }

    const cJSON *payload = cJSON_GetObjectItem(root, "payload");
    if (!cJSON_IsObject(payload)) {
        ESP_LOGW(TAG, "MCP 消息缺少 payload 对象");
        cJSON_Delete(root);
        return;
    }

    const cJSON *method = cJSON_GetObjectItem(payload, "method");
    cJSON *id = cJSON_GetObjectItem(payload, "id");
    const cJSON *params = cJSON_GetObjectItem(payload, "params");

    if (!cJSON_IsString(method) || method->valuestring == NULL) {
        ESP_LOGW(TAG, "MCP 消息缺少 method");
        cJSON_Delete(root);
        return;
    }

    ESP_LOGI(TAG, "MCP <- %s（id=%s）", method->valuestring,
             id != NULL ? "有" : "无（通知）");

    if (strcmp(method->valuestring, "initialize") == 0) {
        handle_initialize(id, params);
    } else if (strcmp(method->valuestring, "notifications/initialized") == 0) {
        /* 通知：没有 id，按 JSON-RPC 不回复。握手到此完成。 */
        ESP_LOGI(TAG, "MCP 握手完成（notifications/initialized 已收到）");
    } else if (strcmp(method->valuestring, "tools/list") == 0) {
        handle_tools_list(id);
    } else if (strcmp(method->valuestring, "tools/call") == 0) {
        handle_tools_call(id, params);
    } else {
        publish_error(id, -32601, "Method not found");
    }

    cJSON_Delete(root);
}

/* ---------------- 队列与任务 ---------------- */

static void cloud_mcp_task(void *arg)
{
    (void)arg;
    mcp_message_t msg;
    for (;;) {
        if (xQueueReceive(s_queue, &msg, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        /* 工具执行放在这里，而不是 MQTT 回调里 —— 文档 §6.2 明令禁止
         * 在回调中控制电机或做耗时操作。 */
        dispatch_message(msg.text, strlen(msg.text));
    }
}

void cloud_mcp_submit(const char *json, size_t len)
{
    if (!s_initialized || json == NULL || len == 0) {
        return;
    }
    if (len >= CLOUD_MCP_MSG_MAX) {
        ESP_LOGW(TAG, "MCP 消息 %u 字节超过 %d 上限，已丢弃",
                 (unsigned)len, CLOUD_MCP_MSG_MAX);
        return;
    }
    mcp_message_t *msg = heap_caps_malloc(sizeof(*msg),
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (msg == NULL) {
        ESP_LOGW(TAG, "MCP 消息分配失败（%u 字节）", (unsigned)len);
        return;
    }
    memcpy(msg->text, json, len);
    msg->text[len] = '\0';
    /* 回调里绝不能阻塞：队列满就丢，宁可服务端回执超时也不要卡住 MQTT 任务。 */
    if (xQueueSend(s_queue, msg, 0) != pdTRUE) {
        ESP_LOGW(TAG, "MCP 队列已满，丢弃一条 %u 字节消息", (unsigned)len);
    }
    heap_caps_free(msg);
}

esp_err_t cloud_mcp_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }
    s_queue_storage = heap_caps_malloc(CLOUD_MCP_QUEUE_DEPTH * sizeof(mcp_message_t),
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_queue_storage != NULL) {
        s_queue = xQueueCreateStatic(CLOUD_MCP_QUEUE_DEPTH, sizeof(mcp_message_t),
                                     s_queue_storage, &s_queue_control);
    }
    if (s_queue == NULL) {
        heap_caps_free(s_queue_storage);
        s_queue_storage = NULL;
        ESP_LOGE(TAG, "创建 MCP 队列失败");
        return ESP_ERR_NO_MEM;
    }
    s_initialized = true;

    /* 内置工具自注册（运动 / 灯光）+ 未实现能力占位登记。
     * 其他组件（音量等）在 app_main 里调用 mcp_registry_register。
     * 注意：必须在 OTA 检查之前完成 —— capability_manifest 从这张表生成。 */
    register_builtin_tools();

    if (xTaskCreatePinnedToCoreWithCaps(cloud_mcp_task, "cloud_mcp",
                                        CLOUD_MCP_TASK_STACK, NULL,
                                        CLOUD_MCP_TASK_PRIO, NULL,
                                        CLOUD_MCP_TASK_CORE,
                                        MALLOC_CAP_SPIRAM |
                                            MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGE(TAG, "创建 MCP 任务失败");
        vQueueDelete(s_queue);
        s_queue = NULL;
        heap_caps_free(s_queue_storage);
        s_queue_storage = NULL;
        s_initialized = false;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG,
             "MCP 就绪：队列 %d 条 × %d 字节，任务栈 %d 优先级 %d 核 %d，"
             "登记 %u 个（READY %u 个）",
             CLOUD_MCP_QUEUE_DEPTH, CLOUD_MCP_MSG_MAX, CLOUD_MCP_TASK_STACK,
             CLOUD_MCP_TASK_PRIO, CLOUD_MCP_TASK_CORE,
             (unsigned)mcp_registry_count(),
             (unsigned)mcp_registry_ready_count());
    return ESP_OK;
}
