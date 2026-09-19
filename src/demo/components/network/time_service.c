#include "time_service.h"

#include <time.h>

#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

static const char *TAG = "TIME_SVC";

/* 粘性事件位：置位后永不清除，任意时刻来问都能看到。 */
#define TIME_SYNCED_BIT BIT0

/* 墙钟下限：2023-11-14。SNTP 未同步时系统时间从 1970 起算，
 * 用它做兜底判断，避免"回调没送到就永远等下去"。 */
#define TIME_SERVICE_PLAUSIBLE_EPOCH 1700000000

static EventGroupHandle_t s_events;
static bool s_initialized;

static void on_sntp_synced(struct timeval *tv)
{
    (void)tv;
    if (s_events != NULL) {
        xEventGroupSetBits(s_events, TIME_SYNCED_BIT);
    }
}

static bool clock_is_plausible(void)
{
    return time(NULL) > (time_t)TIME_SERVICE_PLAUSIBLE_EPOCH;
}

esp_err_t time_service_start(void)
{
    if (s_initialized) {
        return ESP_OK;      /* 幂等：本工程只允许初始化一次 */
    }
    if (s_events == NULL) {
        s_events = xEventGroupCreate();
        if (s_events == NULL) {
            ESP_LOGE(TAG, "创建同步事件组失败");
            return ESP_ERR_NO_MEM;
        }
    }

    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(
        2, ESP_SNTP_SERVER_LIST("ntp.aliyun.com", "pool.ntp.org"));
    /* wait_for_sync=false：那会额外创建一个我们根本用不到的二值信号量，
     * 而它的 sync_wait 语义正是上面注释里那个坑。改用 sync_cb + 粘性事件位。 */
    config.wait_for_sync = false;
    config.sync_cb = on_sntp_synced;

    const esp_err_t err = esp_netif_sntp_init(&config);
    if (err == ESP_OK) {
        s_initialized = true;
        ESP_LOGI(TAG, "SNTP 已启动（全工程唯一实例）");
        return ESP_OK;
    }
    if (err == ESP_ERR_INVALID_STATE) {
        /* 别处已经初始化过。不重复初始化，靠墙钟兜底继续推进状态。 */
        ESP_LOGW(TAG, "SNTP 已被其他模块初始化，本服务不再初始化（仅读时间）");
        s_initialized = true;
        return ESP_OK;
    }
    ESP_LOGE(TAG, "启动 SNTP 失败：%s", esp_err_to_name(err));
    return err;
}

bool time_service_is_synced(void)
{
    if (s_events != NULL &&
        (xEventGroupGetBits(s_events) & TIME_SYNCED_BIT) != 0) {
        return true;
    }
    /* 兜底：回调可能早于本服务启动，也可能根本没送到（例如 SNTP 被别人
     * 初始化）。系统时间已经合理就算同步完成。 */
    return clock_is_plausible();
}

bool time_service_wait_synced(uint32_t timeout_ms)
{
    if (time_service_is_synced()) {
        return true;
    }
    if (s_events == NULL) {
        return false;
    }
    /* 粘性位 + 不清除：多个调用方反复调用都能拿到正确结果。 */
    const EventBits_t bits = xEventGroupWaitBits(s_events, TIME_SYNCED_BIT,
                                                 pdFALSE, pdTRUE,
                                                 pdMS_TO_TICKS(timeout_ms));
    return (bits & TIME_SYNCED_BIT) != 0;
}
