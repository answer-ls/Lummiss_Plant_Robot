#include <assert.h>
#include <stdbool.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "camera_driver.h"
#include "display_driver.h"
#include "network_manager.h"

static const char *TAG = "APP_MAIN";

/* UI 初始化包含 LCD、LVGL 和表情播放器。
 * 初始化完成后，LVGL Port 的内部任务负责定时器和屏幕刷新；本任务保持存活，
 * 后续可以在这里接收 UI 队列事件，统一执行页面和表情切换。 */
static void ui_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "UI Task 启动");
    display_driver_start();

    while (true) {
        /* 当前基础工程还没有业务事件，先低频休眠避免空转占用 CPU。 */
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

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
     * 主入口只调用统一接口，不保存凭据，也不处理 WiFi 系统事件。 */
    esp_err_t network_error = network_manager_init();
    if (network_error == ESP_OK) {
        network_error = network_manager_start();
    }
    if (network_error != ESP_OK) {
        /* 网络故障不能阻止屏幕和摄像头启动，后续由故障管理器统一处理。 */
        ESP_LOGE(TAG, "Network Manager 启动失败：%s",
                 esp_err_to_name(network_error));
    }

    BaseType_t result = xTaskCreate(ui_task, "ui_task", 8192, NULL, 6, NULL);
    assert(result == pdPASS);

    result = xTaskCreate(camera_task, "camera_task", 8192, NULL, 7, NULL);
    assert(result == pdPASS);

    ESP_LOGI(TAG, "UI Task、Camera Task 和 Network Manager 启动完成");
}
