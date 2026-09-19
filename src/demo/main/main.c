#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "camera_driver.h"
#include "cloud_mcp.h"
#include "mcp_registry.h"
#include "device_identity.h"
#include "display_driver.h"
#include "expression_manager.h"
#include "home_info.h"
#include "network_manager.h"
#include "ota_client.h"
#include "peripheral_test.h"
#include "test_profile.h"
#include "time_service.h"
#include "video_streamer.h"
#include "cloud_mqtt.h"
#include "xiaozhi_audio.h"
#include "ambient_led.h"
#include "person_detect.h"

static const char *TAG = "APP_MAIN";

#define APP_USB_CORE 0
#define APP_UI_CORE  1

static void app_log_memory(const char *stage)
{
    ESP_LOGI(TAG,
             "MEM[%s]: DMA free=%u largest=%u INT free=%u largest=%u "
             "PSRAM free=%u largest=%u",
             stage,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
}

#if defined(CONFIG_CLOUD_PROTOCOL_V3)
/* MCP 工具 self.get_device_status 的执行体。
 * JSON 字段形状对照官方 wifi_board.cc 的 GetDeviceStatusJson：
 * 本设备没有电池模块、背光不可调，相关字段省略（不编造数值）。 */
static void device_status_tool_handler(const cJSON *arguments,
                                       char *result, size_t result_size,
                                       bool *is_error)
{
    (void)arguments;
    snprintf(result, result_size, "{\"audio_speaker\":{\"volume\":%d}}",
             xiaozhi_audio_get_volume());
    *is_error = false;
}

/* MCP 工具 self.audio_speaker.set_volume 的执行体：转发到
 * xiaozhi_audio 的公开音量接口（0-100，与官方同名同参）。
 * 执行在 cloud_mcp 的 mcp_task 上下文；音量接口只是设 codec 寄存器，
 * 不是耗时操作。 */
static void volume_set_tool_handler(const cJSON *arguments,
                                    char *result, size_t result_size,
                                    bool *is_error)
{
    const cJSON *volume = cJSON_GetObjectItem(arguments, "volume");
    if (!cJSON_IsNumber(volume)) {
        *is_error = true;
        snprintf(result, result_size,
                 "{\"error\":\"invalid_argument\",\"detail\":\"volume (0-100) required\"}");
        return;
    }
    int level = volume->valueint;
    if (level < 0) {
        level = 0;
    }
    if (level > 100) {
        level = 100;
    }
    if (xiaozhi_audio_set_volume(level) != ESP_OK) {
        /* 音频编解码器还没就绪（会话早于音频初始化完成）时走这里 */
        *is_error = true;
        snprintf(result, result_size,
                 "{\"error\":\"NOT_AVAILABLE\",\"detail\":\"audio codec not ready\"}");
        return;
    }
    snprintf(result, result_size, "{\"volume\":%d}", level);
    *is_error = false;
}
#endif

/* UI 初始化包含 LCD、LVGL 和动态时间天气首页。
 * 初始化完成后，LVGL Port 的内部任务负责定时器和屏幕刷新。 */
#if TP_HAS(UI)
static EventGroupHandle_t s_ui_ready_events;
static const EventBits_t UI_READY_BIT = BIT0;

static void ui_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "UI Task 启动（CPU%d）", xPortGetCoreID());
    display_driver_start();
#if TP_HAS(SD)
    if (expression_manager_init() != ESP_OK) {
        ESP_LOGE(TAG, "Expression Manager 初始化失败，保持 HOME 页面");
    }
    /* 当前采用事件驱动表情，SD 卡仅由 Expression Manager 按事件读取；
     * 不启动旧的 screen_carousel 自动轮播任务或定时器。 */
    ESP_LOGI(TAG, "已禁用表情自动轮播，设备保持 HOME 页面");
#else
    ESP_LOGI(TAG, "测试档位=%d（%s）：已暂停 GIF/SD 卡轮播",
             CAMERA_TEST_PROFILE, test_profile_name());
