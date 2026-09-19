#include "mcp_registry.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"

static const char *TAG = "MCP_REG";

/* 容量：现有 7 个能力位 + WebRTC 2 个占位。满了 register 显式报错。 */
#define MCP_REGISTRY_MAX 12

static mcp_tool_t s_tools[MCP_REGISTRY_MAX];
static size_t s_count;

esp_err_t mcp_registry_register(const mcp_tool_t *tool)
{
    if (tool == NULL || tool->name == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < s_count; i++) {
        if (strcmp(s_tools[i].name, tool->name) == 0) {
            ESP_LOGE(TAG, "工具重复注册：%s", tool->name);
            return ESP_ERR_INVALID_STATE;
        }
    }
    if (s_count >= MCP_REGISTRY_MAX) {
        ESP_LOGE(TAG, "注册表已满（%d），无法注册 %s", MCP_REGISTRY_MAX,
                 tool->name);
        return ESP_ERR_NO_MEM;
    }
    s_tools[s_count++] = *tool;
    ESP_LOGI(TAG, "MCP %s：%s", tool->handler != NULL ? "工具" : "声明（未实现）",
             tool->name);
    return ESP_OK;
}

size_t mcp_registry_count(void)
{
    return s_count;
}

const mcp_tool_t *mcp_registry_get(size_t index)
{
    return index < s_count ? &s_tools[index] : NULL;
}

size_t mcp_registry_ready_count(void)
{
    size_t ready = 0;
    for (size_t i = 0; i < s_count; i++) {
        if (s_tools[i].handler != NULL) {
            ready++;
        }
    }
    return ready;
}

esp_err_t mcp_registry_build_capability_manifest(char *buf, size_t buf_size)
{
    if (buf == NULL || buf_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t off = 0;
    int w = snprintf(buf, buf_size,
                     "{\"manifestVersion\":1,\"variantCode\":\"DESKTOP_PET_V1\","
                     "\"capabilities\":[");
    if (w < 0 || (size_t)w >= buf_size) {
        return ESP_ERR_NO_MEM;
    }
    off = (size_t)w;

    bool first = true;
    for (size_t i = 0; i < s_count; i++) {
        if (s_tools[i].handler == NULL) {
            continue;   /* 未实现的工具不对服务端宣称（用户规格） */
        }
        w = snprintf(buf + off, buf_size - off, "%s\"%s\"",
                     first ? "" : ",", s_tools[i].name);
        if (w < 0 || (size_t)w >= buf_size - off) {
            return ESP_ERR_NO_MEM;
        }
        off += (size_t)w;
        first = false;
    }

    w = snprintf(buf + off, buf_size - off, "]}");
    if (w < 0 || (size_t)w >= buf_size - off) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
