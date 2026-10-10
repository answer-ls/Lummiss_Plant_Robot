#include "alert_clip.h"

#include <inttypes.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sd_card.h"

#define ALERT_PRE_US          10000000LL
#define ALERT_POST_US         10000000LL
#define ALERT_RING_US         11000000LL
#define ALERT_RING_MAX_BYTES  (4U * 1024U * 1024U)
#define ALERT_CLIP_MAX_BYTES  (8U * 1024U * 1024U)
#define ALERT_RING_FRAMES     256U
#define ALERT_CLIP_FRAMES     512U
#define ALERT_UPLOAD_CHUNK    4096U

typedef struct {
    uint8_t *data;
    size_t len;
    int64_t time_us;
    bool idr;
} alert_frame_t;

typedef enum {
    ALERT_IDLE,
    ALERT_RECORDING,
    ALERT_SAVING,
    ALERT_WAIT_GRANT,
    ALERT_UPLOADING,
} alert_phase_t;

static const char *TAG = "ALERT_CLIP";
static const char *const k_boundary = "----LummissAlertBoundary";
static SemaphoreHandle_t s_lock;
static alert_frame_t *s_ring;
static alert_frame_t *s_clip;
static size_t s_ring_count;
static size_t s_ring_bytes;
static size_t s_clip_count;
static size_t s_clip_bytes;
static alert_phase_t s_phase;
static int64_t s_trigger_us;
static int64_t s_grant_deadline_us;
static int64_t s_first_clip_us;
static int64_t s_last_clip_us;
static size_t s_max_bytes;
static char s_event_id[33];
static char s_alert_id[33];
static char s_ticket[160];
static char s_file_path[80];
static bool s_granted;
static int64_t s_last_not_ready_log_us;
static uint8_t s_header_prefix[1024];
static size_t s_header_prefix_len;

static size_t nal_start_code(const uint8_t *data, size_t len, size_t offset)
{
    if (offset + 3 >= len || data[offset] != 0 || data[offset + 1] != 0) return 0;
    if (data[offset + 2] == 1) return 3;
    return data[offset + 2] == 0 && data[offset + 3] == 1 ? 4 : 0;
}

static bool frame_has_decoder_headers(const alert_frame_t *frame)
{
    bool sps = false, pps = false, idr = false;
    const uint8_t *data = frame->data;
    for (size_t i = 0; i + 4 < frame->len; ++i) {
        const size_t prefix = nal_start_code(data, frame->len, i);
        if (prefix == 0 || i + prefix >= frame->len) continue;
        const uint8_t nal = data[i + prefix] & 0x1f;
        sps |= nal == 7;
        pps |= nal == 8;
        idr |= nal == 5;
    }
    return sps && pps && idr;
}

static void remember_decoder_headers(const alert_frame_t *frame)
{
    size_t sps_at = SIZE_MAX;
    size_t idr_at = SIZE_MAX;
    bool pps = false;
    for (size_t i = 0; i + 4 < frame->len; ++i) {
        const size_t prefix = nal_start_code(frame->data, frame->len, i);
        if (prefix == 0 || i + prefix >= frame->len) continue;
        const uint8_t nal = frame->data[i + prefix] & 0x1f;
        if (nal == 7 && sps_at == SIZE_MAX) sps_at = i;
        if (nal == 8 && sps_at != SIZE_MAX) pps = true;
        if (nal == 5) { idr_at = i; break; }
    }
    if (sps_at != SIZE_MAX && pps && idr_at > sps_at &&
        idr_at - sps_at <= sizeof(s_header_prefix)) {
        s_header_prefix_len = idr_at - sps_at;
        memcpy(s_header_prefix, frame->data + sps_at, s_header_prefix_len);
    }
}

static void drop_oldest_ring(void)
{
    if (s_ring_count == 0) return;
    s_ring_bytes -= s_ring[0].len;
    heap_caps_free(s_ring[0].data);
    --s_ring_count;
    memmove(s_ring, s_ring + 1, s_ring_count * sizeof(*s_ring));
}

static void clear_clip(void)
{
    for (size_t i = 0; i < s_clip_count; ++i) {
        heap_caps_free(s_clip[i].data);
    }
    s_clip_count = 0;
    s_clip_bytes = 0;
}

