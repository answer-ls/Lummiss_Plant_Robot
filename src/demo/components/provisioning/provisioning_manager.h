#ifndef LUMMISS_PROVISIONING_MANAGER_H
#define LUMMISS_PROVISIONING_MANAGER_H

#include <stdbool.h>

#include "esp_err.h"

typedef enum {
    PROVISIONING_EVENT_STARTED = 0,
    PROVISIONING_EVENT_CREDENTIALS_RECEIVED,
    PROVISIONING_EVENT_SUCCEEDED,
    PROVISIONING_EVENT_FAILED,
    PROVISIONING_EVENT_STOPPED,
} provisioning_event_t;

typedef void (*provisioning_event_callback_t)(provisioning_event_t event,
                                               void *user_ctx);

/* Wi-Fi 和默认事件循环必须在调用前完成初始化。函数会检查 C6 中是否已有
 * Wi-Fi 凭据；没有凭据时启动 BLE 配网服务。 */
esp_err_t provisioning_manager_start(provisioning_event_callback_t callback,
                                     void *user_ctx,
                                     bool *provisioning_started);

bool provisioning_manager_is_active(void);
const char *provisioning_manager_service_name(void);
const char *provisioning_manager_username(void);
const char *provisioning_manager_pop(void);

#endif /* LUMMISS_PROVISIONING_MANAGER_H */
