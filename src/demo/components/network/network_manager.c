#include "network_manager.h"

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "nvs_flash.h"

#include "provisioning_manager.h"
#include "wifi_manager.h"

static const char *TAG = "NETWORK";

#define NETWORK_CONNECTED_BIT BIT0

static EventGroupHandle_t s_network_events;
static esp_netif_t *s_station_netif;
static volatile network_state_t s_network_state = NETWORK_STATE_STOPPED;
static bool s_initialized;
static bool s_started;
static bool s_provisioning_active;
static bool s_has_ip;

static void network_provisioning_event_callback(provisioning_event_t event,
                                                void *user_ctx)
{
    (void)user_ctx;

    switch (event) {
    case PROVISIONING_EVENT_STARTED:
        s_provisioning_active = true;
        s_network_state = NETWORK_STATE_PROVISIONING;
        xEventGroupClearBits(s_network_events, NETWORK_CONNECTED_BIT);
        ESP_LOGI(TAG, "等待 App 通过 BLE 下发 Wi-Fi 凭据");
        break;

    case PROVISIONING_EVENT_CREDENTIALS_RECEIVED:
        s_network_state = NETWORK_STATE_CONNECTING;
        ESP_LOGI(TAG, "正在验证 App 下发的 Wi-Fi 凭据");
        break;

    case PROVISIONING_EVENT_SUCCEEDED:
        /* 后续掉线由常驻 Wi-Fi Manager 自动重连。Connected bit 要等 BLE
         * 服务真正结束后再设置，确保视频链路不会与配网 BLE 同时启动。 */
        wifi_manager_set_auto_reconnect_enabled(true);
        s_network_state = NETWORK_STATE_CONNECTED;
        break;

    case PROVISIONING_EVENT_FAILED:
        s_network_state = NETWORK_STATE_PROVISIONING;
        break;

    case PROVISIONING_EVENT_STOPPED:
        s_provisioning_active = false;
        if (s_has_ip) {
            s_network_state = NETWORK_STATE_CONNECTED;
            xEventGroupSetBits(s_network_events, NETWORK_CONNECTED_BIT);
            ESP_LOGI(TAG, "BLE 已关闭，网络业务可以启动");
        } else {
            s_network_state = NETWORK_STATE_DISCONNECTED;
        }
        break;
    }
}

static void network_wifi_event_callback(wifi_manager_event_t event,
                                        const wifi_manager_event_data_t *data,
                                        void *user_ctx)
{
    (void)user_ctx;

    switch (event) {
    case WIFI_MANAGER_EVENT_CONNECTING:
        s_network_state = NETWORK_STATE_CONNECTING;
        ESP_LOGI(TAG, "正在通过 ESP32-C6 连接 WiFi");
        break;

    case WIFI_MANAGER_EVENT_GOT_IP:
        if (data == NULL) {
            break;
        }
        s_has_ip = true;
        s_network_state = NETWORK_STATE_CONNECTED;
        if (!s_provisioning_active) {
            xEventGroupSetBits(s_network_events, NETWORK_CONNECTED_BIT);
        }
        ESP_LOGI(TAG, "WiFi 联网成功");
        ESP_LOGI(TAG, "IPv4=" IPSTR "，网关=" IPSTR "，掩码=" IPSTR,
                 IP2STR(&data->ip), IP2STR(&data->gateway), IP2STR(&data->netmask));
        break;

    case WIFI_MANAGER_EVENT_DISCONNECTED:
        s_has_ip = false;
        s_network_state = s_provisioning_active
                              ? NETWORK_STATE_PROVISIONING
                              : NETWORK_STATE_DISCONNECTED;
        xEventGroupClearBits(s_network_events, NETWORK_CONNECTED_BIT);
        ESP_LOGW(TAG, "网络断开，C6 reason=%u，等待自动重连",
                 data != NULL ? data->disconnect_reason : 0);
        break;

    default:
        break;
    }
}

static esp_err_t initialize_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS 分区需要重建：%s", esp_err_to_name(err));
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "擦除损坏的 NVS 分区失败");
        err = nvs_flash_init();
    }
    return err;
}