#endif

    /* LVGL 显示缓冲已经分配完成，且 UI 对 SD 卡的初始化也已结束。
     * 主流程收到这个事件后才允许启动 ESP-DL 模型加载，避免两者同时
     * 争用内部 DMA 内存和 SDMMC。 */
    if (s_ui_ready_events != NULL) {
        xEventGroupSetBits(s_ui_ready_events, UI_READY_BIT);
    }
    ESP_LOGI(TAG, "UI/LVGL 初始化完成，允许启动本地人体检测");

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
#if defined(CONFIG_XIAOZHI_USE_OFFICIAL_SERVER)
    ESP_LOGW(TAG, "SERVER_MODE=XIAOZHI_OFFICIAL（官方测试，临时）");
#else
    ESP_LOGI(TAG, "SERVER_MODE=LUMMISS Official Xiaozhi test=OFF");
#endif
#if defined(CONFIG_CLOUD_PROTOCOL_V3)
    ESP_LOGI(TAG,
             "CLOUD_PROTOCOL=V3（MQTT 控制 + UDP Opus + WebRTC H264）；"
             "阶段一：仅 OTA + MQTT + AI hello v3");
#else
    ESP_LOGW(TAG, "CLOUD_PROTOCOL=LEGACY_WS（Agent WebSocket 单连接，回滚用）");
#endif
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

#if CAMERA_TEST_PROFILE == CAMERA_TEST_FULL
    /* 完整系统也初始化氛围灯；灯效任务只占用 RMT 非 DMA 内存，
     * 不再要求进入档位 9 才能使用 MCP 和状态灯。 */
    if (ambient_led_init() != ESP_OK) {
        ESP_LOGW(TAG, "氛围灯初始化失败，继续启动其它系统模块");
    }
    ambient_led_set_state(AMBIENT_LED_STATE_NETWORK_CONNECTING);
#endif

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
#if defined(CONFIG_CLOUD_PROTOCOL_V3)
    /* 注册官方 MCP 通用工具（工具名与参数对照上游 mcp_server.cc 的
     * AddCommonTools，服务端的 LLM 提示词认识 self.* 这些名字）。
     * 必须在 cloud_mqtt_start() 之前注册 —— 服务端在 MCP 握手完成后立刻
     * 拉取 tools/list，注册晚了本次会话的工具清单里就没有它。 */

    /* 官方约定：LLM 在调音量等控制类工具前，先调 get_device_status 查当前值 */
    static const mcp_tool_t device_status_tool = {
        .name = "self.get_device_status",
        .description = "Provides the real-time information of the device, "
                       "including the current status of the audio speaker.",
        .input_schema = NULL,   /* 官方定义无参数 */
        .handler = device_status_tool_handler,
    };
    mcp_registry_register(&device_status_tool);

    static const mcp_tool_t volume_tool = {
        .name = "self.audio_speaker.set_volume",
        .description = "Set the volume of the audio speaker. If the current "
                       "volume is unknown, you must call `self.get_device_status` "
                       "tool first and then call this tool.",
        .input_schema = "{\"type\":\"object\",\"properties\":{"
                        "\"volume\":{\"type\":\"integer\","
                        "\"minimum\":0,\"maximum\":100}},"
                        "\"required\":[\"volume\"]}",
        .handler = volume_set_tool_handler,
    };
    mcp_registry_register(&volume_tool);

    /* 立刻初始化 MCP（幂等）：内置工具（motion.stop / motion.get_state /
     * light.pulse）与 4 个未实现占位都在 cloud_mcp_init 里登记。
     * **必须早于 ota_client_check** —— OTA 请求体的 capability_manifest
     * 从注册表生成，晚了就会漏掉内置工具（2026-09-19 review 发现的缺陷）。
     * cloud_mqtt_start 内部还会再调一次，是空操作。 */
    if (cloud_mcp_init() != ESP_OK) {
        ESP_LOGE(TAG, "MCP 初始化失败，工具与能力清单将不完整");
    }
