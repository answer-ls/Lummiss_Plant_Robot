#ifndef LUMMISS_MCP_REGISTRY_H
#define LUMMISS_MCP_REGISTRY_H

/* MCP 工具与能力清单的**唯一登记处**。
 *
 * tools/list、tools/call 分发、hello v3 的 capability_manifest、OTA 请求体里的
 * capability_manifest —— 全部从这张表生成，消除多处硬编码清单的漂移。
 *
 * handler == NULL 的条目是「已声明未实现」的占位（如 media.webrtc.start）：
 * 不进 tools/list、不进 capability_manifest（不对服务端宣称 READY），
 * 但 tools/call 到达时会回明确的 NOT_IMPLEMENTED 错误而不是"未知工具"。
 *
 * 注册必须在 OTA 检查与 cloud_mqtt_start 之前完成（两处都会读这张表）。 */

#include <stdbool.h>
#include <stddef.h>

#include "cJSON.h"
#include "esp_err.h"

/* 工具执行函数：结果 JSON 写进 result（受 result_size 限制）；
 * 失败时置 *is_error = true 并写 {"error":...}。
 * arguments 可能为 NULL（工具声明了无参数）。 */
typedef void (*mcp_tool_handler_t)(const cJSON *arguments,
                                   char *result, size_t result_size,
                                   bool *is_error);

typedef struct {
    const char *name;         /* 工具名 = 能力码，如 "motion.stop"（静态字符串） */
    const char *description;  /* LLM 可读描述（静态字符串） */
    const char *input_schema; /* JSON-Schema 文本；NULL = 无参数 */
    mcp_tool_handler_t handler; /* NULL = 已声明未实现 */
} mcp_tool_t;

/* 注册工具。name/description/input_schema 必须是持久字符串（字面量即可）。
 * 同名重复注册返回错误。 */
esp_err_t mcp_registry_register(const mcp_tool_t *tool);

/* 已注册工具总数（含未实现的占位）。 */
size_t mcp_registry_count(void);

/* 取第 index 个注册项；越界返回 NULL。 */
const mcp_tool_t *mcp_registry_get(size_t index);

/* 已实现（handler != NULL）的工具数。 */
size_t mcp_registry_ready_count(void);

/* 生成 capability_manifest JSON 对象文本（**只含已实现**的工具）：
 * {"manifestVersion":1,"variantCode":"DESKTOP_PET_V1","capabilities":[...]}
 * hello v3 与 OTA 请求体都以
 * "capability_manifest":<本函数输出> 的形式嵌入。 */
esp_err_t mcp_registry_build_capability_manifest(char *buf, size_t buf_size);

#endif
