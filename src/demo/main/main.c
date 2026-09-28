#include "mem_contig.h"
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "camera_driver.h"
#include "cloud_mcp.h"
#include "camera_photo.h"
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
#include "webrtc_whip.h"
#include "cloud_mqtt.h"
#include "xiaozhi_audio.h"
#include "ambient_led.h"
#include "person_detect.h"
#include "sd_card.h"
#include "board_init.h"

static const char *TAG = "APP_MAIN";

#define APP_USB_CORE 0
#define APP_UI_CORE  1

/* 完整系统关闭 YOLO 且未开启 UVC 丢包诊断时，RTC 指令才按需启动摄像头。 */
#if CAMERA_TEST_PROFILE == CAMERA_TEST_FULL && !CAMERA_PERSON_DETECT_ENABLED && \
    !CAMERA_UVC_AUTOSTART && defined(CONFIG_CLOUD_PROTOCOL_V3)
#define RTC_CAMERA_ON_DEMAND 1
#else
#define RTC_CAMERA_ON_DEMAND 0
#endif

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

#if CAMERA_TEST_PROFILE == CAMERA_TEST_FULL && FULL_POWER_DIAG_HOLD_MS > 0
static void full_power_diag_hold(const char *stage)
{
    /* 欠压定位只在模块之间等待；业务任务继续运行，因此能分辨稳态负载和启动瞬态。 */
    ESP_LOGW(TAG, "POWER_DIAG stage=%s hold_begin=%dms", stage, FULL_POWER_DIAG_HOLD_MS);
    vTaskDelay(pdMS_TO_TICKS(FULL_POWER_DIAG_HOLD_MS));
    ESP_LOGW(TAG, "POWER_DIAG stage=%s hold_pass", stage);
}
#endif

/*
 * 统一打印当前完整系统的长期 Buffer 预算。
 *
 * 这里列的是应用代码能够确定的显式缓冲和已知组件池；ESP-SR、ESP-DL、
 * esp_peer、USB Host 等库内部还会按运行状态申请控制块/描述符，API 没有逐项
 * 查询接口，所以单独标成“组件内部动态”，最终以末尾 heap 实测值为准。
 * 本函数不分配内存，避免诊断本身改变紧张的 INTERNAL/DMA 堆。
 */
