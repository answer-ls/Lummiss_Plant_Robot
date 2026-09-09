#include "wifi_manager.h"

#include <stdbool.h>
#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"

static const char *TAG = "WIFI_MANAGER";

#define WIFI_RECONNECT_DELAY_MS 2000

static wifi_manager_event_callback_t s_event_callback;
static void *s_event_user_ctx;
static esp_timer_handle_t s_reconnect_timer;
static bool s_initialized;
static bool s_started;

/* 统一向 Network Manager 上报状态，业务模块不直接订阅底层 WiFi 事件。 */
static void wifi_manager_notify(wifi_manager_event_t event,
                                const wifi_manager_event_data_t *data)
{
    if (s_event_callback != NULL) {
        s_event_callback(event, data, s_event_user_ctx);
    }
}

/* 重连在 esp_timer 任务中执行，避免在系统事件回调里阻塞或延时。 */
static void reconnect_timer_callback(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "开始重新连接 WiFi");
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "重新连接请求失败：%s", esp_err_to_name(err));
    }
}

static void schedule_reconnect(void)
{
    if (!s_started || s_reconnect_timer == NULL) {
        return;
    }

    /* 定时器可能已经停止或正在等待，先停止并忽略状态错误，再重新计时。 */
    esp_timer_stop(s_reconnect_timer);
    esp_err_t err = esp_timer_start_once(s_reconnect_timer,
                                         WIFI_RECONNECT_DELAY_MS * 1000ULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "安排 WiFi 重连失败：%s", esp_err_to_name(err));
    }
}

static void wifi_event_handler(void *arg,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data)
{
    (void)arg;
    (void)event_base;

    if (event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "WiFi STA 已启动，开始连接路由器");
        wifi_manager_notify(WIFI_MANAGER_EVENT_CONNECTING, NULL);
        esp_err_t err = esp_wifi_connect();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "连接请求失败：%s", esp_err_to_name(err));
            schedule_reconnect();
        }
        return;
    }

    if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *disconnected = event_data;
        wifi_manager_event_data_t data = {0};
        if (disconnected != NULL) {
            data.disconnect_reason = disconnected->reason;
        }
        ESP_LOGW(TAG, "WiFi 已断开，reason=%u，%u ms 后重连",
                 data.disconnect_reason, WIFI_RECONNECT_DELAY_MS);
        wifi_manager_notify(WIFI_MANAGER_EVENT_DISCONNECTED, &data);
        schedule_reconnect();
    }
}

static void ip_event_handler(void *arg,
                             esp_event_base_t event_base,
                             int32_t event_id,
                             void *event_data)
{
    (void)arg;
    (void)event_base;

    if (event_id != IP_EVENT_STA_GOT_IP || event_data == NULL) {
        return;
    }

    const ip_event_got_ip_t *got_ip = event_data;
    wifi_manager_event_data_t data = {
        .ip = got_ip->ip_info.ip,
        .netmask = got_ip->ip_info.netmask,
        .gateway = got_ip->ip_info.gw,
    };

    esp_timer_stop(s_reconnect_timer);
    wifi_manager_notify(WIFI_MANAGER_EVENT_GOT_IP, &data);
}

esp_err_t wifi_manager_init(wifi_manager_event_callback_t callback, void *user_ctx)
{
    if (s_initialized) {
        return ESP_OK;
    }
    if (callback == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    s_event_callback = callback;
    s_event_user_ctx = user_ctx;

    const esp_timer_create_args_t reconnect_timer_args = {
        .callback = reconnect_timer_callback,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "wifi_reconnect",
        .skip_unhandled_events = true,
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&reconnect_timer_args, &s_reconnect_timer),
                        TAG, "创建 WiFi 重连定时器失败");

    /* 在 ESP32-P4 上，该调用由 esp_wifi_remote 转发给板载 ESP32-C6。 */
    wifi_init_config_t wifi_init_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&wifi_init_config),
                        TAG, "初始化 esp_wifi_remote 失败");

    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT,
                                                   ESP_EVENT_ANY_ID,
                                                   wifi_event_handler,
                                                   NULL),
                        TAG, "注册 WiFi 事件处理器失败");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT,
                                                   IP_EVENT_STA_GOT_IP,
                                                   ip_event_handler,
                                                   NULL),
                        TAG, "注册 IP 事件处理器失败");

    s_initialized = true;
    ESP_LOGI(TAG, "esp_wifi_remote 初始化完成");
    return ESP_OK;
}

esp_err_t wifi_manager_start(const char *ssid, const char *password)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (ssid == NULL || password == NULL || ssid[0] == '\0' ||
        strlen(ssid) > 32 || strlen(password) > 64) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_started) {
        return ESP_OK;
    }

    wifi_config_t station_config = {0};
    memcpy(station_config.sta.ssid, ssid, strlen(ssid));
    memcpy(station_config.sta.password, password, strlen(password));
    station_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    station_config.sta.pmf_cfg.capable = true;
    station_config.sta.pmf_cfg.required = false;

    /* 凭据由当前固件固定配置，暂不写入远端 C6 的 NVS。 */
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM),
                        TAG, "设置 WiFi RAM 存储失败");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA),
                        TAG, "设置 WiFi STA 模式失败");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &station_config),
                        TAG, "设置固定 WiFi 凭据失败");

    s_started = true;
    esp_err_t err = esp_wifi_start();
    if (err != ESP_OK) {
        s_started = false;
        ESP_LOGE(TAG, "启动 WiFi 失败：%s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "WiFi STA 启动请求完成，SSID=%s", ssid);
    return ESP_OK;
}