static void reset_job(void)
{
    clear_clip();
    s_phase = ALERT_IDLE;
    s_trigger_us = 0;
    s_grant_deadline_us = 0;
    s_granted = false;
    s_first_clip_us = 0;
    s_last_clip_us = 0;
    s_max_bytes = 0;
    memset(s_ticket, 0, sizeof(s_ticket));
    s_event_id[0] = '\0';
    s_alert_id[0] = '\0';
    s_file_path[0] = '\0';
}

void alert_clip_push_h264(const uint8_t *data, size_t len, bool idr)
{
    if (s_lock == NULL || data == NULL || len == 0 || len > 128U * 1024U) return;
    uint8_t *copy = heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (copy == NULL) {
        ESP_LOGW(TAG, "PSRAM 帧分配失败：%u bytes", (unsigned)len);
        return;
    }
    memcpy(copy, data, len);
    const alert_frame_t frame = {copy, len, esp_timer_get_time(), idr};
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(5)) != pdTRUE) {
        heap_caps_free(copy);
        return;
    }
    if (idr) remember_decoder_headers(&frame);

    if (s_phase == ALERT_RECORDING && frame.time_us <= s_trigger_us + ALERT_POST_US) {
        if (s_clip_count < ALERT_CLIP_FRAMES && s_clip_bytes + len <= ALERT_CLIP_MAX_BYTES) {
            s_clip[s_clip_count++] = frame;
            s_clip_bytes += len;
            s_last_clip_us = frame.time_us;
            xSemaphoreGive(s_lock);
            return;
        }
        ESP_LOGE(TAG, "片段超过帧数或 8 MiB 上限，放弃 event_id=%s", s_event_id);
        reset_job();
    }

    while (s_ring_count > 0 &&
           (s_ring_count >= ALERT_RING_FRAMES ||
            s_ring_bytes + len > ALERT_RING_MAX_BYTES ||
            frame.time_us - s_ring[0].time_us > ALERT_RING_US)) {
        drop_oldest_ring();
    }
    if (s_ring_count < ALERT_RING_FRAMES && s_ring_bytes + len <= ALERT_RING_MAX_BYTES) {
        s_ring[s_ring_count++] = frame;
        s_ring_bytes += len;
    } else {
        heap_caps_free(copy);
    }
    xSemaphoreGive(s_lock);
}

bool alert_clip_trigger(const char *event_id)
{
    if (s_lock == NULL || !sd_card_is_mounted() ||
        event_id == NULL || strlen(event_id) != 32 ||
        xSemaphoreTake(s_lock, pdMS_TO_TICKS(20)) != pdTRUE) return false;
    const int64_t now = esp_timer_get_time();
    if (s_phase != ALERT_IDLE || s_ring_count == 0 ||
        now - s_ring[0].time_us < ALERT_PRE_US) {
        if (now - s_last_not_ready_log_us >= 5000000LL) {
            s_last_not_ready_log_us = now;
            ESP_LOGI(TAG, "RING_WAIT phase=%d frames=%u bytes=%u history_ms=%" PRId64,
                     (int)s_phase, (unsigned)s_ring_count, (unsigned)s_ring_bytes,
                     s_ring_count > 0 ? (now - s_ring[0].time_us) / 1000 : 0);
        }
        xSemaphoreGive(s_lock);
        return false;
    }

    /* 从触发前十秒附近的 IDR 开始，优先选边界之前最近的一帧。 */
    const int64_t wanted = now - ALERT_PRE_US;
    size_t first = SIZE_MAX;
    for (size_t i = 0; i < s_ring_count; ++i) {
        if (s_ring[i].idr && s_ring[i].time_us <= wanted) first = i;
    }
    if (first == SIZE_MAX || s_ring_count - first > ALERT_CLIP_FRAMES) {
        xSemaphoreGive(s_lock);
        ESP_LOGW(TAG, "前十秒边界前没有可用 IDR，暂不上报预警");
        return false;
    }
    size_t bytes = 0;
    for (size_t i = first; i < s_ring_count; ++i) bytes += s_ring[i].len;
    if (!frame_has_decoder_headers(&s_ring[first])) {
        if (s_header_prefix_len == 0) {
            xSemaphoreGive(s_lock);
            ESP_LOGW(TAG, "没有 SPS/PPS，暂不上报预警");
            return false;
        }
        bytes += s_header_prefix_len;
    }
    if (bytes >= ALERT_CLIP_MAX_BYTES) {
        xSemaphoreGive(s_lock);
        ESP_LOGW(TAG, "触发前片段已超过 8 MiB，暂不上报预警");
        return false;
    }
    if (!frame_has_decoder_headers(&s_ring[first])) {
        uint8_t *prefixed = heap_caps_malloc(s_ring[first].len + s_header_prefix_len,
                                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (prefixed == NULL) {
            xSemaphoreGive(s_lock);
            return false;
        }
        memcpy(prefixed, s_header_prefix, s_header_prefix_len);
        memcpy(prefixed + s_header_prefix_len, s_ring[first].data, s_ring[first].len);
        heap_caps_free(s_ring[first].data);
        s_ring[first].data = prefixed;
        s_ring[first].len += s_header_prefix_len;
    }
    for (size_t i = 0; i < first; ++i) heap_caps_free(s_ring[i].data);
    s_clip_count = s_ring_count - first;
    memcpy(s_clip, s_ring + first, s_clip_count * sizeof(*s_clip));
    s_clip_bytes = bytes;
    s_first_clip_us = s_clip[0].time_us;
    s_last_clip_us = s_clip[s_clip_count - 1].time_us;
    s_ring_count = 0;
    s_ring_bytes = 0;
    memcpy(s_event_id, event_id, 33);
    s_trigger_us = now;
    s_phase = ALERT_RECORDING;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "CAPTURE_BEGIN event_id=%s pre_ms=%" PRId64 " pre_bytes=%u",
             event_id, (now - s_first_clip_us) / 1000, (unsigned)bytes);
    return true;
}