#endif
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
            /* 文档 §2 的启动顺序：WiFi → NTP_TIME_READY → OTA_CONFIGURED。
             * SNTP 必须早于 OTA 与 RTC 信令（RTC 的 sent_at 允许与服务器
             * 相差 5 分钟，时间不对会让 SDP/ICE 被丢弃）。
             * 时间同步统一走 time_service —— **全工程只有它能初始化 SNTP**，
             * home_info 只等不初始化（重复 init 会报 already initialized，
             * 而 sync_wait 的信号量只能被消费一次，第二个调用方必然超时）。
             * 校时失败不阻塞启动：阶段一/二不依赖墙钟，阶段四 RTC 前会重试。 */
            time_service_start();
            if (time_service_wait_synced(15000)) {
                ESP_LOGI(TAG, "NTP 校时完成");
#if defined(CONFIG_CLOUD_PROTOCOL_V3)
                cloud_mqtt_notify_ntp_ready();
#endif
            } else {
                ESP_LOGW(TAG, "NTP 校时超时，继续启动（阶段四 RTC 信令前再重试）");
            }

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

                /* 通道配置分叉（Kconfig 二选一，不允许两条链路同时跑）：
                 *   V3   → MQTT 六字段交给 cloud_mqtt；**不配置也不建立
                 *          任何 WebSocket**，旧 Agent 通道彻底停用；
                 *   旧协议 → 沿用 WebSocket 配置。 */
#if defined(CONFIG_CLOUD_PROTOCOL_V3)
                cloud_mqtt_config_t mqtt_cfg = {0};
                snprintf(mqtt_cfg.endpoint, sizeof(mqtt_cfg.endpoint), "%s",
                         ota_result.mqtt_endpoint);
                snprintf(mqtt_cfg.client_id, sizeof(mqtt_cfg.client_id), "%s",
                         ota_result.mqtt_client_id);
                snprintf(mqtt_cfg.username, sizeof(mqtt_cfg.username), "%s",
                         ota_result.mqtt_username);
                snprintf(mqtt_cfg.password, sizeof(mqtt_cfg.password), "%s",
                         ota_result.mqtt_password);
                snprintf(mqtt_cfg.publish_topic,
                         sizeof(mqtt_cfg.publish_topic), "%s",
                         ota_result.mqtt_publish_topic);
                snprintf(mqtt_cfg.subscribe_topic,
                         sizeof(mqtt_cfg.subscribe_topic), "%s",
                         ota_result.mqtt_subscribe_topic);
                if (cloud_mqtt_start(&mqtt_cfg) != ESP_OK) {
                    ESP_LOGE(TAG, "MQTT 通道启动失败");
                }
                ESP_LOGI(TAG,
                         "旧 Agent WebSocket 已停用：本模式下不注入 WS 地址，"
                         "video_streamer 不会建立 WebSocket");
                /* 凭据用完即从栈上消失；进 cloud_mqtt 的那份是它自己的静态副本。 */
                memset(&mqtt_cfg, 0, sizeof(mqtt_cfg));
#else
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
#endif
            } else {
                ESP_LOGE(TAG, "OTA 检查失败：%s",
                         ota_result.has_error ? ota_result.error : "未知错误");
            }
        } else {
            ESP_LOGW(TAG, "WiFi 连接超时，跳过 OTA 检查");
        }
    }
#endif

#if TP_HAS(WIFI) && VIDEO_STREAM_PC_PREVIEW_ENABLED
    /* 本地预览使用无鉴权 TCP WebSocket，覆盖 OTA 返回的云端 WSS 地址。 */
    video_streamer_config_t pc_preview_cfg = {0};
    snprintf(pc_preview_cfg.ws_url, sizeof(pc_preview_cfg.ws_url), "%s",
             VIDEO_STREAM_PC_PREVIEW_URL);
    video_streamer_set_config(&pc_preview_cfg);
    ESP_LOGW(TAG, "本地 PC 视频预览已启用：%s", pc_preview_cfg.ws_url);
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

#if TP_HAS(PERIPH)
    /* 按键 / TTP223 触摸 / WS2812 氛围灯 的驱动自检。
     * 只初始化驱动、登记回调并跑固定灯效序列，不含任何业务状态映射。
     * 默认档位 0 不带 TP_PERIPH，所以这条路径不影响已经稳定的完整系统。 */
    esp_err_t periph_error = peripheral_test_start();
    if (periph_error != ESP_OK) {
        ESP_LOGE(TAG, "外设自检启动失败：%s", esp_err_to_name(periph_error));
    }
