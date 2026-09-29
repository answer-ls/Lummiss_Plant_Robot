#include "home_info.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "network_manager.h"
#include "time_service.h"

static const char *TAG = "HOME_INFO";

#define HOME_INFO_TASK_STACK        8192
#define HOME_INFO_TASK_PRIORITY     0
#define HOME_INFO_TASK_CORE         0
#define HOME_INFO_HTTP_TIMEOUT_MS   12000
#define HOME_INFO_RESPONSE_BYTES    4096
#define HOME_INFO_RETRY_SECONDS     60
#define HOME_INFO_WEATHER_SECONDS   (30 * 60)
#define HOME_INFO_LOCATION_SECONDS  (6 * 60 * 60)
#define HOME_INFO_RETRY_MIN_SECONDS 5U
#define HOME_INFO_RETRY_MAX_SECONDS 60U

/* 首页联网后启用 IP 定位与 Open-Meteo 天气查询。 */
#define HOME_INFO_REMOTE_HTTP_ENABLED 1

/* 默认时区：中国全境统一 UTC+8 且不使用夏令时。
 * 时钟只在 NTP 校时成功后就该显示，不能再等第三方定位 / 天气接口——
 * 否则接口一旦不通，屏幕会一直停在 "--:--"。定位或天气返回真实偏移后再覆盖它。 */
#define HOME_INFO_DEFAULT_UTC_OFFSET_SECONDS  (8 * 3600)

/* IP 定位使用可通过 IPv4 访问的公开接口；天气接口无需 API Key。 */
#define HOME_INFO_LOCATION_URL "https://ipinfo.io/json"
#define HOME_INFO_WEATHER_URL \
    "https://api.open-meteo.com/v1/forecast?latitude=%.6f&longitude=%.6f" \
    "&current=temperature_2m,weather_code&timezone=auto&forecast_days=1"

typedef struct {
    char data[HOME_INFO_RESPONSE_BYTES];
    size_t length;
    bool overflow;
} http_response_buffer_t;

static SemaphoreHandle_t s_snapshot_mutex;
static home_info_snapshot_t s_snapshot;
static int32_t s_timezone_offset_seconds;
static bool s_timezone_valid;
static bool s_clock_synced;
static bool s_started;

static esp_err_t http_event_handler(esp_http_client_event_t *event)
{
    http_response_buffer_t *response = event->user_data;
    if (event->event_id != HTTP_EVENT_ON_DATA || response == NULL || event->data_len <= 0) {
        return ESP_OK;
    }

    const size_t remaining = sizeof(response->data) - response->length - 1;
    const size_t copy_size = (size_t)event->data_len < remaining
                                 ? (size_t)event->data_len
                                 : remaining;
    if (copy_size > 0) {
        memcpy(response->data + response->length, event->data, copy_size);
        response->length += copy_size;
        response->data[response->length] = '\0';
    }
    if (copy_size != (size_t)event->data_len) {
        response->overflow = true;
    }
    /* 响应数据较大时主动给网络任务让出调度点，避免连续回调长期占用 CPU。 */
    taskYIELD();
    return ESP_OK;
}