void alert_clip_cancel(const char *event_id)
{
    if (s_lock == NULL || event_id == NULL ||
        xSemaphoreTake(s_lock, pdMS_TO_TICKS(20)) != pdTRUE) return;
    if (s_phase == ALERT_RECORDING && strcmp(s_event_id, event_id) == 0) {
        reset_job();
        ESP_LOGW(TAG, "MQTT 上报失败，已取消片段：%s", event_id);
    }
    xSemaphoreGive(s_lock);
}

bool alert_clip_set_grant(const char *event_id, const char *alert_id,
                          const char *ticket, const char *upload_path,
                          uint32_t expires_in, size_t max_bytes)
{
    if (s_lock == NULL || event_id == NULL || alert_id == NULL || ticket == NULL ||
        upload_path == NULL || strlen(alert_id) != 32 ||
        strlen(ticket) >= sizeof(s_ticket) ||
        strcmp(upload_path, "/device-api/v1/device-alerts") != 0 ||
        expires_in == 0 || expires_in > 3600 || max_bytes == 0 ||
        xSemaphoreTake(s_lock, pdMS_TO_TICKS(10)) != pdTRUE) return false;
    for (const char *p = ticket; *p != '\0'; ++p) {
        if (!isalnum((unsigned char)*p) && *p != '-' && *p != '_') {
            xSemaphoreGive(s_lock);
            return false;
        }
    }
    const bool match = (s_phase == ALERT_RECORDING || s_phase == ALERT_SAVING ||
                        s_phase == ALERT_WAIT_GRANT) &&
                       strcmp(s_event_id, event_id) == 0;
    if (match) {
        memcpy(s_alert_id, alert_id, 33);
        strcpy(s_ticket, ticket);
        s_grant_deadline_us = esp_timer_get_time() + (int64_t)expires_in * 1000000LL;
        s_max_bytes = max_bytes;
        s_granted = true;
    }
    xSemaphoreGive(s_lock);
    return match;
}

static bool write_all(esp_http_client_handle_t client, const char *data, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
        const int n = esp_http_client_write(client, data + sent, (int)(len - sent));
        if (n <= 0) return false;
        sent += (size_t)n;
    }
    return true;
}

