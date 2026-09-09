#ifndef LUMMISS_NETWORK_MANAGER_H
#define LUMMISS_NETWORK_MANAGER_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    NETWORK_STATE_STOPPED = 0,
    NETWORK_STATE_STARTING,
    NETWORK_STATE_CONNECTING,
    NETWORK_STATE_CONNECTED,
    NETWORK_STATE_DISCONNECTED,
} network_state_t;

/* 初始化 NVS、TCP/IP 协议栈、默认事件循环和 WiFi STA 网络接口。 */
esp_err_t network_manager_init(void);

/* 启动 ESP-Hosted/esp_wifi_remote 并异步连接代码中配置的路由器。 */
esp_err_t network_manager_start(void);

/* 给后续天气、WebSocket、OTA 等模块使用的统一网络状态接口。 */
network_state_t network_manager_get_state(void);
bool network_manager_is_connected(void);
bool network_manager_wait_connected(uint32_t timeout_ms);

#endif /* LUMMISS_NETWORK_MANAGER_H */