/* label 只用于日志，标明是哪一路接口失败，方便对着串口判断。 */
static esp_err_t http_get_json(const char *label, const char *url,
                               http_response_buffer_t *response)
{
    memset(response, 0, sizeof(*response));
    const esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event_handler,
        .user_data = response,
        .timeout_ms = HOME_INFO_HTTP_TIMEOUT_MS,
        .buffer_size = 2048,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = true,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = esp_http_client_perform(client);
    const int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s接口请求失败：%s", label, esp_err_to_name(err));
        /* TLS 建连失败时记录连续内存，区分网络故障和本地内存不足。 */
        ESP_LOGW(TAG,
                 "HTTP_MEM DMA=%u/%u INT=%u/%u PSRAM=%u/%u bytes",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
        return err;
    }
    if (status != 200 || response->overflow || response->length == 0) {
        ESP_LOGW(TAG, "%s接口响应异常：HTTP=%d，长度=%u，溢出=%d",
                 label, status, (unsigned)response->length, response->overflow);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static bool json_number(const cJSON *object, const char *name, double *value)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    if (!cJSON_IsNumber(item)) {
        return false;
    }
    *value = item->valuedouble;
    return true;
}

static void copy_city_name(const cJSON *root)
{
    const cJSON *city = cJSON_GetObjectItemCaseSensitive(root, "city");
    if (!cJSON_IsString(city) || city->valuestring == NULL) {
        return;
    }

    xSemaphoreTake(s_snapshot_mutex, portMAX_DELAY);
    strlcpy(s_snapshot.city, city->valuestring, sizeof(s_snapshot.city));
    xSemaphoreGive(s_snapshot_mutex);
}

static esp_err_t fetch_location(double *latitude, double *longitude)
{
    http_response_buffer_t response;
    esp_err_t err = http_get_json("定位", HOME_INFO_LOCATION_URL, &response);
    if (err != ESP_OK) {
        return err;
    }

    cJSON *root = cJSON_Parse(response.data);
    if (root == NULL) {
        ESP_LOGW(TAG, "定位 API 返回的 JSON 无法解析");
        return ESP_ERR_INVALID_RESPONSE;
    }

    const cJSON *location = cJSON_GetObjectItemCaseSensitive(root, "loc");
    double lat = 0.0;
    double lon = 0.0;
    const bool valid = cJSON_IsString(location) && location->valuestring != NULL &&
                       sscanf(location->valuestring, "%lf,%lf", &lat, &lon) == 2 &&
                       lat >= -90.0 && lat <= 90.0 && lon >= -180.0 && lon <= 180.0;

    if (valid) {
        *latitude = lat;
        *longitude = lon;
        copy_city_name(root);
    }
    cJSON_Delete(root);

    if (!valid) {
        ESP_LOGW(TAG, "定位 API 缺少有效坐标");
        return ESP_ERR_INVALID_RESPONSE;
    }

    ESP_LOGI(TAG, "自动定位成功：%s，坐标 %.4f, %.4f；时区等待天气响应确认",
             s_snapshot.city[0] != '\0' ? s_snapshot.city : "未知城市",
             lat, lon);
    return ESP_OK;
}

static esp_err_t fetch_weather(double latitude, double longitude)
{
    char url[320];
    snprintf(url, sizeof(url), HOME_INFO_WEATHER_URL, latitude, longitude);

    http_response_buffer_t response;
    esp_err_t err = http_get_json("天气", url, &response);
    if (err != ESP_OK) {
        return err;
    }

    cJSON *root = cJSON_Parse(response.data);
    if (root == NULL) {
        ESP_LOGW(TAG, "天气 API 返回的 JSON 无法解析");
        return ESP_ERR_INVALID_RESPONSE;
    }

    const cJSON *current = cJSON_GetObjectItemCaseSensitive(root, "current");
    double temperature = 0.0;
    double weather_code = 0.0;
    double utc_offset = 0.0;
    const bool valid = cJSON_IsObject(current) &&
                       json_number(current, "temperature_2m", &temperature) &&
                       json_number(current, "weather_code", &weather_code) &&
                       json_number(root, "utc_offset_seconds", &utc_offset);
    if (valid) {
        xSemaphoreTake(s_snapshot_mutex, portMAX_DELAY);
        s_snapshot.temperature_c = (float)temperature;
        s_snapshot.weather_code = (int)weather_code;
        s_snapshot.weather_valid = true;
        s_timezone_offset_seconds = (int32_t)utc_offset;
        s_timezone_valid = true;
        xSemaphoreGive(s_snapshot_mutex);
    }
    cJSON_Delete(root);

    if (!valid) {
        ESP_LOGW(TAG, "天气 API 缺少当前温度或天气码");
        return ESP_ERR_INVALID_RESPONSE;
    }

    ESP_LOGI(TAG, "天气更新：%s %.1f°C（WMO=%d）",
             home_info_weather_text((int)weather_code), temperature, (int)weather_code);
    return ESP_OK;
}

/* 只**等待**时间同步完成，绝不在这里初始化 SNTP —— 那是 time_service 的唯一职责。
 *
 * 之前这里自己调 esp_netif_sntp_init + esp_netif_sntp_sync_wait，结果两件事一起发生：
 *   1) init 报 "esp_netif_sntp already initialized"（app_main 已经初始化过）；
 *   2) sync_wait 内部是 xQueueSemaphoreTake 一个**二值信号量**，只可能被消费一次
 *      —— app_main 已经取走了，本函数于是白等满 15 s 再报"网络校时超时"，
 *      而系统时间其实早就同步好了。
 * 现在时间同步状态由 time_service 用粘性事件位广播，谁都可以反复问。 */
static bool synchronize_clock(void)
{
    if (!time_service_wait_synced(15000)) {
        ESP_LOGW(TAG, "等待网络校时超时，稍后重试");
        return false;
    }
    ESP_LOGI(TAG, "网络时间同步成功");
    return true;
}

static void home_info_task(void *arg)
{
    (void)arg;
    double latitude = 0.0;
    double longitude = 0.0;
    bool location_valid = false;
    TickType_t last_location_tick = 0;
    TickType_t last_weather_tick = 0;
    uint32_t failure_backoff_seconds = HOME_INFO_RETRY_MIN_SECONDS;

    while (true) {
        if (!network_manager_wait_connected(30000)) {
            ESP_LOGW(TAG, "等待 WiFi 联网后获取时间和天气");
            /* 网络不可用时也必须退避，不能快速 continue 形成忙等。 */
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        if (!s_clock_synced) {
            const bool synced = synchronize_clock();
            xSemaphoreTake(s_snapshot_mutex, portMAX_DELAY);
            s_clock_synced = synced;
            xSemaphoreGive(s_snapshot_mutex);
        }

        /* WebRTC 联调期间不创建定位/天气 TLS 连接，避免它与 ICE/DTLS/WHIP
         * 同时争用已经很紧张的 INTERNAL/DMA 内存。这里只暂停远端请求，
         * home_info_get_snapshot() 仍会基于 NTP 和默认 UTC+8 生成本地时间。 */
        if (!HOME_INFO_REMOTE_HTTP_ENABLED) {
            vTaskDelay(pdMS_TO_TICKS(HOME_INFO_RETRY_SECONDS * 1000U));
            continue;
        }

        const TickType_t now = xTaskGetTickCount();
        const bool location_due = !location_valid ||
            (now - last_location_tick) >= pdMS_TO_TICKS(HOME_INFO_LOCATION_SECONDS * 1000U);
        if (location_due) {
            if (fetch_location(&latitude, &longitude) == ESP_OK) {
                location_valid = true;
                last_location_tick = now;
                failure_backoff_seconds = HOME_INFO_RETRY_MIN_SECONDS;
            } else {
                ESP_LOGW(TAG, "定位失败，%u 秒后退避重试",
                         (unsigned)failure_backoff_seconds);
                vTaskDelay(pdMS_TO_TICKS(failure_backoff_seconds * 1000U));
                if (failure_backoff_seconds < HOME_INFO_RETRY_MAX_SECONDS) {
                    failure_backoff_seconds *= 2U;
                    if (failure_backoff_seconds > HOME_INFO_RETRY_MAX_SECONDS) {
                        failure_backoff_seconds = HOME_INFO_RETRY_MAX_SECONDS;
                    }
                }
                continue;
            }
        }

        const bool weather_due = last_weather_tick == 0 ||
            (now - last_weather_tick) >= pdMS_TO_TICKS(HOME_INFO_WEATHER_SECONDS * 1000U);
        if (weather_due) {
            if (fetch_weather(latitude, longitude) == ESP_OK) {
                last_weather_tick = now;
                failure_backoff_seconds = HOME_INFO_RETRY_MIN_SECONDS;
            } else {
                ESP_LOGW(TAG, "天气失败，%u 秒后退避重试",
                         (unsigned)failure_backoff_seconds);
                vTaskDelay(pdMS_TO_TICKS(failure_backoff_seconds * 1000U));
                if (failure_backoff_seconds < HOME_INFO_RETRY_MAX_SECONDS) {
                    failure_backoff_seconds *= 2U;
                    if (failure_backoff_seconds > HOME_INFO_RETRY_MAX_SECONDS) {
                        failure_backoff_seconds = HOME_INFO_RETRY_MAX_SECONDS;
                    }
                }
                continue;
            }
        }

        /* 定位和天气成功后进入低频轮询，任务不会空转。 */
        vTaskDelay(pdMS_TO_TICKS(HOME_INFO_RETRY_SECONDS * 1000U));
    }
}

esp_err_t home_info_start(void)
{
    if (s_started) {
        return ESP_OK;
    }

    s_snapshot_mutex = xSemaphoreCreateMutex();
    if (s_snapshot_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }

    /* 时钟先按默认时区（UTC+8）成立：NTP 一成功就能出时间，
     * 不再把"显示时间"绑死在 ipwho.is 这类第三方接口上。
     * 这里的写入发生在任务创建之前，无需加锁。 */
    s_timezone_offset_seconds = HOME_INFO_DEFAULT_UTC_OFFSET_SECONDS;
    s_timezone_valid = true;
    ESP_LOGI(TAG, "默认时区 UTC+8（中国），定位成功后按实际时区修正");

    /* 天气/定位属于最低优先级后台任务，固定在 CPU0。使用与 IDLE 同级的
     * 优先级，让 CPU0 空闲任务在 TLS 大数运算期间仍能获得时间片，避免
     * 任务看门狗只看到 IDLE0 长时间无法运行。请求完成后任务始终进入延时。 */
    /* 时间/HTTP 控制任务不向硬件提交栈地址，栈迁到 PSRAM。 */
    BaseType_t result = xTaskCreatePinnedToCoreWithCaps(
        home_info_task, "home_info", HOME_INFO_TASK_STACK, NULL,
        HOME_INFO_TASK_PRIORITY, NULL, HOME_INFO_TASK_CORE,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (result != pdPASS) {
        vSemaphoreDelete(s_snapshot_mutex);
        s_snapshot_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_started = true;
    if (HOME_INFO_REMOTE_HTTP_ENABLED) {
        ESP_LOGI(TAG, "首页时间与天气服务已启动");
    } else {
        ESP_LOGW(TAG, "RTC 联调模式：定位/天气 HTTPS 已暂停，仅保留首页本地时间");
    }
    return ESP_OK;
}

bool home_info_get_snapshot(home_info_snapshot_t *snapshot)
{
    if (snapshot == NULL || s_snapshot_mutex == NULL) {
        return false;
    }

    xSemaphoreTake(s_snapshot_mutex, portMAX_DELAY);
    *snapshot = s_snapshot;
    snapshot->time_valid = s_clock_synced && s_timezone_valid;
    const int32_t offset = s_timezone_offset_seconds;
    xSemaphoreGive(s_snapshot_mutex);

    if (snapshot->time_valid) {
        time_t local_time = time(NULL) + offset;
        struct tm value;
        gmtime_r(&local_time, &value);
        snapshot->year = value.tm_year + 1900;
        snapshot->month = value.tm_mon + 1;
        snapshot->day = value.tm_mday;
        snapshot->weekday = value.tm_wday;
        snapshot->hour = value.tm_hour;
        snapshot->minute = value.tm_min;
    }
    return true;
}

const char *home_info_weather_text(int weather_code)
{
    if (weather_code == 0) return "晴";
    if (weather_code <= 2) return "少云";
    if (weather_code == 3) return "阴";
    if (weather_code == 45 || weather_code == 48) return "雾";
    if (weather_code >= 51 && weather_code <= 57) return "毛毛雨";
    if (weather_code >= 61 && weather_code <= 67) return "雨";
    if (weather_code >= 71 && weather_code <= 77) return "雪";
    if (weather_code >= 80 && weather_code <= 82) return "阵雨";
    if (weather_code == 85 || weather_code == 86) return "阵雪";
    if (weather_code >= 95 && weather_code <= 99) return "雷雨";
    return "未知";
}
