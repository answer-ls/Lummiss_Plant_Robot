#include "capture_diag.h"
#include "audio_probe.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "mbedtls/base64.h"
#include "soc/i2s_struct.h"

#define SECONDS 8
/* 原始数据保持 24kHz 四槽，便于离线重新验证槽位映射。 */
static struct {
    uint8_t *data;
    size_t size, used;
    int64_t first_us, last_us;
} streams[7];
static const unsigned channels[] = {4, 1, 1, 3, 1, 1, 1};
/* 各层块时间戳记录在PSRAM，AFE/Opus有延迟，不能假定相同数组下标就是同一时刻。 */
static struct stamp { int64_t us; uint32_t offset, bytes; } *stamps;
static unsigned stamp_count[7];
#define STAMP_LIMIT 1024
static SemaphoreHandle_t lock;
static bool recording;
static bool started;
static int64_t deadline;

static void append_locked(unsigned index, const int16_t *data, size_t bytes, int64_t now)
{
    if (recording && now < deadline) {
        size_t count = streams[index].size - streams[index].used;
        if (count > bytes) count = bytes;
        if (count) {
            if (!streams[index].used) streams[index].first_us = now;
            if (stamp_count[index] < STAMP_LIMIT) {
                stamps[index * STAMP_LIMIT + stamp_count[index]++] =
                    (struct stamp){now, streams[index].used, count};
            }
            memcpy(streams[index].data + streams[index].used, data, count);
            streams[index].used += count;
            streams[index].last_us = now;
        }
    }
}
static void append(unsigned index, const int16_t *data, size_t bytes)
{
    if (!__atomic_load_n(&recording, __ATOMIC_ACQUIRE)) return;
    xSemaphoreTake(lock, portMAX_DELAY);
    append_locked(index, data, bytes, esp_timer_get_time());
    xSemaphoreGive(lock);
}

/* 原始四槽与重采样输出由同一次capture调用提交，使用同一录音边界。
 * raw帧: int16[4*n+0..3]=SLOT0..3（物理映射尚未确认）。
 * feed帧: int16[3*n+0]=历史MIC1槽0，+1=历史MIC2槽2，+2=历史REF槽1。
 * 这里的MIC命名是当前软件路由名称，不是本次实验已证实的物理映射。 */
