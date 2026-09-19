#ifndef LUMMISS_CLOUD_MCP_H
#define LUMMISS_CLOUD_MCP_H

/* 三通道新协议的 MCP 层（文档 §3.2）：JSON-RPC 2.0 包在 type=mcp 里，
 * 走 MQTT 的 publish_topic / subscribe_topic。
 *
 * 设计约束（文档 §6.2）：MQTT 回调里**只做长度检查 / JSON 解析 / 分类 / 入队**，
 * 不允许初始化摄像头、发 HTTP、建 PeerConnection、控制电机或做耗时操作。
 * 所以本模块是一个「队列 + 独立任务」：回调只 submit，工具在 mcp_task 里执行。
 *
 * 工具与能力清单的登记在 components/mcp_registry（tools/list、tools/call、
 * hello/OTA 的 capability_manifest 三方共用的唯一登记处）。
 * 本模块负责 MCP 协议：initialize / tools/list / tools/call 分发与回执。
 *
 * cloud_mcp_init() 必须在 OTA 检查之前调用：内置工具在 init 时自注册，
 * capability_manifest 从注册表生成。 */

#include <stddef.h>

#include "esp_err.h"

/* 创建队列与 mcp_task，并把内置工具注册进 mcp_registry。幂等。 */
esp_err_t cloud_mcp_init(void);

/* 供 MQTT 回调调用：把一条 type=mcp 的原始 JSON **拷贝**进队列后立即返回。
 * 队列满时丢弃并告警（会表现为服务端等不到回执），不阻塞回调。 */
void cloud_mcp_submit(const char *json, size_t len);

#endif
