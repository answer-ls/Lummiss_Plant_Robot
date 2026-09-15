#include <assert.h>
#include <stdbool.h>
#include <stdint.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "camera_driver.h"
#include "device_identity.h"
#include "display_driver.h"
#include "home_info.h"
#include "network_manager.h"
#include "ota_client.h"
#include "screen_carousel.h"
#include "test_profile.h"
#include "video_streamer.h"
#include "xiaozhi_audio.h"

static const char *TAG = "APP_MAIN";

#define APP_USB_CORE 0
#define APP_UI_CORE  1

/* UI 初始化包含 LCD、LVGL 和动态时间天气首页。
 * 初始化完成后，LVGL Port 的内部任务负责定时器和屏幕刷新。 */
#if TP_HAS(UI)
static void ui_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "UI Task 启动（CPU%d）", xPortGetCoreID());
    display_driver_start();

#if TP_HAS(SD)
    /* 屏幕轮播：天气首页 5 秒 → TF 卡上每个 GIF 各 5 秒 → 回到首页，循环。
     * 必须放在 display_driver_start() 之后：轮播把已建好的天气首页当作
     * 循环的第一环，也依赖这里初始化好的 LVGL。 */
    screen_carousel_start();
#else
    ESP_LOGI(TAG, "测试档位=%d（%s）：已暂停 GIF/SD 卡轮播",
             CAMERA_TEST_PROFILE, test_profile_name());
#endif

    /* 当前没有 UI 事件队列，初始化任务完成后退出，减少常驻空任务。 */
    vTaskDelete(NULL);
}
#endif

/* 摄像头驱动在独立任务内运行，USB 收帧不会阻塞 UI 初始化和刷新。 */
static void camera_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Camera Task 启动（CPU%d）", xPortGetCoreID());
    camera_driver_run();

    /* 驱动通常不会返回；发生不可恢复错误时结束当前任务并保留系统日志。 */
    ESP_LOGE(TAG, "Camera Task 已停止");
    vTaskDelete(NULL);
}