static void app_log_memory_budget(void)
{
    ESP_LOGD(TAG, "========== MEMORY BUDGET ==========");

#if CAMERA_PERSON_DETECT_ENABLED
    ESP_LOGD(TAG, "AI_MJPEG       256 KB PSRAM x1 | person_detect_init | AI生命周期 | 可复用=需引用计数");
    ESP_LOGD(TAG, "AI_RGB565      600 KB PSRAM x1 | person_detect_init | AI生命周期 | 可复用=否");
    ESP_LOGD(TAG, "AI_INPUT       200 KB PSRAM x1 | person_detect_init | AI生命周期 | 可复用=否");
    ESP_LOGD(TAG, "AI_MODEL       动态 PSRAM       | ESP-DL模型加载    | AI生命周期 | 可复用=否");
#else
    ESP_LOGD(TAG, "AI_*           0 KB（当前 CAMERA_PERSON_DETECT_ENABLED=0）");
#endif

    ESP_LOGD(TAG, "UVC_URB        18 KB PSRAM+DMA x8 | uvc_host_stream_open | Stream生命周期 | 可复用=否");
    ESP_LOGD(TAG, "UVC_FRAME      512 KB PSRAM x3    | uvc_host_stream_open | Stream生命周期 | 可复用=否");
    ESP_LOGD(TAG, "UVC_HANDOFF    512 KB PSRAM x3    | camera_frame_copy_pool_init | Camera生命周期 | Video/Photo引用共享");

#if TP_HAS(HANDOFF)
    ESP_LOGD(TAG, "VIDEO_MJPEG    0 KB（V3 owned backing 已删除，复用 UVC handoff）");
    ESP_LOGD(TAG, "VIDEO_YUV422   1800 KB PSRAM x1   | video_codec_task | Codec生命周期 | 可复用=需串行仲裁");
    ESP_LOGD(TAG, "VIDEO_YUV420   1350 KB PSRAM x1   | video_codec_task | Codec生命周期 | 可复用=否");
    ESP_LOGD(TAG, "H264_OUT       131200 B PSRAM x4  | video_codec_task | Codec生命周期 | 可复用=槽池内部复用");
    ESP_LOGD(TAG, "H264_HW_REF    约92 KB INTERNAL   | 首帧按需 open | 推流生命周期 | stop 后释放");
#if !defined(CONFIG_CLOUD_PROTOCOL_V3)
    ESP_LOGD(TAG, "VIDEO_AUDIO_Q  1402 B PSRAM x16   | legacy WS only | Codec生命周期 | 队列内部复用");
#else
    ESP_LOGD(TAG, "VIDEO_AUDIO_Q  0 KB（V3 使用 cloud_udp，旧 WS 队列已删除）");
#endif
#endif

#if TP_HAS(UI)
    ESP_LOGD(TAG, "LVGL_DRAW      1920 B INTERNAL+DMA x2 | lvgl_port_add_disp | UI生命周期 | 3行双缓冲A/B测试");
    ESP_LOGD(TAG, "LVGL_TASK      7168 B PSRAM x1 | lvgl_port_init | UI生命周期 | stack_caps=SPIRAM");
#endif
#if TP_HAS(SD)
    ESP_LOGD(TAG, "ANIM_RGB565    150 KB PSRAM x3    | 播放时按需申请 | 动画生命周期 | stop 后释放");
    ESP_LOGD(TAG, "ANIM_SD_READ   4 KB PSRAM x1 | 播放时按需申请 | 动画生命周期 | stop 后释放");
    ESP_LOGD(TAG, "ANIM_INDEX     最多12 KB PSRAM | 文件加载时申请 | 动画生命周期 | stop 后释放");
#endif

#if CAMERA_TEST_PROFILE == CAMERA_TEST_PERIPH_ONLY
    ESP_LOGD(TAG, "AUDIO_HW_TEST 本地 ES8311/I2S 自检 | MIC 20ms采样 + SPK周期提示音");
#elif TP_HAS(XIAOZHI)
    ESP_LOGD(TAG, "AUDIO_CAPTURE  raw/PCM/Opus PSRAM（大小由编码器决定） | audio_work_buffers_init | Audio生命周期 | 可复用=任务内复用");
    ESP_LOGD(TAG, "AUDIO_PCM      4 KB PSRAM x3      | audio_work_buffers_init | Audio生命周期 | 可复用=池内复用");
    ESP_LOGD(TAG, "AUDIO_OPUS_Q   1402 B PSRAM x24  | service_task | Audio生命周期 | 可复用=队列内部复用");
    ESP_LOGD(TAG, "WAKE_FEED      6 KB PSRAM x1（MMR/1024）      | wake_word_init | Wake生命周期 | 可复用=任务内复用");
    ESP_LOGD(TAG, "WAKE_PREROLL   64 KB PSRAM x1     | wake_word_init | Wake生命周期 | 可复用=环形复用");
    ESP_LOGD(TAG, "AFE/WAKENET    组件内部动态（MORE_PSRAM）| ESP-SR | Wake生命周期 | API不暴露精确值");
    ESP_LOGD(TAG, "AUDIO_STACKS   113 KB PSRAM       | xiaozhi_audio_start | Audio生命周期 | 可复用=否");
#endif

#if CAMERA_TEST_PROFILE == CAMERA_TEST_FULL
    ESP_LOGD(TAG, "PHOTO_JPEG     0 KB（引用复用 UVC_HANDOFF 槽，完成后归还）");
#endif
#if defined(CONFIG_CLOUD_PROTOCOL_V3)
    ESP_LOGD(TAG, "MCP_QUEUE      3 KB PSRAM x4      | cloud_mcp_init | App生命周期 | 可复用=队列内部复用");
    ESP_LOGD(TAG, "MQTT_RX/TX     4 KB/1 KB INTERNAL | esp_mqtt_client_init | MQTT生命周期 | 可复用=客户端内部复用");
    ESP_LOGD(TAG, "WHIP_CTRL_Q    约4.4 KB PSRAM     | webrtc_whip_init | App生命周期 | 可复用=队列内部复用");
    ESP_LOGD(TAG, "WHIP_SESSION   约170 KB PSRAM + 19 KB INT + 12 KB DMA | begin_peer | 推流期间 | 可复用=停止后释放");
#endif

    ESP_LOGD(TAG, "PSRAM free=%u largest=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
    ESP_LOGD(TAG, "INT free=%u largest=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    ESP_LOGD(TAG, "DMA free=%u largest=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
    ESP_LOGD(TAG, "===================================");
}

/* app_main 返回后，其主任务栈才会归还 heap。延迟诊断任务使用 PSRAM 栈，
 * 避免为了测量稳定态反而长期占用紧张的 INTERNAL。 */
static void idle_stable_diag_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(4000));
    app_log_memory("IDLE_STABLE");
#if TP_HAS(UI)
    display_driver_log_task_stack();
#endif
    vTaskDeleteWithCaps(NULL);
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
#if CAMERA_TEST_PROFILE == CAMERA_TEST_PERIPH_ONLY
    /* 新 PCB 外设档位显示固定测试图，验证 ST7789 颜色、字节序和方向。 */
    display_driver_show_test_pattern();
    /* 首板屏幕诊断：再次确认 GPIO47 为高电平，后续持续保持。 */
    display_driver_run_backlight_test();
#endif
#if TP_HAS(SD)
#if CAMERA_TEST_PROFILE != CAMERA_TEST_FULL
    /* 隔离测试档位不加载 YOLO，保持原来的 UI 阶段挂载顺序。 */
    const esp_err_t sd_error = sd_card_mount();
    if (sd_error != ESP_OK) {
        ESP_LOGW(TAG, "TF 卡挂载失败，表情资源暂不可用：%s",
                 esp_err_to_name(sd_error));
    }
#endif
    /* 完整系统先初始化页面和播放器，但暂不访问 TF 卡。Flash 模型加载
     * 完成后由 app_main 挂载，避免 SDMMC 与模型映射同时占用内存/中断。 */
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

    /* LVGL 显示缓冲已分配完成。完整系统此时尚未挂载 TF 卡，
     * 主流程收到事件后可先启动摄像头并加载 Flash 模型。 */
    if (s_ui_ready_events != NULL) {
        xEventGroupSetBits(s_ui_ready_events, UI_READY_BIT);
    }
    ESP_LOGI(TAG, "UI/LVGL 初始化完成，允许启动本地人体检测");

    /* 当前没有 UI 事件队列，初始化任务完成后退出，减少常驻空任务。 */
    vTaskDelete(NULL);
}
#endif

