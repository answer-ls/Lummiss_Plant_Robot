#ifndef LUMMISS_WIFI_MANAGER_H
#define LUMMISS_WIFI_MANAGER_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_netif_ip_addr.h"

typedef enum {
    WIFI_MANAGER_EVENT_CONNECTING = 0,
    WIFI_MANAGER_EVENT_GOT_IP,
    WIFI_MANAGER_EVENT_DISCONNECTED,
} wifi_manager_event_t;

typedef struct {
    esp_ip4_addr_t ip;
    esp_ip4_addr_t netmask;
    esp_ip4_addr_t gateway;
    uint8_t disconnect_reason;
} wifi_manager_event_data_t;

typedef void (*wifi_manager_event_callback_t)(wifi_manager_event_t event,
                                               const wifi_manager_event_data_t *data,
                                               void *user_ctx);

/* 初始化 esp_wifi_remote 及其事件处理器。调用前必须已经初始化 esp_netif
 * 并创建默认事件循环。 */
esp_err_t wifi_manager_init(wifi_manager_event_callback_t callback, void *user_ctx);

/* 使用 C6 Flash 中已经持久化的凭据启动 STA。 */
esp_err_t wifi_manager_start_saved(void);

/* 配网期间由官方 Network Provisioning 管理重试；成功后再打开常驻自动重连。 */
void wifi_manager_set_auto_reconnect_enabled(bool enabled);

#endif /* LUMMISS_WIFI_MANAGER_H */