void app_main(void)
{
    ESP_LOGI(TAG,
             "Lummiss 基础工程启动：FreeRTOS + 屏幕 + USB 摄像头 + WiFi + H.264 实时流");
    ESP_LOGI(TAG, "测试档位=%d（%s）", CAMERA_TEST_PROFILE, test_profile_name());
    ESP_LOGI(TAG,
             "任务分配：CPU0=USB/UVC+ESP-Hosted+WebSocket+音频，"
             "CPU1=视频编解码+LVGL+动画");

    /* 设备身份：MAC (Device-Id) 和 UUID (Client-Id) 必须优先初始化，
     * OTA 请求和 WebSocket 建连均依赖这两个标识。 */
    esp_err_t identity_error = device_identity_init();
    if (identity_error != ESP_OK) {
        ESP_LOGE(TAG, "设备身份初始化失败：%s", esp_err_to_name(identity_error));
    }

    /* Network Manager 内部负责 ESP-Hosted、C6、WiFi 事件和 DHCP。
     * UVC-only 测试必须完全跳过它，避免 WiFi/SDIO 负载干扰 USB 统计。 */
#if TP_HAS(WIFI)
    esp_err_t network_error = network_manager_init();
    if (network_error == ESP_OK) {
        network_error = network_manager_start();
    }
    if (network_error != ESP_OK) {
        /* 网络故障不能阻止屏幕和摄像头启动，后续由故障管理器统一处理。 */
        ESP_LOGE(TAG, "Network Manager 启动失败：%s",
                 esp_err_to_name(network_error));
    }
#else
    ESP_LOGI(TAG, "测试档位=%d（%s）：跳过 Network Manager/WiFi/ESP-Hosted",
             CAMERA_TEST_PROFILE, test_profile_name());
#endif

    /* OTA 检查：设备身份必须在调用前初始化完毕。 */
#if TP_HAS(WIFI)
    ota_client_init();
    if (identity_error == ESP_OK && network_error == ESP_OK) {
        /* 首次使用时持续等待 App 完成 BLE 配网。已有凭据时维持原来的
         * 20 秒启动上限。Network Manager 仅在 BLE 已关闭后才报告 ready，
         * 避免配网和摄像头/H.264 同时运行。 */
        const bool provisioning = network_manager_is_provisioning();
        if (provisioning) {
            ESP_LOGI(TAG, "等待 App 完成 BLE 配网...");
        } else {
            ESP_LOGI(TAG, "等待 WiFi 连接以发起 OTA 检查...");
        }
        bool wifi_ready = network_manager_wait_connected(
            provisioning ? UINT32_MAX : 20000);
        if (wifi_ready) {
            ESP_LOGI(TAG, "WiFi 已连接，发起 OTA 检查");
            ota_result_t ota_result;
            esp_err_t ota_error = ota_client_check(&ota_result);

            if (ota_error == ESP_OK && !ota_result.has_error) {
                ESP_LOGI(TAG, "OTA 检查成功");

                if (ota_result.has_activation) {
                    ESP_LOGW(TAG, "设备未激活！激活码：%s\n%s",
                             ota_result.activation_code,
                             ota_result.activation_message);
                }

                if (ota_result.has_firmware) {
                    ESP_LOGI(TAG, "发现新固件：%s（%s）",
                             ota_result.firmware_version,
                             ota_result.firmware_url);
                    /* TODO: 固件下载和升级流程 */
                }

                /* 将 OTA 获取的 WebSocket 配置传递给 video_streamer。
                 * 必须在摄像头任务启动前完成，确保上传任务使用正确的地址和凭证。 */
                const device_identity_t *id = device_identity_get();
                video_streamer_config_t ws_cfg = {0};
                snprintf(ws_cfg.ws_url, sizeof(ws_cfg.ws_url),
                         "%s", ota_result.websocket_url);
                snprintf(ws_cfg.token, sizeof(ws_cfg.token),
                         "%s", ota_result.websocket_token);
                if (id != NULL) {
                    snprintf(ws_cfg.device_id, sizeof(ws_cfg.device_id),
                             "%s", id->device_id);
                    snprintf(ws_cfg.client_id, sizeof(ws_cfg.client_id),
                             "%s", id->client_id);
                }
                video_streamer_set_config(&ws_cfg);
            } else {
                ESP_LOGE(TAG, "OTA 检查失败：%s",
                         ota_result.has_error ? ota_result.error : "未知错误");
            }
        } else {
            ESP_LOGW(TAG, "WiFi 连接超时，跳过 OTA 检查");
        }
    }
#endif

#if TP_HAS(XIAOZHI)
    /* 小智初始化板载 ES8311，并注册到 video_streamer 的共享 Agent WSS。
     * 连接仍使用上面 OTA 注入的 URL、Token、Device-Id 和 Client-Id。 */
    esp_err_t xiaozhi_error = xiaozhi_audio_start();
    if (xiaozhi_error != ESP_OK) {
        ESP_LOGE(TAG, "启动小智语音服务失败：%s",
                 esp_err_to_name(xiaozhi_error));
    }
#endif

#if TP_HAS(WEATHER)
    /* 首页信息服务独立等待网络并访问定位、网络时间和天气接口。 */
    esp_err_t home_error = home_info_start();
    if (home_error != ESP_OK) {
        ESP_LOGE(TAG, "首页信息服务启动失败：%s", esp_err_to_name(home_error));
    }
#else
    ESP_LOGI(TAG, "测试档位=%d（%s）：已暂停天气 HTTPS/首页信息任务",
             CAMERA_TEST_PROFILE, test_profile_name());
#endif

    BaseType_t result;
#if TP_HAS(UI)
    result = xTaskCreatePinnedToCore(ui_task, "ui_task", 8192, NULL, 6,
                                     NULL, APP_UI_CORE);
    assert(result == pdPASS);
#else
    ESP_LOGI(TAG, "测试档位=%d（%s）：跳过 UI/LVGL",
             CAMERA_TEST_PROFILE, test_profile_name());
#endif

    result = xTaskCreatePinnedToCore(camera_task, "camera_task", 8192, NULL, 7,
                                     NULL, APP_USB_CORE);
    assert(result == pdPASS);

    ESP_LOGI(TAG, "测试档位=%d（%s）：已完成所选任务启动",
             CAMERA_TEST_PROFILE, test_profile_name());
}
