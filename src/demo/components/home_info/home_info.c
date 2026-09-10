#include "home_info.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "network_manager.h"

static const char *TAG = "HOME_INFO";

#define HOME_INFO_TASK_STACK        8192
#define HOME_INFO_TASK_PRIORITY     5
#define HOME_INFO_HTTP_TIMEOUT_MS   12000
#define HOME_INFO_RESPONSE_BYTES    4096
#define HOME_INFO_RETRY_SECONDS     60
#define HOME_INFO_WEATHER_SECONDS   (30 * 60)
#define HOME_INFO_LOCATION_SECONDS  (6 * 60 * 60)

/* IP 定位不需要 API Key，用于自动取得经纬度和时区。天气接口同样不需要 Key。 */
#define HOME_INFO_LOCATION_URL \
    "https://ipwho.is/?fields=success,city,latitude,longitude,timezone"
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
    return ESP_OK;
}

static esp_err_t http_get_json(const char *url, http_response_buffer_t *response)
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
        ESP_LOGW(TAG, "HTTPS 请求失败：%s", esp_err_to_name(err));
        return err;
    }
    if (status != 200 || response->overflow || response->length == 0) {
        ESP_LOGW(TAG, "API 响应异常：HTTP=%d，长度=%u，溢出=%d",
                 status, (unsigned)response->length, response->overflow);
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
    esp_err_t err = http_get_json(HOME_INFO_LOCATION_URL, &response);
    if (err != ESP_OK) {
        return err;
    }

    cJSON *root = cJSON_Parse(response.data);
    if (root == NULL) {
        ESP_LOGW(TAG, "定位 API 返回的 JSON 无法解析");
        return ESP_ERR_INVALID_RESPONSE;
    }

    const cJSON *success = cJSON_GetObjectItemCaseSensitive(root, "success");
    double lat = 0.0;
    double lon = 0.0;
    bool valid = cJSON_IsTrue(success) &&
                 json_number(root, "latitude", &lat) &&
                 json_number(root, "longitude", &lon);

    const cJSON *timezone = cJSON_GetObjectItemCaseSensitive(root, "timezone");
    double offset = 0.0;
    if (valid && cJSON_IsObject(timezone) && json_number(timezone, "offset", &offset)) {
        xSemaphoreTake(s_snapshot_mutex, portMAX_DELAY);
        s_timezone_offset_seconds = (int32_t)offset;
        s_timezone_valid = true;
        xSemaphoreGive(s_snapshot_mutex);
    } else {
        valid = false;
    }

    if (valid) {
        *latitude = lat;
        *longitude = lon;
        copy_city_name(root);
    }
    cJSON_Delete(root);

    if (!valid) {
        ESP_LOGW(TAG, "定位 API 缺少经纬度或时区字段");
        return ESP_ERR_INVALID_RESPONSE;
    }

    ESP_LOGI(TAG, "自动定位成功：%s，坐标 %.4f, %.4f，UTC 偏移 %ld 秒",
             s_snapshot.city[0] != '\0' ? s_snapshot.city : "未知城市",
             lat, lon, (long)s_timezone_offset_seconds);
    return ESP_OK;
}

static esp_err_t fetch_weather(double latitude, double longitude)
{
    char url[320];
    snprintf(url, sizeof(url), HOME_INFO_WEATHER_URL, latitude, longitude);

    http_response_buffer_t response;
    esp_err_t err = http_get_json(url, &response);
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

static bool synchronize_clock(void)
{
    static bool initialized;
    if (!initialized) {
        /* 阿里云 NTP 对国内网络更友好，pool.ntp.org 作为备用。 */
        esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(
            2, ESP_SNTP_SERVER_LIST("ntp.aliyun.com", "pool.ntp.org"));
        esp_err_t err = esp_netif_sntp_init(&config);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "启动网络校时失败：%s", esp_err_to_name(err));
            return false;
        }
        initialized = true;
    }

    if (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(15000)) != ESP_OK) {
        ESP_LOGW(TAG, "网络校时超时，稍后重试");
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

    while (true) {
        if (!network_manager_wait_connected(30000)) {
            ESP_LOGW(TAG, "等待 WiFi 联网后获取时间和天气");
            continue;
        }

        if (!s_clock_synced) {
            const bool synced = synchronize_clock();
            xSemaphoreTake(s_snapshot_mutex, portMAX_DELAY);
            s_clock_synced = synced;
            xSemaphoreGive(s_snapshot_mutex);
        }

        const TickType_t now = xTaskGetTickCount();
        const bool location_due = !location_valid ||
            (now - last_location_tick) >= pdMS_TO_TICKS(HOME_INFO_LOCATION_SECONDS * 1000U);
        if (location_due) {
            if (fetch_location(&latitude, &longitude) == ESP_OK) {
                location_valid = true;
                last_location_tick = now;
            } else {
                vTaskDelay(pdMS_TO_TICKS(HOME_INFO_RETRY_SECONDS * 1000U));
                continue;
            }
        }

        const bool weather_due = last_weather_tick == 0 ||
            (now - last_weather_tick) >= pdMS_TO_TICKS(HOME_INFO_WEATHER_SECONDS * 1000U);
        if (weather_due) {
            if (fetch_weather(latitude, longitude) == ESP_OK) {
                last_weather_tick = now;
            }
        }

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

    BaseType_t result = xTaskCreate(home_info_task, "home_info", HOME_INFO_TASK_STACK,
                                    NULL, HOME_INFO_TASK_PRIORITY, NULL);
    if (result != pdPASS) {
        vSemaphoreDelete(s_snapshot_mutex);
        s_snapshot_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_started = true;
    ESP_LOGI(TAG, "首页时间与天气服务已启动");
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
