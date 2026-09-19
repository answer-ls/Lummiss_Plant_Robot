#include "cloud_mcp.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "ambient_led.h"
#include "cloud_mqtt.h"
#include "mcp_registry.h"
#include "stepper_motor.h"

static const char *TAG = "CLOUD_MCP";

/* 队列深度与单条上限。
 * ponytail: 现在 MCP 请求都是几百字节，1 KiB × 4 条足够。等 WebRTC 的 SDP
 * 也走 MCP 时再按需上调（SDP 走的是 rtc_signal，不是 MCP，届时确认）。 */
#define CLOUD_MCP_QUEUE_DEPTH  4
#define CLOUD_MCP_MSG_MAX      1024

#define CLOUD_MCP_TASK_STACK   4096
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

/* ---------------- 内置工具：运动与灯光 ----------------
 * 这两个工具的执行体在 stepper_motor / ambient_led 组件里，
 * 由本模块在 init 时自注册。新工具请用 mcp_registry_register 登记，
 * 不要往 dispatch 里硬编码。 */

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
             "{\"fault\":%s,\"positionAvailable\":false,"
             "\"note\":\"no encoder or limit switch; absolute angle unavailable\"}",
             stepper_motor_is_fault() ? "true" : "false");
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

/* 内置工具在 init 时自注册；同时登记「已声明未实现」的占位能力
 * （motion.rotate_to / self.camera.take_photo / media.webrtc.*）——
 * 占位不进 tools/list、不进 capability_manifest（不对服务端宣称 READY），
 * 但 tools/call 误到达时能回明确的 NOT_IMPLEMENTED 而不是"未知工具"。 */
static void register_builtin_tools(void)
{
    static const mcp_tool_t tools[] = {
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
          .description = "Capture a JPEG photo and return it. Not implemented "
                         "yet.",
          .input_schema = NULL,
          .handler = NULL },
        { .name = "media.webrtc.start",
          .description = "Start a WebRTC video session (phase 4).",
          .input_schema = NULL,
          .handler = NULL },
        { .name = "media.webrtc.stop",
          .description = "Stop the WebRTC video session (phase 4).",
          .input_schema = NULL,
          .handler = NULL },
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
        if (entry == NULL || entry->handler == NULL) {
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
    if (tool->handler == NULL) {
        /* 已声明未实现的占位（如 media.webrtc.*）：明确回未实现，
         * 不假装受理。 */
        res.is_error = true;
        snprintf(res.text, sizeof(res.text),
                 "{\"error\":\"NOT_IMPLEMENTED\",\"tool\":\"%.64s\"}",
                 tool->name);
        publish_tool_result(id, &res);
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
    mcp_message_t msg;
    memcpy(msg.text, json, len);
    msg.text[len] = '\0';
    /* 回调里绝不能阻塞：队列满就丢，宁可服务端回执超时也不要卡住 MQTT 任务。 */
    if (xQueueSend(s_queue, &msg, 0) != pdTRUE) {
        ESP_LOGW(TAG, "MCP 队列已满，丢弃一条 %u 字节消息", (unsigned)len);
    }
}

esp_err_t cloud_mcp_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }
    s_queue = xQueueCreate(CLOUD_MCP_QUEUE_DEPTH, sizeof(mcp_message_t));
    if (s_queue == NULL) {
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