static esp_err_t save_clip_to_tf(void)
{
    if (!sd_card_is_mounted()) return ESP_ERR_INVALID_STATE;
    snprintf(s_file_path, sizeof(s_file_path), "/sdcard/alert_%s.h264", s_event_id);
    FILE *file = fopen(s_file_path, "wb");
    if (file == NULL) return ESP_FAIL;
    uint8_t *chunk = heap_caps_malloc(ALERT_UPLOAD_CHUNK, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (chunk == NULL) {
        fclose(file);
        remove(s_file_path);
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = ESP_OK;
    uint32_t written_hash = 2166136261U;
    for (size_t i = 0; i < s_clip_count && err == ESP_OK; ++i) {
        for (size_t offset = 0; offset < s_clip[i].len; offset += ALERT_UPLOAD_CHUNK) {
            size_t n = s_clip[i].len - offset;
            if (n > ALERT_UPLOAD_CHUNK) n = ALERT_UPLOAD_CHUNK;
            memcpy(chunk, s_clip[i].data + offset, n);
            if (fwrite(chunk, 1, n, file) != n) { err = ESP_FAIL; break; }
            for (size_t j = 0; j < n; ++j) written_hash = (written_hash ^ chunk[j]) * 16777619U;
        }
    }
    if (fflush(file) != 0) err = ESP_FAIL;
    if (fclose(file) != 0) err = ESP_FAIL;
    if (err == ESP_OK) {
        file = fopen(s_file_path, "rb");
        if (file == NULL) {
            err = ESP_FAIL;
        } else {
            size_t read_bytes = 0;
            uint32_t read_hash = 2166136261U;
            size_t n;
            while ((n = fread(chunk, 1, ALERT_UPLOAD_CHUNK, file)) > 0) {
                read_bytes += n;
                for (size_t j = 0; j < n; ++j) read_hash = (read_hash ^ chunk[j]) * 16777619U;
            }
            if (ferror(file) || read_bytes != s_clip_bytes || read_hash != written_hash) {
                err = ESP_FAIL;
            }
            fclose(file);
            ESP_LOGI(TAG, "TF_VERIFY event_id=%s bytes=%u result=%s",
                     s_event_id, (unsigned)read_bytes, esp_err_to_name(err));
        }
    }
    heap_caps_free(chunk);
    if (err != ESP_OK) remove(s_file_path);
    return err;
}

static esp_err_t upload_file(size_t file_bytes, uint32_t duration_ms)
{
    const int64_t started_us = esp_timer_get_time();
    if (s_grant_deadline_us <= esp_timer_get_time() ||
        file_bytes > s_max_bytes || file_bytes > ALERT_CLIP_MAX_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }
    char url[448];
    const int url_len = snprintf(url, sizeof(url),
        "https://www.lummiss.com/lummiss/device-api/v1/device-alerts"
        "?alertId=%s&ticket=%s&container=RAW_H264&durationMs=%" PRIu32,
        s_alert_id, s_ticket, duration_ms);
    if (url_len <= 0 || (size_t)url_len >= sizeof(url)) return ESP_ERR_INVALID_SIZE;
    char head[256];
    const int head_len = snprintf(head, sizeof(head),
        "--%s\r\nContent-Disposition: form-data; name=\"file\"; filename=\"clip.h264\"\r\n"
        "Content-Type: application/octet-stream\r\n\r\n", k_boundary);
    char tail[80];
    const int tail_len = snprintf(tail, sizeof(tail), "\r\n--%s--\r\n", k_boundary);
    if (head_len <= 0 || (size_t)head_len >= sizeof(head) ||
        tail_len <= 0 || (size_t)tail_len >= sizeof(tail)) return ESP_ERR_INVALID_SIZE;

    FILE *file = fopen(s_file_path, "rb");
    if (file == NULL) return ESP_FAIL;
    uint8_t *chunk = heap_caps_malloc(ALERT_UPLOAD_CHUNK, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (chunk == NULL) { fclose(file); return ESP_ERR_NO_MEM; }
    const esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 60000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 4096,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) { heap_caps_free(chunk); fclose(file); return ESP_ERR_NO_MEM; }
    char content_type[96];
    snprintf(content_type, sizeof(content_type), "multipart/form-data; boundary=%s", k_boundary);
    esp_http_client_set_header(client, "Content-Type", content_type);
    esp_http_client_set_header(client, "Connection", "close");
    esp_err_t err = esp_http_client_open(client, head_len + file_bytes + tail_len);
    if (err == ESP_OK && !write_all(client, head, (size_t)head_len)) err = ESP_FAIL;
    size_t transferred = 0;
    while (err == ESP_OK && transferred < file_bytes) {
        const size_t n = fread(chunk, 1, ALERT_UPLOAD_CHUNK, file);
        if (n == 0 || !write_all(client, (char *)chunk, n)) { err = ESP_FAIL; break; }
        transferred += n;
    }
    if (err == ESP_OK && !write_all(client, tail, (size_t)tail_len)) err = ESP_FAIL;
    int status = 0;
    int business_code = -1;
    if (err == ESP_OK) {
        (void)esp_http_client_fetch_headers(client);
        status = esp_http_client_get_status_code(client);
        char response[512] = {0};
        const int got = esp_http_client_read_response(client, response, sizeof(response) - 1);
        cJSON *root = got > 0 ? cJSON_Parse(response) : NULL;
        cJSON *code = root ? cJSON_GetObjectItem(root, "code") : NULL;
        if (cJSON_IsNumber(code)) business_code = code->valueint;
        cJSON_Delete(root);
        if (status < 200 || status >= 300 || business_code != 0) err = ESP_FAIL;
    }
    ESP_LOGI(TAG, "UPLOAD_RESULT event_id=%s alert_id=%s bytes=%u elapsed_ms=%" PRId64
             " http=%d code=%d result=%s",
             s_event_id, s_alert_id, (unsigned)transferred,
             (esp_timer_get_time() - started_us) / 1000, status, business_code,
             esp_err_to_name(err));
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    heap_caps_free(chunk);
    fclose(file);
    memset(s_ticket, 0, sizeof(s_ticket));
    return err;
}

static void alert_clip_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(100));
        if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) continue;
        const int64_t now = esp_timer_get_time();
        if (s_phase == ALERT_RECORDING && now >= s_trigger_us + ALERT_POST_US) {
            s_phase = ALERT_SAVING;
            ESP_LOGI(TAG, "CAPTURE_END event_id=%s duration_ms=%" PRId64
                     " frames=%u bytes=%u", s_event_id,
                     (s_last_clip_us - s_first_clip_us) / 1000,
                     (unsigned)s_clip_count, (unsigned)s_clip_bytes);
            xSemaphoreGive(s_lock);
            const int64_t save_started_us = esp_timer_get_time();
            const esp_err_t saved = save_clip_to_tf();
            ESP_LOGI(TAG, "TF_SAVE event_id=%s bytes=%u elapsed_ms=%" PRId64
                     " result=%s path=%s",
                     s_event_id, (unsigned)s_clip_bytes,
                     (esp_timer_get_time() - save_started_us) / 1000,
                     esp_err_to_name(saved), s_file_path);
            xSemaphoreTake(s_lock, portMAX_DELAY);
            clear_clip();
            if (saved == ESP_OK) s_phase = ALERT_WAIT_GRANT;
            else reset_job();
        } else if (s_phase == ALERT_WAIT_GRANT && s_granted) {
            if (now < s_grant_deadline_us) {
                const uint32_t duration_ms = (uint32_t)((s_last_clip_us - s_first_clip_us) / 1000);
                s_phase = ALERT_UPLOADING;
                xSemaphoreGive(s_lock);
                struct stat st;
                const size_t file_bytes = stat(s_file_path, &st) == 0 ? (size_t)st.st_size : 0;
                const esp_err_t uploaded = file_bytes > 0 ?
                    upload_file(file_bytes, duration_ms) : ESP_FAIL;
                if (uploaded == ESP_OK) remove(s_file_path);
                else ESP_LOGW(TAG, "上传未成功，保留 TF 文件：%s", s_file_path);
                xSemaphoreTake(s_lock, portMAX_DELAY);
                reset_job();
            } else {
                ESP_LOGW(TAG, "授权已过期，保留 TF 文件：%s", s_file_path);
                reset_job();
            }
        } else if (s_phase == ALERT_WAIT_GRANT && now - s_trigger_us > 40000000LL) {
            ESP_LOGW(TAG, "40 秒内未收到授权，保留 TF 文件：%s", s_file_path);
            reset_job();
        }
        xSemaphoreGive(s_lock);
    }
}

esp_err_t alert_clip_init(void)
{
    if (s_lock != NULL) return ESP_OK;
    s_ring = heap_caps_calloc(ALERT_RING_FRAMES, sizeof(*s_ring),
                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_clip = heap_caps_calloc(ALERT_CLIP_FRAMES, sizeof(*s_clip),
                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_ring == NULL || s_clip == NULL) {
        heap_caps_free(s_ring);
        heap_caps_free(s_clip);
        s_ring = s_clip = NULL;
        return ESP_ERR_NO_MEM;
    }
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        heap_caps_free(s_ring);
        heap_caps_free(s_clip);
        s_ring = s_clip = NULL;
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreatePinnedToCoreWithCaps(alert_clip_task, "alert_clip", 12288,
            NULL, 3, NULL, 0, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        vSemaphoreDelete(s_lock);
        s_lock = NULL;
        heap_caps_free(s_ring);
        heap_caps_free(s_clip);
        s_ring = s_clip = NULL;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "预警片段已启用：PSRAM 前10秒 + 后10秒，TF落盘，HTTPS分块上传");
    return ESP_OK;
}