void audio_probe_input(const int16_t *raw, size_t raw_bytes, const int16_t *mmr, size_t frames)
{
    if (!__atomic_load_n(&recording, __ATOMIC_ACQUIRE)) return;
    if (frames > 160 || raw_bytes != frames * 12 || frames % 2) return;
    int16_t mono[3][160];
    for (size_t n = 0; n < frames; ++n) {
        for (unsigned ch = 0; ch < 3; ++ch) mono[ch][n] = mmr[3*n+ch];
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    int64_t now = esp_timer_get_time();
    append_locked(0, raw, raw_bytes, now);
    append_locked(3, mmr, frames * 6, now);
    for (unsigned ch = 0; ch < 3; ++ch) append_locked(4+ch, mono[ch], frames * 2, now);
    xSemaphoreGive(lock);
}
void audio_probe_raw(const int16_t *data, size_t bytes) { append(0, data, bytes); }
void audio_probe_afe(const int16_t *data, size_t bytes) { append(1, data, bytes); }
void audio_probe_uplink(const int16_t *data, size_t bytes) { append(2, data, bytes); }

static void probe_task(void *arg)
{
    (void)arg;
    ESP_LOGI("AUDIO_PROBE", "RECORD_BEGIN 回答结束后录音8秒：请直接说现在几点了");
    xSemaphoreTake(lock, portMAX_DELAY);
    deadline = esp_timer_get_time() + SECONDS * 1000000LL;
    __atomic_store_n(&recording, true, __ATOMIC_RELEASE);
    xSemaphoreGive(lock);
    vTaskDelay(pdMS_TO_TICKS(SECONDS * 1000));
    xSemaphoreTake(lock, portMAX_DELAY);
    __atomic_store_n(&recording, false, __ATOMIC_RELEASE);
    xSemaphoreGive(lock);
    ESP_LOGI("AUDIO_PROBE", "RECORD_END 已缓存同一窗口raw/MMR/三通道/AFE/Opus输入，27秒后导出，继续正常对话");
    /* 本次监听最多30秒；把大批串口输出移到该观察窗口之后。 */
    vTaskDelay(pdMS_TO_TICKS(27000));
    ESP_LOGI("AUDIO_PROBE", "EXPORT_BEGIN 开始导出，请等待约8分钟，不要关闭串口");
    /* 导出仅在录制停止后运行；序号和整段校验用于发现串口丢行。 */
    printf("AP_LAYOUT frame[n]: raw[4*n+0..3]=SLOT0..3; mmr[3*n+0]=MIC1(slot0), +1=MIC2(slot2), +2=REF(slot1)\n");
    /* 录音后打印确定种子的100个抽样帧，避免串口输出干扰采集时序。 */
    size_t frames = streams[3].used / 6;
    uint32_t rng = 0x7210;
    for (unsigned i = 0; i < 100 && frames; ++i) {
        rng = rng * 1664525u + 1013904223u;
        size_t n = rng % frames;
        const int16_t *mmr = (const int16_t *)streams[3].data;
        printf("AP_FRAME %u %d %d %d\n", (unsigned)n, mmr[3*n], mmr[3*n+1], mmr[3*n+2]);
    }
    for (unsigned id = 0; id < 7; ++id) {
        printf("\nAP_BEGIN %u %u %u %u %lld %lld\n", id, id ? 16000 : 24000,
               channels[id], (unsigned)streams[id].used,
               (long long)streams[id].first_us, (long long)streams[id].last_us);
        uint32_t hash = 2166136261U;
        for (size_t pos = 0; pos < streams[id].used;) {
            unsigned char encoded[345];
            size_t length = streams[id].used - pos, output = 0;
            if (length > 256) length = 256;
            mbedtls_base64_encode(encoded, sizeof(encoded), &output, streams[id].data + pos, length);
            for (size_t i = 0; i < length; ++i) hash = (hash ^ streams[id].data[pos+i]) * 16777619U;
            printf("AP_DATA %u %u %.*s\n", id, (unsigned)pos, (int)output, encoded);
            pos += length;
            vTaskDelay(1);
        }
        printf("AP_END %u %08lx\n", id, (unsigned long)hash);
        for (unsigned i = 0; i < stamp_count[id]; ++i) {
            struct stamp *t = &stamps[id * STAMP_LIMIT + i];
            printf("AP_TIME %u %u %u %lld\n", id, (unsigned)t->offset, (unsigned)t->bytes, (long long)t->us);
        }
        free(streams[id].data);
        streams[id].data = NULL;
    }
    /* 保留互斥量至重启，避免刚检查 recording 的采集任务访问已释放锁。 */
    free(stamps);
    stamps = NULL;
    printf("AP_DONE\n");
    vTaskDelete(NULL);
}
void audio_probe_start(void)
{
    /* A/B窗口使用独立raw缓存；禁止旧探针只在首轮额外占用采集和串口时间。 */
    if (!AUDIO_RECORDING_DIAGNOSTIC || CAPTURE_UPLINK_AB_DIAGNOSTIC) return;
    if (started) return;
    started = true;
    /* 回答播放后再读一次真实RX格式，避免把运行时变更的格式误标为四槽16bit。 */
    ESP_LOGI("AUDIO_PROBE", "RX_FORMAT data=%u slot=%u total=%u ws=%u shift=%u conf=%08lx conf1=%08lx tdm=%08lx",
             (unsigned)I2S0.rx_conf1.rx_bits_mod + 1, (unsigned)I2S0.rx_conf1.rx_tdm_chan_bits + 1,
             (unsigned)I2S0.rx_tdm_ctrl.rx_tdm_tot_chan_num + 1, (unsigned)I2S0.rx_conf1.rx_tdm_ws_width + 1,
             (unsigned)I2S0.rx_conf.rx_msb_shift, (unsigned long)I2S0.rx_conf.val,
             (unsigned long)I2S0.rx_conf1.val, (unsigned long)I2S0.rx_tdm_ctrl.val);
    if (I2S0.rx_conf1.rx_bits_mod != 15 || I2S0.rx_conf1.rx_tdm_chan_bits != 15 ||
        I2S0.rx_tdm_ctrl.rx_tdm_tot_chan_num != 3 || (I2S0.rx_tdm_ctrl.val & 0xffff) != 15) {
        ESP_LOGE("AUDIO_PROBE", "RX格式不符，停止诊断录音，正常语音任务保持运行");
        return;
    }
    streams[0].size = SECONDS * 24000 * 4 * 2;
    streams[1].size = SECONDS * 16000 * 2;
    streams[2].size = SECONDS * 16000 * 2;
    streams[3].size = SECONDS * 16000 * 3 * 2;
    for (unsigned i = 4; i < 7; ++i) streams[i].size = SECONDS * 16000 * 2;
    stamps = heap_caps_malloc(7 * STAMP_LIMIT * sizeof(*stamps), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    lock = xSemaphoreCreateMutex();
    for (unsigned i = 0; i < 7; ++i)
        streams[i].data = heap_caps_malloc(streams[i].size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    bool allocated = lock && stamps;
    for (unsigned i = 0; i < 7; ++i) allocated = allocated && streams[i].data;
    if (!allocated ||
        xTaskCreate(probe_task, "audio_probe", 4096, NULL, 1, NULL) != pdPASS) {
        for (unsigned i = 0; i < 7; ++i) free(streams[i].data);
        free(stamps);
        if (lock) vSemaphoreDelete(lock);
        ESP_LOGE("AUDIO_PROBE", "录音内存/任务申请失败，正常音频不受影响");
    }
}
