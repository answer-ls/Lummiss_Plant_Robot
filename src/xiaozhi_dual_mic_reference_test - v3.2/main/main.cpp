#include <cstring>
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "nvs_flash.h"
#include "bsp_board_extra.h"
#include "XiaozhiApp.hpp"

static void wifi_event(void *, esp_event_base_t base, int32_t id, void *)
{
    if (base == WIFI_EVENT && (id == WIFI_EVENT_STA_START || id == WIFI_EVENT_STA_DISCONNECTED)) {
        esp_wifi_connect();
    }
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) ESP_LOGI("REF_WIFI", "DHCP OK");
}

extern "C" void app_main(void)
{
    // 独立测试不擦除已有 NVS；配置错误时停止，避免破坏原设备信息。
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(board_audio_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, nullptr));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    wifi_config_t config = {};
    ESP_ERROR_CHECK(esp_wifi_get_config(WIFI_IF_STA, &config));
    // 测试网络由 menuconfig 配置；留空时沿用已经保存的 STA 配置。
    if (strlen(CONFIG_REFERENCE_WIFI_SSID)) {
        strlcpy((char *)config.sta.ssid, CONFIG_REFERENCE_WIFI_SSID, sizeof(config.sta.ssid));
        strlcpy((char *)config.sta.password, CONFIG_REFERENCE_WIFI_PASSWORD, sizeof(config.sta.password));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &config));
    }
    if (!config.sta.ssid[0]) {
        ESP_LOGE("REF_WIFI", "没有已保存的WiFi，请在menuconfig的Reference board中配置网络后编译");
        return;
    }
    esp_vfs_spiffs_conf_t storage = {};
    storage.base_path = "/spiffs";
    storage.partition_label = "storage";
    storage.max_files = 4;
    storage.format_if_mount_failed = false;
    ESP_ERROR_CHECK(esp_vfs_spiffs_register(&storage));
    // 先注册原版应用的联网事件，再启动 STA，避免漏掉首次 DHCP 通知。
    auto app = esp_brookesia::apps::XiaozhiApp::requestInstance();
    if (!app || !app->startHeadless()) {
        ESP_LOGE("REF", "参考应用启动失败");
        return;
    }
    ESP_ERROR_CHECK(esp_wifi_start());
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        ref_diag_print();
    }
}
