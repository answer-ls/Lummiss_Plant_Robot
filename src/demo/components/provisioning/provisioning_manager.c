#include "provisioning_manager.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_srp.h"
#include "device_identity.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_random.h"
#include "network_provisioning/manager.h"
#include "network_provisioning/scheme_ble.h"
#include "nvs.h"

static const char *TAG = "BLE_PROV";

#define PROVISIONING_NVS_NAMESPACE "lummiss"
#define PROVISIONING_NVS_POP_KEY   "prov_pop"
#define PROVISIONING_USERNAME      "lummiss"
#define PROVISIONING_SALT_LEN      16

static provisioning_event_callback_t s_callback;
static void *s_user_ctx;
static bool s_active;
static bool s_manager_initialized;
static char s_service_name[24];
static char s_pop[16];
static char *s_salt;
static char *s_verifier;
static network_prov_security2_params_t s_security2_params;

static void provisioning_notify(provisioning_event_t event)
{
    if (s_callback != NULL) {
        s_callback(event, s_user_ctx);
    }
}

static esp_err_t load_or_create_pop(void)
{
    nvs_handle_t handle;
    ESP_RETURN_ON_ERROR(nvs_open(PROVISIONING_NVS_NAMESPACE,
                                 NVS_READWRITE,
                                 &handle),
                        TAG, "打开配网 NVS 失败");

    size_t pop_len = sizeof(s_pop);
    esp_err_t err = nvs_get_str(handle, PROVISIONING_NVS_POP_KEY,
                                s_pop, &pop_len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        /* 当前开发板没有制造分区，首次启动生成每台设备独立的 8 位口令。
         * 正式量产时可改为读取制造分区或二维码标签中的凭据。 */
        snprintf(s_pop, sizeof(s_pop), "%08" PRIX32, esp_random());
        err = nvs_set_str(handle, PROVISIONING_NVS_POP_KEY, s_pop);
        if (err == ESP_OK) {
            err = nvs_commit(handle);
        }
    }
    nvs_close(handle);
    return err;
}

static void make_service_name(void)
{
    const device_identity_t *identity = device_identity_get();
    unsigned int mac3 = 0;
    unsigned int mac4 = 0;
    unsigned int mac5 = 0;

    if (identity != NULL &&
        sscanf(identity->device_id, "%*02X:%*02X:%*02X:%02X:%02X:%02X",
               &mac3, &mac4, &mac5) == 3) {
        snprintf(s_service_name, sizeof(s_service_name),
                 "LUMMISS_%02X%02X%02X", mac3, mac4, mac5);
    } else {
        snprintf(s_service_name, sizeof(s_service_name), "LUMMISS_DEVICE");
    }
}

static esp_err_t prepare_security2(void)
{
    ESP_RETURN_ON_ERROR(load_or_create_pop(), TAG, "准备配网口令失败");

    int verifier_len = 0;
    esp_err_t err = esp_srp_gen_salt_verifier(
        PROVISIONING_USERNAME, strlen(PROVISIONING_USERNAME),
        s_pop, strlen(s_pop),
        &s_salt, PROVISIONING_SALT_LEN,
        &s_verifier, &verifier_len);
    if (err != ESP_OK) {
        return err;
    }
    if (verifier_len <= 0 || verifier_len > UINT16_MAX) {
        free(s_salt);
        free(s_verifier);
        s_salt = NULL;
        s_verifier = NULL;
        return ESP_ERR_INVALID_SIZE;
    }

    s_security2_params.salt = s_salt;
    s_security2_params.salt_len = PROVISIONING_SALT_LEN;
    s_security2_params.verifier = s_verifier;
    s_security2_params.verifier_len = (uint16_t)verifier_len;
    return ESP_OK;
}

static void release_security2(void)
{
    free(s_salt);
    free(s_verifier);
    s_salt = NULL;
    s_verifier = NULL;
    memset(&s_security2_params, 0, sizeof(s_security2_params));
}

static void provisioning_event_handler(void *user_data,
                                       esp_event_base_t event_base,
                                       int32_t event_id,
                                       void *event_data)
{
    (void)user_data;
    if (event_base != NETWORK_PROV_EVENT) {
        return;
    }

    switch ((network_prov_cb_event_t)event_id) {
    case NETWORK_PROV_START:
        s_active = true;
        ESP_LOGI(TAG, "BLE 配网已启动，设备名=%s", s_service_name);
        provisioning_notify(PROVISIONING_EVENT_STARTED);
        break;

    case NETWORK_PROV_WIFI_CRED_RECV: {
        const wifi_sta_config_t *config = event_data;
        ESP_LOGI(TAG, "收到 Wi-Fi 凭据，SSID=%s",
                 config != NULL ? (const char *)config->ssid : "");
        provisioning_notify(PROVISIONING_EVENT_CREDENTIALS_RECEIVED);
        break;
    }

    case NETWORK_PROV_WIFI_CRED_SUCCESS:
        ESP_LOGI(TAG, "配网成功，等待 BLE 向 App 返回最终状态后关闭服务");
        provisioning_notify(PROVISIONING_EVENT_SUCCEEDED);
        break;

    case NETWORK_PROV_WIFI_CRED_FAIL: {
        const network_prov_wifi_sta_fail_reason_t *reason = event_data;
        ESP_LOGW(TAG, "配网连接失败：%s",
                 reason != NULL && *reason == NETWORK_PROV_WIFI_STA_AUTH_ERROR
                     ? "认证失败" : "未找到接入点");
        provisioning_notify(PROVISIONING_EVENT_FAILED);
        /* 清除失败凭据并恢复为可继续接收下一组凭据的状态。 */
        esp_err_t err = network_prov_mgr_reset_wifi_sm_state_on_failure();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "重置配网状态失败：%s", esp_err_to_name(err));
        }
        break;
    }

    case NETWORK_PROV_END: {
        s_active = false;
        esp_err_t err = network_prov_mgr_deinit();
        s_manager_initialized = false;
        release_security2();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "释放配网管理器失败：%s", esp_err_to_name(err));
        }
        ESP_LOGI(TAG, "BLE 配网服务已关闭");
        provisioning_notify(PROVISIONING_EVENT_STOPPED);
        break;
    }

    default:
        break;
    }
}

