#include "device_identity.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_system.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "DEV_ID";
static const char *NVS_NS = "lummiss";
static const char *NVS_KEY_CLIENT = "client_id";

static device_identity_t s_identity;

esp_err_t device_identity_init(void)
{
    memset(&s_identity, 0, sizeof(s_identity));

    uint8_t mac[6];
    esp_err_t err = esp_efuse_mac_get_default(mac);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "读取默认 MAC 地址失败，使用全零");
        memset(mac, 0, sizeof(mac));
    }
    snprintf(s_identity.device_id, sizeof(s_identity.device_id),
             "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    err = nvs_flash_init();
    if (err != ESP_OK && err != ESP_ERR_NVS_NO_FREE_PAGES) {
        ESP_LOGW(TAG, "NVS init warning: %s", esp_err_to_name(err));
    }

    nvs_handle_t handle;
    err = nvs_open(NVS_NS, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "打开 NVS 失败：%s", esp_err_to_name(err));
        return err;
    }

    size_t len = sizeof(s_identity.client_id);
    err = nvs_get_str(handle, NVS_KEY_CLIENT,
                      s_identity.client_id, &len);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "从 NVS 读取 Client-Id：%s", s_identity.client_id);
    } else {
        uint8_t uuid_bin[16];
        esp_fill_random(uuid_bin, sizeof(uuid_bin));
        uuid_bin[6] = (uint8_t)((uuid_bin[6] & 0x0f) | 0x40);
        uuid_bin[8] = (uint8_t)((uuid_bin[8] & 0x3f) | 0x80);
        snprintf(s_identity.client_id, sizeof(s_identity.client_id),
                 "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                 uuid_bin[0], uuid_bin[1], uuid_bin[2], uuid_bin[3],
                 uuid_bin[4], uuid_bin[5],
                 uuid_bin[6], uuid_bin[7],
                 uuid_bin[8], uuid_bin[9],
                 uuid_bin[10], uuid_bin[11], uuid_bin[12], uuid_bin[13],
                 uuid_bin[14], uuid_bin[15]);

        err = nvs_set_str(handle, NVS_KEY_CLIENT, s_identity.client_id);
        if (err == ESP_OK) {
            err = nvs_commit(handle);
        }
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "持久化 Client-Id 失败：%s", esp_err_to_name(err));
        } else {
            ESP_LOGI(TAG, "生成并持久化 Client-Id：%s", s_identity.client_id);
        }
    }

    nvs_close(handle);

    ESP_LOGI(TAG, "Device-Id：%s  Client-Id：%s",
             s_identity.device_id, s_identity.client_id);
    return ESP_OK;
}

const device_identity_t *device_identity_get(void)
{
    return &s_identity;
}