#else
    ESP_LOGI(TAG, "测试档位=%d（%s）：跳过按键/触摸固定自检，氛围灯由状态/MCP管理",
             CAMERA_TEST_PROFILE, test_profile_name());
#endif

    BaseType_t result;
#if CAMERA_TEST_PROFILE == CAMERA_TEST_FULL
    /* UI 必须先完成 LVGL 双缓冲申请，再启动 ESP-DL 模型加载。
     * 模型加载会使用大量 PSRAM/运行时内存，不能与 LVGL 首次分配并发。 */
#if TP_HAS(UI)
    s_ui_ready_events = xEventGroupCreate();
    if (s_ui_ready_events == NULL) {
        ESP_LOGE(TAG, "创建 UI 就绪事件失败，跳过本地人体检测");
        result = xTaskCreatePinnedToCore(ui_task, "ui_task", 8192, NULL, 6,
                                         NULL, APP_UI_CORE);
        assert(result == pdPASS);
    } else {
        result = xTaskCreatePinnedToCore(ui_task, "ui_task", 8192, NULL, 6,
                                         NULL, APP_UI_CORE);
        if (result != pdPASS) {
            ESP_LOGE(TAG, "创建 UI Task 失败，跳过本地人体检测");
            vEventGroupDelete(s_ui_ready_events);
            s_ui_ready_events = NULL;
        } else {
            /* display_driver_start() 失败时原有 ESP_ERROR_CHECK 会直接复位，
             * 因此这里等待到 UI 明确完成，不再让模型加载与 LVGL 竞争。 */
            (void)xEventGroupWaitBits(s_ui_ready_events, UI_READY_BIT,
                                      pdFALSE, pdTRUE, portMAX_DELAY);
            app_log_memory("LVGL_READY");
            app_log_memory("CAMERA_START");
            ESP_LOGI(TAG, "准备启动 Camera Task");
            if (camera_driver_prepare_ready_event() != ESP_OK) {
                ESP_LOGE(TAG, "CAMERA_READY 事件初始化失败，跳过本地人体检测");
            } else {
                result = xTaskCreatePinnedToCore(camera_task, "camera_task", 8192,
                                                 NULL, 7, NULL, APP_USB_CORE);
                if (result != pdPASS) {
                    ESP_LOGE(TAG, "创建 Camera Task 失败，跳过本地人体检测");
                } else {
                    ESP_LOGI(TAG, "等待 CAMERA_READY（UVC stream open + 第一帧有效 MJPEG）");
                    if (!camera_driver_wait_ready(60000)) {
                        ESP_LOGE(TAG, "等待 CAMERA_READY 超时，跳过本地人体检测");
                    } else if (person_detect_init() != ESP_OK) {
                        ESP_LOGW(TAG, "本地人体检测初始化失败，继续运行其它模块");
                    }
                }
            }
            vEventGroupDelete(s_ui_ready_events);
            s_ui_ready_events = NULL;
        }
    }
#else
    if (person_detect_init() != ESP_OK) {
        ESP_LOGW(TAG, "本地人体检测初始化失败，继续运行其它模块");
    }
#endif
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

#if !TP_HAS(UI)
    ESP_LOGI(TAG, "测试档位=%d（%s）：跳过 UI/LVGL",
             CAMERA_TEST_PROFILE, test_profile_name());
#elif CAMERA_TEST_PROFILE != CAMERA_TEST_FULL
    /* 非完整档位不需要等待人体检测，但保留原有 UI 启动行为。 */
    result = xTaskCreatePinnedToCore(ui_task, "ui_task", 8192, NULL, 6,
                                     NULL, APP_UI_CORE);
    assert(result == pdPASS);
#endif

#if CAMERA_TEST_PROFILE != CAMERA_TEST_FULL
    result = xTaskCreatePinnedToCore(camera_task, "camera_task", 8192, NULL, 7,
                                     NULL, APP_USB_CORE);
    assert(result == pdPASS);
#endif

    ESP_LOGI(TAG, "测试档位=%d（%s）：已完成所选任务启动",
             CAMERA_TEST_PROFILE, test_profile_name());
}