esp_err_t provisioning_manager_start(provisioning_event_callback_t callback,
                                     void *user_ctx,
                                     bool *provisioning_started)
{
    if (callback == NULL || provisioning_started == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_manager_initialized || s_active) {
        return ESP_ERR_INVALID_STATE;
    }

    *provisioning_started = false;
    s_callback = callback;
    s_user_ctx = user_ctx;
    make_service_name();

    network_prov_mgr_config_t manager_config = {
        .scheme = network_prov_scheme_ble,
        .scheme_event_handler = NETWORK_PROV_SCHEME_BLE_EVENT_HANDLER_FREE_BLE,
        .network_prov_wifi_conn_cfg = {
            .wifi_conn_attempts = 5,
        },
    };

    ESP_RETURN_ON_ERROR(esp_event_handler_register(NETWORK_PROV_EVENT,
                                                   ESP_EVENT_ANY_ID,
                                                   provisioning_event_handler,
                                                   NULL),
                        TAG, "注册配网事件处理器失败");

    esp_err_t err = network_prov_mgr_init(manager_config);
    if (err != ESP_OK) {
        esp_event_handler_unregister(NETWORK_PROV_EVENT,
                                     ESP_EVENT_ANY_ID,
                                     provisioning_event_handler);
        ESP_LOGE(TAG, "初始化官方配网管理器失败：%s", esp_err_to_name(err));
        return err;
    }
    s_manager_initialized = true;

    bool provisioned = false;
    err = network_prov_mgr_is_wifi_provisioned(&provisioned);
    if (err != ESP_OK) {
        network_prov_mgr_deinit();
        s_manager_initialized = false;
        return err;
    }

    bool force_provisioning = false;
#ifdef CONFIG_LUMMISS_FORCE_BLE_PROVISIONING
    force_provisioning = true;
#endif

    if (provisioned && !force_provisioning) {
        ESP_LOGI(TAG, "C6 已保存 Wi-Fi 凭据，无需启动 BLE 配网");
        err = network_prov_mgr_deinit();
        s_manager_initialized = false;
        return err;
    }

    if (provisioned && force_provisioning) {
        ESP_LOGW(TAG, "开发测试：保留 C6 已保存凭据，仍强制启动 BLE 配网");
    }

    err = prepare_security2();
    if (err != ESP_OK) {
        network_prov_mgr_deinit();
        s_manager_initialized = false;
        return err;
    }

    static uint8_t service_uuid[16] = {
        0xb4, 0xdf, 0x5a, 0x1c, 0x3f, 0x6b, 0xf4, 0xbf,
        0xea, 0x4a, 0x82, 0x03, 0x04, 0x90, 0x1a, 0x02,
    };
    err = network_prov_scheme_ble_set_service_uuid(service_uuid);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "设置配网 BLE UUID 失败：%s", esp_err_to_name(err));
        goto fail;
    }

    err = network_prov_mgr_start_provisioning(
        NETWORK_PROV_SECURITY_2,
        &s_security2_params,
        s_service_name,
        NULL);
    if (err != ESP_OK) {
        goto fail;
    }

    *provisioning_started = true;
    ESP_LOGI(TAG, "配网参数：用户名=%s，PoP=%s", PROVISIONING_USERNAME, s_pop);
    ESP_LOGI(TAG,
             "官方 App 二维码数据={\"ver\":\"v1\",\"name\":\"%s\","
             "\"username\":\"%s\",\"pop\":\"%s\","
             "\"transport\":\"ble\",\"network\":\"wifi\"}",
             s_service_name, PROVISIONING_USERNAME, s_pop);
    return ESP_OK;

fail:
    release_security2();
    network_prov_mgr_deinit();
    s_manager_initialized = false;
    return err;
}

bool provisioning_manager_is_active(void)
{
    return s_active;
}

const char *provisioning_manager_service_name(void)
{
    return s_service_name;
}

const char *provisioning_manager_username(void)
{
    return PROVISIONING_USERNAME;
}

const char *provisioning_manager_pop(void)
{
    return s_pop;
}