#if RTC_CAMERA_ON_DEMAND
static bool s_rtc_camera_started;
#endif

/* 摄像头驱动在独立任务内运行，USB 收帧不会阻塞 UI 初始化和刷新。 */
/* RTC 在启动摄像头前回收动画并初始化编解码任务；不预留 DMA 占位块。 */
static esp_err_t rtc_prepare_resources(uint32_t timeout_ms)
{
    esp_err_t err = expression_manager_prepare_for_webrtc(timeout_ms);
    if (err != ESP_OK) return err;
    return video_streamer_init(NULL);
}

static void camera_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Camera Task 启动（CPU%d）", xPortGetCoreID());
    camera_driver_run();

    /* 驱动通常不会返回；发生不可恢复错误时结束当前任务并保留系统日志。 */
    ESP_LOGE(TAG, "Camera Task 已停止");
#if RTC_CAMERA_ON_DEMAND
    __atomic_store_n(&s_rtc_camera_started, false, __ATOMIC_RELEASE);
#endif
    vTaskDelete(NULL);
}

#if RTC_CAMERA_ON_DEMAND
static esp_err_t rtc_camera_start(void)
{
    bool expected = false;
    if (!__atomic_compare_exchange_n(&s_rtc_camera_started, &expected, true,
                                     false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        return ESP_OK;
    }
    const esp_err_t err = camera_driver_prepare_ready_event();
    if (err != ESP_OK) {
        __atomic_store_n(&s_rtc_camera_started, false, __ATOMIC_RELEASE);
        return err;
    }
    if (xTaskCreatePinnedToCoreWithCaps(camera_task, "camera_task", 8192,
                                        NULL, 7, NULL, APP_USB_CORE,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        __atomic_store_n(&s_rtc_camera_started, false, __ATOMIC_RELEASE);
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "RTC 预览指令已启动 Camera Task，等待 UVC 首帧");
    return ESP_OK;
}

static bool rtc_camera_is_ready(void)
{
    return camera_driver_wait_ready(0);
}
#endif

void app_main(void)
{
    mem_contig_log("APP_MAIN_ENTER");
    mem_fragment_begin();
    ESP_LOGW(TAG, "POWER_DIAG boot_reset_reason=%d (BROWNOUT=%d)",
             (int)esp_reset_reason(), (int)ESP_RST_BROWNOUT);
    /* 新 PCB 的 PWR_IO 引脚由 board_pins.h 定义，必须先拉高并保持供电。 */
    const esp_err_t board_error = board_init_early();
    if (board_error != ESP_OK) {
        ESP_LOGE(TAG, "板级启动 GPIO 初始化失败：%s",
                 esp_err_to_name(board_error));
    }
    app_log_memory("BOOT");
#if CAMERA_TEST_PROFILE == CAMERA_TEST_AUDIO_RAW
    if (board_error != ESP_OK) return;
    ESP_LOGI(TAG, "RAW_ADC_TEST start=%s", esp_err_to_name(xiaozhi_audio_start_raw_test()));
    return;
#endif
#if CAMERA_TEST_PROFILE == CAMERA_TEST_STEPPER_ONLY
    ESP_LOGI(TAG, "测试档位12：仅步进电机，100/50/20/10ms速度阶梯测试");
    if (board_error != ESP_OK) {
        return;
    }
    const esp_err_t motor_err = stepper_test_start();
    ESP_LOGI(TAG, "STEPPER_TEST start=%s", esp_err_to_name(motor_err));
    return;
#endif


#if CAMERA_TEST_PROFILE == CAMERA_TEST_SD_ONLY || CAMERA_TEST_PROFILE == CAMERA_TEST_WIFI_ONLY
    ESP_LOGI(TAG, "测试档位%d：%s", CAMERA_TEST_PROFILE, test_profile_name());
#if CAMERA_TEST_PROFILE == CAMERA_TEST_SD_ONLY
    // 仅测试期间延长 TWDT；不在 SD Busy 轮询中插入任务延时。
    const esp_task_wdt_config_t sd_wdt = {
        .timeout_ms = 15000, .idle_core_mask = (1U << 0) | (1U << 1), .trigger_panic = false,
    };
    esp_err_t sd_wdt_err = esp_task_wdt_reconfigure(&sd_wdt);
    ESP_LOGI(TAG, "SD_TEST TWDT 15s: %s", esp_err_to_name(sd_wdt_err));
    // 本轮只在 FAT 未分配的空闲扇区执行 1000 次 raw CMD24/17，首次失败即停。
    const esp_err_t sd_test_result = sd_card_raw_diagnostic();
    const esp_task_wdt_config_t normal_wdt = {
        .timeout_ms = 5000, .idle_core_mask = (1U << 0) | (1U << 1), .trigger_panic = false,
    };
    if (sd_wdt_err == ESP_OK) {
        ESP_LOGI(TAG, "SD_TEST TWDT restore: %s",
                 esp_err_to_name(esp_task_wdt_reconfigure(&normal_wdt)));
    }
    ESP_LOGI(TAG, "SD_TEST result=%s", esp_err_to_name(sd_test_result));
    app_log_memory("SD_TEST_DONE");
#endif
#if CAMERA_TEST_PROFILE == CAMERA_TEST_WIFI_ONLY || SD_TEST_WIFI_ENABLED
    // 档位11只启动C6/WiFi，不挂载SD卡；档位10可在SD卸载后继续测WiFi。
    ESP_LOGI("WIFI_TEST", "BEGIN: ESP-Hosted/C6 + WiFi + DHCP");
#if defined(CONFIG_ESP_HOSTED_SDIO_HOST_INTERFACE)
    ESP_LOGI("WIFI_TEST", "C6 SDIO: CLK=%d CMD=%d D0=%d D1=%d D2=%d D3=%d reset=%d max_clock=%d kHz",
             CONFIG_ESP_HOSTED_SDIO_PIN_CLK, CONFIG_ESP_HOSTED_SDIO_PIN_CMD,
             CONFIG_ESP_HOSTED_SDIO_PIN_D0, CONFIG_ESP_HOSTED_SDIO_PIN_D1,
             CONFIG_ESP_HOSTED_SDIO_PIN_D2, CONFIG_ESP_HOSTED_SDIO_PIN_D3,
             CONFIG_ESP_HOSTED_SDIO_GPIO_RESET_SLAVE, CONFIG_ESP_HOSTED_SDIO_CLOCK_FREQ_KHZ);
#endif
    esp_err_t wifi_test_result = device_identity_init();
    if (wifi_test_result == ESP_OK) wifi_test_result = network_manager_init();
    if (wifi_test_result == ESP_OK) wifi_test_result = network_manager_start();
    if (wifi_test_result == ESP_OK) {
        if (network_manager_is_provisioning()) {
            ESP_LOGI("WIFI_TEST", "未保存WiFi凭据，请通过现有BLE配网流程配置；等待120秒");
        } else {
            ESP_LOGI("WIFI_TEST", "使用已保存WiFi凭据，等待DHCP，最多120秒");
        }
        if (!network_manager_wait_connected(120000)) {
            wifi_test_result = ESP_ERR_TIMEOUT;
        }
    }
    ESP_LOGI("WIFI_TEST", "result=%s state=%d connected=%d provisioning=%d",
             esp_err_to_name(wifi_test_result), network_manager_get_state(),
             network_manager_is_connected(), network_manager_is_provisioning());
#if CAMERA_TEST_PROFILE == CAMERA_TEST_WIFI_ONLY
    ESP_LOGI(TAG, "TEST_SUMMARY SD=SKIPPED WiFi=%s (WiFi success means DHCP, not Internet)",
             esp_err_to_name(wifi_test_result));
#else
    ESP_LOGI(TAG, "TEST_SUMMARY SD=%s WiFi=%s (WiFi success means DHCP, not Internet)",
             esp_err_to_name(sd_test_result), esp_err_to_name(wifi_test_result));
#endif
    app_log_memory("WIFI_TEST_DONE");
#else
    ESP_LOGI(TAG, "TEST_SUMMARY SD=%s WiFi=SKIPPED", esp_err_to_name(sd_test_result));
#endif
    return;
#endif

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

    /* WHIP 控制队列先于 MCP 能力注册，避免工具已公布但控制器尚未就绪。
     * 这里只分配队列；PeerConnection、HTTPS 和编码都在收到 start 后执行。 */
    if (webrtc_whip_init() == ESP_OK) {
        webrtc_whip_set_video_control(video_streamer_start, video_streamer_stop,
                                      video_streamer_force_idr);
        webrtc_whip_set_resource_prepare(rtc_prepare_resources);
        webrtc_whip_set_audio_control(xiaozhi_audio_set_rtc_suspended);
#if RTC_CAMERA_ON_DEMAND
        webrtc_whip_set_camera_control(rtc_camera_start, rtc_camera_is_ready);
#endif
    } else {
        ESP_LOGE(TAG, "WHIP 控制器初始化失败");
    }

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
            /* 未绑定时可能没有 MQTT 配置；即使 OTA 后续校验失败也要显示绑定码。 */
            display_driver_set_binding_code(ota_result.has_activation ?
                                            ota_result.activation_code : NULL);

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

#if CAMERA_TEST_PROFILE == CAMERA_TEST_FULL && FULL_POWER_DIAG_HOLD_MS > 0
    full_power_diag_hold("NETWORK_CLOUD_BEFORE_AUDIO");
#endif

#if CAMERA_TEST_PROFILE == CAMERA_TEST_PERIPH_ONLY
    /* 档位 9 只做本地 ES8311/I2S 硬件验证，不创建小智、WakeNet、Opus
     * 或网络会话。测试任务持续输出 MIC 电平并周期播放短测试音。 */
    esp_err_t audio_test_error = xiaozhi_audio_start_hardware_test();
    if (audio_test_error != ESP_OK) {
        ESP_LOGE(TAG, "启动麦克风/扬声器硬件自检失败：%s",
                 esp_err_to_name(audio_test_error));
    }
#elif TP_HAS(XIAOZHI)
    /* 小智初始化板载 ES8311，并注册到 video_streamer 的共享 Agent WSS。
     * 连接仍使用上面 OTA 注入的 URL、Token、Device-Id 和 Client-Id。 */
    esp_err_t xiaozhi_error = xiaozhi_audio_start();
    if (xiaozhi_error != ESP_OK) {
        ESP_LOGE(TAG, "启动小智语音服务失败：%s",
                 esp_err_to_name(xiaozhi_error));
    }
#endif

#if CAMERA_TEST_PROFILE == CAMERA_TEST_FULL && FULL_POWER_DIAG_HOLD_MS > 0
    full_power_diag_hold("AUDIO_BEFORE_UI");
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
#if TP_HAS(WEATHER)
    bool home_info_start_allowed = true;
#endif
#if TP_HAS(SD) && CAMERA_TEST_PROFILE == CAMERA_TEST_FULL
    bool sd_mount_allowed = true;
#endif
#if CAMERA_TEST_PROFILE == CAMERA_TEST_FULL && !RTC_CAMERA_ON_DEMAND
    bool rtc_camera_ready = false;
#endif
#if CAMERA_TEST_PROFILE == CAMERA_TEST_FULL
    /* 完整系统先完成 LVGL 初始化，再启动摄像头；启用 YOLO 时
     * 还要等 UVC 首帧后加载模型，避免与 LVGL 首次分配并发。 */
#if TP_HAS(UI)
    s_ui_ready_events = xEventGroupCreate();
    if (s_ui_ready_events == NULL) {
        ESP_LOGE(TAG, "创建 UI 就绪事件失败，跳过本地人体检测");
        result = xTaskCreatePinnedToCoreWithCaps(ui_task, "ui_task", 8192, NULL, 6,
                                                 NULL, APP_UI_CORE,
                                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        assert(result == pdPASS);
    } else {
        result = xTaskCreatePinnedToCoreWithCaps(ui_task, "ui_task", 8192, NULL, 6,
                                                 NULL, APP_UI_CORE,
                                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
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
#if FULL_POWER_DIAG_HOLD_MS > 0
            full_power_diag_hold("UI_BEFORE_CAMERA");
#endif
#if RTC_CAMERA_ON_DEMAND
            ESP_LOGI(TAG, "YOLO 已关闭，摄像头等待 RTC 预览指令后启动");
#else
            app_log_memory("CAMERA_START");
            ESP_LOGI(TAG, "开机启动 Camera Task，持续统计 UVC 收帧与丢帧（编码仍按需开启）");
            if (camera_driver_prepare_ready_event() != ESP_OK) {
                ESP_LOGE(TAG, "CAMERA_READY 事件初始化失败，跳过本地人体检测");
            } else {
                result = xTaskCreatePinnedToCoreWithCaps(camera_task, "camera_task", 8192,
                                                         NULL, 7, NULL, APP_USB_CORE,
                                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                if (result != pdPASS) {
                    ESP_LOGE(TAG, "创建 Camera Task 失败，跳过本地人体检测");
                } else {
                    ESP_LOGI(TAG, "等待 CAMERA_READY（UVC stream open + 第一帧有效 MJPEG）");
                    if (!camera_driver_wait_ready(60000)) {
                        ESP_LOGE(TAG, "等待 CAMERA_READY 超时，摄像头视频链路未就绪");
#if CAMERA_PERSON_DETECT_ENABLED
                    } else if (person_detect_init() != ESP_OK) {
                        ESP_LOGW(TAG, "本地人体检测初始化失败，继续运行其它模块");
                    } else {
                        /* 不让定位 HTTPS 与 YOLO 模型解包争抢瞬时内部 RAM。
                         * 失败已结束加载，可以继续天气；超时则跳过本次天气
                         * 启动，避免重新引入并发分配。 */
                        ESP_LOGI(TAG, "等待 YOLO 模型加载完成后启动天气定位");
                        const esp_err_t ai_ready = person_detect_wait_startup(30000);
                        if (ai_ready == ESP_ERR_TIMEOUT) {
                            ESP_LOGE(TAG, "等待 YOLO 加载超时，跳过本次天气定位启动");
                            home_info_start_allowed = false;
#if TP_HAS(SD)
                            /* 加载仍可能占用 Flash/内存，不能并发挂载 SDMMC。 */
                            sd_mount_allowed = false;
#endif
                        } else if (ai_ready != ESP_OK) {
                            ESP_LOGW(TAG, "YOLO 初始化失败，继续启动天气定位");
                        } else {
                            ESP_LOGI(TAG, "YOLO 模型就绪，启动天气定位");
                        }
#else
                    } else {
                        ESP_LOGW(TAG, "视频链路隔离测试：YOLO 已关闭，不加载模型或抽帧");
#endif
                    }
                    /* 只有 stream open 且收到首帧后 wait_ready 才返回 true。
                     * 先记录 Camera 条件；WHIP 仍需继续等 SD 初始化结束。 */
                    rtc_camera_ready = camera_driver_wait_ready(0);
                    /* 首帧与断线通知均由 Camera Task 统一发送，避免此处在
                     * 断线清除就绪后再次写入过期的 CAMERA_READY。 */
                }
            }
#endif
            vEventGroupDelete(s_ui_ready_events);
            s_ui_ready_events = NULL;
        }
    }
#else
#if CAMERA_PERSON_DETECT_ENABLED
    if (person_detect_init() != ESP_OK) {
        ESP_LOGW(TAG, "本地人体检测初始化失败，继续运行其它模块");
    }
#endif
#endif
#endif

#if TP_HAS(SD) && CAMERA_TEST_PROFILE == CAMERA_TEST_FULL
    /* YOLO 启用时须等模型加载结束；隔离测试不加载模型，可直接挂载 TF 卡。
     * 表情管理器已就绪，挂载失败时仍可
     * 保持 HOME；后续事件播放失败会按现有逻辑退回 HOME。 */
    if (sd_mount_allowed) {
#if CAMERA_PERSON_DETECT_ENABLED
        ESP_LOGI(TAG, "YOLO 加载阶段结束，开始挂载 TF 卡表情资源");
#else
        ESP_LOGI(TAG, "YOLO 已关闭，开始挂载 TF 卡表情资源");
#endif
        const esp_err_t sd_error = sd_card_mount();
        if (sd_error != ESP_OK) {
            ESP_LOGW(TAG, "TF 卡挂载失败，表情资源暂不可用：%s",
                     esp_err_to_name(sd_error));
        }
        app_log_memory("SD_MOUNTED");
    } else {
        ESP_LOGW(TAG, "YOLO 加载仍未完成，本次跳过 TF 卡挂载");
    }
#endif

#if CAMERA_TEST_PROFILE == CAMERA_TEST_FULL
    /* 无论挂载成功、失败还是当前档位不含 SD，到这里都表示 SD 初始化阶段
     * 已经结束。WHIP 只有同时看到 CAMERA_READY 才会真正创建 PeerConnection。 */
    webrtc_whip_notify_storage_init_done();
#if RTC_CAMERA_ON_DEMAND
    ESP_LOGI(TAG, "RTC 摄像头按需启动：等待 media.webrtc.start");
#else
    if (!rtc_camera_ready) {
        ESP_LOGW(TAG, "RTC 前置条件：SD 初始化已结束，但 CAMERA_READY 尚未成立");
    }
#endif
#endif

#if CAMERA_TEST_PROFILE == CAMERA_TEST_FULL
    /* 拍照模块只复用已经运行的 UVC 流。初始化失败不会影响摄像头、音频或
     * WebRTC；MCP 调用会明确返回 PHOTO_NOT_READY。 */
    const esp_err_t photo_error = camera_photo_init();
    if (photo_error != ESP_OK) {
        ESP_LOGW(TAG, "拍照模块初始化失败：%s", esp_err_to_name(photo_error));
    }
#endif

#if TP_HAS(WEATHER)
    /* 首页信息服务独立等待网络；模型加载结束后才允许创建 HTTPS 任务。 */
    if (home_info_start_allowed) {
        esp_err_t home_error = home_info_start();
        if (home_error != ESP_OK) {
            ESP_LOGE(TAG, "首页信息服务启动失败：%s", esp_err_to_name(home_error));
        }
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
    result = xTaskCreatePinnedToCoreWithCaps(ui_task, "ui_task", 8192, NULL, 6,
                                             NULL, APP_UI_CORE,
                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    assert(result == pdPASS);
#endif

#if CAMERA_TEST_PROFILE != CAMERA_TEST_FULL && TP_HAS(UVC)
    result = xTaskCreatePinnedToCoreWithCaps(camera_task, "camera_task", 8192, NULL, 7,
                                             NULL, APP_USB_CORE,
                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    assert(result == pdPASS);
#elif CAMERA_TEST_PROFILE != CAMERA_TEST_FULL
    ESP_LOGI(TAG, "测试档位=%d（%s）：已停用 Camera Task 与 USB/UVC Host",
             CAMERA_TEST_PROFILE, test_profile_name());
#endif

    ESP_LOGI(TAG, "测试档位=%d（%s）：已完成所选任务启动",
             CAMERA_TEST_PROFILE, test_profile_name());
    app_log_memory_budget();

    if (xTaskCreatePinnedToCoreWithCaps(idle_stable_diag_task,
                                        "idle_stable_diag", 3072, NULL, 2,
                                        NULL, APP_UI_CORE,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGW(TAG, "创建 IDLE_STABLE 内存诊断任务失败");
    }
}
