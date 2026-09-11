#include <assert.h>
#include <stdbool.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "camera_driver.h"
#include "display_driver.h"
#include "home_info.h"
#include "network_manager.h"
#include "screen_carousel.h"

static const char *TAG = "APP_MAIN";

/*
 * USB + 视频链路隔离测试开关。
 *
 * 保留 Network Manager（视频 WebSocket 仍需要 Wi-Fi），但暂停天气 HTTPS
 * 和 GIF/SD 卡轮播，便于判断 USB 回调间隔是否仍会出现 100 ms 级停顿。
 * 测试完成后改为 0 即可恢复原来的天气与 GIF 任务。
 */
#define CAMERA_ISOLATION_TEST CAMERA_UVC_ONLY_TEST

/* UI 初始化包含 LCD、LVGL 和动态时间天气首页。
 * 初始化完成后，LVGL Port 的内部任务负责定时器和屏幕刷新；本任务保持存活，
 * 后续可以在这里接收 UI 队列事件，统一执行页面和表情切换。 */
#if !CAMERA_ISOLATION_TEST
static void ui_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "UI Task 启动");
    display_driver_start();

#if CAMERA_ISOLATION_TEST
    ESP_LOGI(TAG, "隔离测试模式：已暂停 GIF/SD 卡轮播，保留显示驱动");
#else
    /* 屏幕轮播：天气首页 5 秒 → TF 卡上每个 GIF 各 5 秒 → 回到首页，循环。
     * 必须放在 display_driver_start() 之后：轮播把已建好的天气首页当作
     * 循环的第一环，也依赖这里初始化好的 LVGL。 */
    screen_carousel_start();
#endif

    while (true) {
        /* 页面刷新由 LVGL 定时器完成，本任务保留给后续 UI 事件队列。 */
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
#endif

/* 摄像头驱动在独立任务内运行，USB 收帧不会阻塞 UI 初始化和刷新。 */
static void camera_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Camera Task 启动");
    camera_driver_run();

    /* 驱动通常不会返回；发生不可恢复错误时结束当前任务并保留系统日志。 */
    ESP_LOGE(TAG, "Camera Task 已停止");
    vTaskDelete(NULL);
}

void app_main(void)
{
    ESP_LOGI(TAG,
             "Lummiss 基础工程启动：FreeRTOS + 屏幕 + USB 摄像头 + WiFi + H.264 实时流");

    /* Network Manager 内部负责 ESP-Hosted、C6、WiFi 事件和 DHCP。
     * UVC-only 测试必须完全跳过它，避免 WiFi/SDIO 负载干扰 USB 统计。 */
#if CAMERA_ISOLATION_TEST
    ESP_LOGI(TAG, "UVC-only 隔离测试：跳过 Network Manager/WiFi/ESP-Hosted");
#else
    esp_err_t network_error = network_manager_init();
    if (network_error == ESP_OK) {
        network_error = network_manager_start();
    }
    if (network_error != ESP_OK) {
        /* 网络故障不能阻止屏幕和摄像头启动，后续由故障管理器统一处理。 */
        ESP_LOGE(TAG, "Network Manager 启动失败：%s",
                 esp_err_to_name(network_error));
    }
#endif

#if CAMERA_ISOLATION_TEST
    ESP_LOGI(TAG, "隔离测试模式：已暂停天气 HTTPS/首页信息任务");
#else
    /* 首页信息服务独立等待网络并访问定位、网络时间和天气接口。 */
    esp_err_t home_error = home_info_start();
    if (home_error != ESP_OK) {
        ESP_LOGE(TAG, "首页信息服务启动失败：%s", esp_err_to_name(home_error));
    }
#endif

    BaseType_t result;
#if CAMERA_ISOLATION_TEST
    ESP_LOGI(TAG, "UVC-only 隔离测试：跳过 UI/LVGL、天气 HTTPS 和 GIF/SD 轮播");
#else
    result = xTaskCreate(ui_task, "ui_task", 8192, NULL, 6, NULL);
    assert(result == pdPASS);
#endif

    result = xTaskCreate(camera_task, "camera_task", 8192, NULL, 7, NULL);
    assert(result == pdPASS);

    ESP_LOGI(TAG, "UI Task、Camera Task 和 Network Manager 启动完成");
}