esp_err_t network_manager_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    s_network_state = NETWORK_STATE_STARTING;

    /* ESP-Hosted 默认会输出大量内部初始化细节。保留 WARN/ERROR 和本组件的
     * 联网结果，正常启动时不再逐项打印 SDIO 队列、RPC 与 CLI 注册信息。 */
    esp_log_level_set("H_SDIO_DRV", ESP_LOG_WARN);
    esp_log_level_set("sdio_wrapper", ESP_LOG_WARN);
    esp_log_level_set("transport", ESP_LOG_WARN);
    esp_log_level_set("H_API", ESP_LOG_WARN);
    esp_log_level_set("RPC_WRAP", ESP_LOG_WARN);
    esp_log_level_set("esp_cli", ESP_LOG_WARN);
    esp_log_level_set("hci_stub_drv", ESP_LOG_WARN);

    s_network_events = xEventGroupCreate();
    if (s_network_events == NULL) {
        s_network_state = NETWORK_STATE_STOPPED;
        return ESP_ERR_NO_MEM;
    }

    ESP_RETURN_ON_ERROR(initialize_nvs(), TAG, "初始化 NVS 失败");
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "初始化 TCP/IP 协议栈失败");

    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "创建默认事件循环失败：%s", esp_err_to_name(err));
        return err;
    }

    s_station_netif = esp_netif_create_default_wifi_sta();
    if (s_station_netif == NULL) {
        ESP_LOGE(TAG, "创建 WiFi STA 网络接口失败");
        return ESP_ERR_NO_MEM;
    }

    ESP_RETURN_ON_ERROR(wifi_manager_init(network_wifi_event_callback, NULL),
                        TAG, "初始化 WiFi Manager 失败");

    s_initialized = true;
    ESP_LOGI(TAG, "Network Manager 初始化完成");
    return ESP_OK;
}

esp_err_t network_manager_start(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_started) {
        return ESP_OK;
    }

    wifi_manager_set_auto_reconnect_enabled(false);

    bool provisioning_started = false;
    esp_err_t err = provisioning_manager_start(
        network_provisioning_event_callback, NULL, &provisioning_started);
    if (err != ESP_OK) {
        s_network_state = NETWORK_STATE_STOPPED;
        ESP_LOGE(TAG, "启动 BLE 配网管理器失败：%s", esp_err_to_name(err));
        return err;
    }

    if (provisioning_started) {
        s_provisioning_active = true;
        s_network_state = NETWORK_STATE_PROVISIONING;
        s_started = true;
        return ESP_OK;
    }

    s_network_state = NETWORK_STATE_CONNECTING;
    err = wifi_manager_start_saved();
    if (err != ESP_OK) {
        s_network_state = NETWORK_STATE_STOPPED;
        ESP_LOGE(TAG, "使用已保存凭据启动 Wi-Fi 失败：%s",
                 esp_err_to_name(err));
        return err;
    }

    s_started = true;
    return ESP_OK;
}

network_state_t network_manager_get_state(void)
{
    return s_network_state;
}

bool network_manager_is_connected(void)
{
    return s_network_events != NULL &&
           (xEventGroupGetBits(s_network_events) & NETWORK_CONNECTED_BIT) != 0;
}

bool network_manager_is_provisioning(void)
{
    return s_provisioning_active;
}

bool network_manager_wait_connected(uint32_t timeout_ms)
{
    if (s_network_events == NULL) {
        return false;
    }

    const TickType_t wait_ticks = timeout_ms == UINT32_MAX
                                      ? portMAX_DELAY
                                      : pdMS_TO_TICKS(timeout_ms);
    EventBits_t bits = xEventGroupWaitBits(s_network_events,
                                           NETWORK_CONNECTED_BIT,
                                           pdFALSE,
                                           pdTRUE,
                                           wait_ticks);
    return (bits & NETWORK_CONNECTED_BIT) != 0;
}

esp_err_t network_manager_forget_wifi_credentials(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    wifi_manager_set_auto_reconnect_enabled(false);
    esp_err_t err = esp_wifi_disconnect();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_STARTED &&
        err != ESP_ERR_WIFI_NOT_CONNECT) {
        ESP_LOGW(TAG, "清除凭据前断开 Wi-Fi 失败：%s", esp_err_to_name(err));
    }

    ESP_RETURN_ON_ERROR(esp_wifi_restore(), TAG, "清除 C6 Wi-Fi 凭据失败");
    s_has_ip = false;
    s_network_state = NETWORK_STATE_STOPPED;
    xEventGroupClearBits(s_network_events, NETWORK_CONNECTED_BIT);
    ESP_LOGI(TAG, "C6 Wi-Fi 凭据已清除；重启后进入 BLE 配网");
    return ESP_OK;
}
