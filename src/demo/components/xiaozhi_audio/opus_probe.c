#include "opus_probe.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"

#define OPUS_PROBE_SECONDS 8
#define OPUS_PROBE_CAPACITY (200 * 1024)
#define OPUS_PROBE_HEADER_BYTES 10

static SemaphoreHandle_t s_lock;
static uint8_t *s_data;
static size_t s_used;
static uint32_t s_packets;
static uint32_t s_overflow;
static int64_t s_first_us;
static int64_t s_last_us;
static int64_t s_deadline_us;
static bool s_started;
static bool s_recording;

void opus_probe_packet(const uint8_t *packet, size_t bytes)
{
    if (!__atomic_load_n(&s_recording, __ATOMIC_ACQUIRE) || packet == NULL ||
        bytes == 0 || bytes > UINT16_MAX) return;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    const int64_t now_us = esp_timer_get_time();
    if (s_recording && now_us < s_deadline_us) {
        if (s_used + OPUS_PROBE_HEADER_BYTES + bytes <= OPUS_PROBE_CAPACITY) {
            /* 每条记录为 little-endian uint16 长度、uint64 时间戳、原始 Opus 包。 */
            s_data[s_used] = (uint8_t)bytes;
            s_data[s_used + 1] = (uint8_t)(bytes >> 8);
            const uint64_t stamp = (uint64_t)now_us;
            for (unsigned i = 0; i < 8; ++i) {
                s_data[s_used + 2 + i] = (uint8_t)(stamp >> (i * 8));
            }
            memcpy(s_data + s_used + OPUS_PROBE_HEADER_BYTES, packet, bytes);
            s_used += OPUS_PROBE_HEADER_BYTES + bytes;
            if (s_packets == 0) s_first_us = now_us;
            s_last_us = now_us;
            ++s_packets;
        } else {
            ++s_overflow;
        }
    }
    xSemaphoreGive(s_lock);
}

static void opus_probe_task(void *arg)
{
    (void)arg;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_deadline_us = esp_timer_get_time() + OPUS_PROBE_SECONDS * 1000000LL;
    __atomic_store_n(&s_recording, true, __ATOMIC_RELEASE);
    xSemaphoreGive(s_lock);
    ESP_LOGI("OPUS_PROBE", "RECORD_BEGIN 只缓存8秒实际发送成功的 Opus 包");

    vTaskDelay(pdMS_TO_TICKS(OPUS_PROBE_SECONDS * 1000));
    xSemaphoreTake(s_lock, portMAX_DELAY);
    __atomic_store_n(&s_recording, false, __ATOMIC_RELEASE);
    xSemaphoreGive(s_lock);
    ESP_LOGI("OPUS_PROBE", "RECORD_END packets=%lu bytes=%u overflow=%lu；27秒后导出",
             (unsigned long)s_packets, (unsigned)s_used, (unsigned long)s_overflow);

    /* 在30秒续听观察窗口结束后导出，避免串口输出干扰本轮识别。 */
    vTaskDelay(pdMS_TO_TICKS(27000));
    ESP_LOGI("OPUS_PROBE", "EXPORT_BEGIN 请保持串口连接直到 OP_DONE");
    printf("OP_BEGIN 1 16000 60 %lu %u %lld %lld %lu\n",
           (unsigned long)s_packets, (unsigned)s_used,
           (long long)s_first_us, (long long)s_last_us, (unsigned long)s_overflow);
    uint32_t hash = 2166136261U;
    for (size_t pos = 0; pos < s_used;) {
        unsigned char encoded[345];
        size_t length = s_used - pos, output = 0;
        if (length > 256) length = 256;
        mbedtls_base64_encode(encoded, sizeof(encoded), &output, s_data + pos, length);
        for (size_t i = 0; i < length; ++i) hash = (hash ^ s_data[pos + i]) * 16777619U;
        printf("OP_DATA %u %.*s\n", (unsigned)pos, (int)output, encoded);
        pos += length;
        vTaskDelay(1);
    }
    printf("OP_END %08lx\nOP_DONE\n", (unsigned long)hash);
    fflush(stdout);
    heap_caps_free(s_data);
    s_data = NULL;
    vTaskDelete(NULL);
}

void opus_probe_start(void)
{
    if (s_started) return;
    s_started = true;
    s_data = heap_caps_malloc(OPUS_PROBE_CAPACITY, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_lock = xSemaphoreCreateMutex();
    if (s_data == NULL || s_lock == NULL ||
        xTaskCreate(opus_probe_task, "opus_probe", 4096, NULL, 1, NULL) != pdPASS) {
        heap_caps_free(s_data);
        s_data = NULL;
        if (s_lock != NULL) vSemaphoreDelete(s_lock);
        s_lock = NULL;
        ESP_LOGE("OPUS_PROBE", "诊断资源申请失败，正常上行保持运行");
    }
}
