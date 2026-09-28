#include "wake_word.h"
#include "audio_probe.h"
#include "mem_contig.h"

#include <string.h>

#include "esp_afe_sr_models.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"

static const char *TAG = "WAKE_WORD";

#define WAKE_EVENT_RUNNING  BIT0
#define WAKE_EVENT_FEED_STARTED BIT1
#define WAKE_EVENT_EXITED BIT2
#define WAKE_FETCH_TIMEOUT_MS 100U
#define WAKE_PREROLL_SAMPLE_RATE 16000U
#define WAKE_PREROLL_SECONDS     2U
#define WAKE_PREROLL_SAMPLES     (WAKE_PREROLL_SAMPLE_RATE * WAKE_PREROLL_SECONDS)
#define AFE_OUTPUT_BYTES (4U * 960U * sizeof(int16_t))

typedef struct {
    int channels;
    srmodel_list_t *models;
    const esp_afe_sr_iface_t *afe_iface;
    esp_afe_sr_data_t *afe_data;
    char *wakenet_model_name;
    char wake_words_str[128];
    char last_wake_word[64];
    size_t feed_chunk_size;
    size_t fetch_chunk_size;
    EventGroupHandle_t event_group;
    TaskHandle_t detection_task;
    wake_word_callback_t callback;
    void *callback_user_data;
    bool detected;
    int16_t *feed_buffer;
    size_t feed_buffer_count;
    int16_t *preroll_buffer;
    size_t preroll_capacity;
    size_t preroll_count;
    size_t preroll_write;
    volatile uint32_t feed_count;
    volatile uint32_t fetch_count;
    volatile uint32_t fetch_null_count;
    volatile uint32_t feed_fail;
    volatile uint32_t wake_detect_count;
    volatile uint32_t output_samples;
    volatile uint32_t output_drop;
    volatile uint32_t fetch_errors;
    volatile uint32_t processed_samples;
    volatile uint32_t processed_peak;
    volatile uint32_t speech_frames;
    volatile uint32_t silence_frames;
    volatile uint32_t speech_begin_count;
    volatile uint32_t speech_end_count;
    bool voice_detected;
    bool stopping;
    volatile uint32_t feed_samples;
    volatile uint32_t accumulator_remaining_samples;
    volatile uint32_t ringbuffer_underflow;
    SemaphoreHandle_t output_lock;
    StreamBufferHandle_t output_stream;
    StaticStreamBuffer_t output_stream_control;
    uint8_t *output_storage;
    bool uplink_enabled;
    uint32_t output_generation;
    uint32_t wake_generation;
} wake_word_ctx_t;

static wake_word_ctx_t s_ww;

/* A 保留静态栈作对照；B 在模型生命周期结束后同步归还动态栈和 TCB。
 * 同步对象始终保留，生命周期仍由启动流程/capture_guard 串行保护。 */
#define WAKE_LIFECYCLE_STACK_BYTES 8192U
#ifndef CONFIG_WAKE_CLEANUP_TRANSIENT
#define CONFIG_WAKE_CLEANUP_TRANSIENT 0
#endif
#if !CONFIG_WAKE_CLEANUP_TRANSIENT
static DRAM_ATTR StackType_t s_lifecycle_stack[WAKE_LIFECYCLE_STACK_BYTES / sizeof(StackType_t)];
static DRAM_ATTR StaticTask_t s_lifecycle_task_storage;
#endif
static DRAM_ATTR StaticSemaphore_t s_lifecycle_lock_storage;
static DRAM_ATTR StaticSemaphore_t s_lifecycle_done_storage;
static TaskHandle_t s_lifecycle_task;
static SemaphoreHandle_t s_lifecycle_lock;
static SemaphoreHandle_t s_lifecycle_done;
static bool s_lifecycle_load;

static void wake_word_deinit_internal(void);

static void wake_lifecycle_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (s_lifecycle_load) {
            mem_contig_log("MODEL_LOAD_BEFORE");
            ESP_LOGI(TAG, "WAKE_MODEL_LOAD begin stack_caps=INTERNAL stack_bytes=%u", WAKE_LIFECYCLE_STACK_BYTES);
            s_ww.models = esp_srmodel_init("storage");
            mem_contig_log("MODEL_LOAD_AFTER");
            ESP_LOGI(TAG, "WAKE_MODEL_LOAD done loaded=%d stack_high_water=%u",
                     s_ww.models != NULL, (unsigned)uxTaskGetStackHighWaterMark(NULL));
        } else {
            wake_word_deinit_internal();
        }
        /* 完成之后不再访问模型/AFE；调用方拿到确认才允许 RTC 继续。 */
        xSemaphoreGive(s_lifecycle_done);
    }
}

static esp_err_t wake_lifecycle_init(void)
{
    if (s_lifecycle_lock != NULL) return ESP_OK;
    s_lifecycle_lock = xSemaphoreCreateMutexStatic(&s_lifecycle_lock_storage);
    s_lifecycle_done = xSemaphoreCreateBinaryStatic(&s_lifecycle_done_storage);
    return ESP_OK;
}

static esp_err_t wake_lifecycle_call(bool load)
{
    /* 禁止并发覆盖请求；完成信号独立于调用任务自己的通知槽。 */
    xSemaphoreTake(s_lifecycle_lock, portMAX_DELAY);
    if (s_lifecycle_task == NULL && !load && s_ww.models == NULL && s_ww.afe_data == NULL) {
        xSemaphoreGive(s_lifecycle_lock);
        return ESP_OK;
    }
    if (s_lifecycle_task == NULL) {
        mem_contig_log("WAKE_CLEANUP_CREATE_BEFORE");
#if CONFIG_WAKE_CLEANUP_TRANSIENT
        BaseType_t created = xTaskCreatePinnedToCoreWithCaps(
            wake_lifecycle_task, "wake_cleanup", WAKE_LIFECYCLE_STACK_BYTES,
            NULL, 8, &s_lifecycle_task, 1, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (created != pdPASS) s_lifecycle_task = NULL;
#else
    s_lifecycle_task = xTaskCreateStaticPinnedToCore(
        wake_lifecycle_task, "wake_cleanup", WAKE_LIFECYCLE_STACK_BYTES,
        NULL, 8, s_lifecycle_stack, &s_lifecycle_task_storage, 1);
#endif
        mem_contig_log("WAKE_CLEANUP_CREATE_AFTER");
        ESP_LOGI(TAG, "CLEANUP_AB mode=%s task=%p stack_bytes=%u core=1 priority=8",
                 CONFIG_WAKE_CLEANUP_TRANSIENT ? "B_TRANSIENT" : "A_STATIC", s_lifecycle_task, WAKE_LIFECYCLE_STACK_BYTES);
        if (s_lifecycle_task == NULL) {
            xSemaphoreGive(s_lifecycle_lock);
            return ESP_ERR_NO_MEM;
        }
    }
    s_lifecycle_load = load;
    xTaskNotifyGive(s_lifecycle_task);
    xSemaphoreTake(s_lifecycle_done, portMAX_DELAY);
    if (!load) {
        mem_contig_log("WAKE_CLEANUP_DELETE_BEFORE");
#if CONFIG_WAKE_CLEANUP_TRANSIENT
        /* 由调用方删除，不走 worker 自删的延迟回收路径。本地 IDF 会先暂停目标、
         * 等待它离开所有 CPU，再同步销毁并释放 WithCaps 栈/TCB；返回后才 ACK RTC。 */
        vTaskDeleteWithCaps(s_lifecycle_task);
        s_lifecycle_task = NULL;
        mem_contig_log("WAKE_CLEANUP_DELETE_AFTER");
#else
        /* 静态数组不会因删任务变成 heap；A 组明确记录仍然保留，不能伪称已回收。 */
        mem_contig_log("WAKE_CLEANUP_RETAINED_A");
#endif
    }
    xSemaphoreGive(s_lifecycle_lock);
    return ESP_OK;
}

static inline void stats_inc(volatile uint32_t *value)
{
    __atomic_add_fetch(value, 1U, __ATOMIC_RELAXED);
}

/* 保存最近两秒 AFE 输出。检测任务是唯一写入者，检测回调在同一任务中读取，
 * 因而不需要额外锁，也不会在每个音频块上 malloc/free。 */
static void store_preroll(wake_word_ctx_t *ctx,
                          const int16_t *data,
                          size_t samples)
{
    if (ctx->preroll_buffer == NULL || ctx->preroll_capacity == 0) {
        return;
    }
    for (size_t i = 0; i < samples; i++) {
        ctx->preroll_buffer[ctx->preroll_write] = data[i];
        ctx->preroll_write = (ctx->preroll_write + 1) % ctx->preroll_capacity;
        if (ctx->preroll_count < ctx->preroll_capacity) {
            ctx->preroll_count++;
        }
    }
}

/* WakeNet 返回的 model index 从 1 开始；模型字符串中的唤醒词用分号分隔。 */
static void select_detected_word(wake_word_ctx_t *ctx, int model_index)
{
    const char *start = ctx->wake_words_str;
    int current = 1;
    while (current < model_index && start != NULL) {
        start = strchr(start, ';');
        if (start != NULL) {
            start++;
            current++;
        }
    }
    if (start == NULL || *start == '\0') {
        start = ctx->wake_words_str;
    }
    const char *end = strchr(start, ';');
    size_t length = end == NULL ? strlen(start) : (size_t)(end - start);
    if (length >= sizeof(ctx->last_wake_word)) {
        length = sizeof(ctx->last_wake_word) - 1;
    }
    memcpy(ctx->last_wake_word, start, length);
    ctx->last_wake_word[length] = '\0';
}

/* 与官方 AfeAudioEngine 一样，VAD 事件与增强 PCM 输出在此分开处理。 */
static void handle_voice_result(wake_word_ctx_t *ctx, const afe_fetch_result_t *afe_result,
                                uint32_t output_generation)
{
    /* VAD 只报告状态变化，不作为 PCM 输出开关。静音也必须送给服务端。 */
    const bool speech = afe_result->vad_state == VAD_SPEECH;
    stats_inc(speech ? &ctx->speech_frames : &ctx->silence_frames);
    if (speech != __atomic_load_n(&ctx->voice_detected, __ATOMIC_RELAXED)) {
        /* 只在有效AFE结果发生转换时记录，不能将旧缓存状态当作新VAD判断。 */
        ESP_LOGI("VAD_EDGE", "us=%lld VAD %s -> %s", (long long)esp_timer_get_time(),
                 speech ? "SILENCE" : "SPEECH", speech ? "SPEECH" : "SILENCE");
        __atomic_store_n(&ctx->voice_detected, speech, __ATOMIC_RELAXED);
        stats_inc(speech ? &ctx->speech_begin_count : &ctx->speech_end_count);
    }

    xSemaphoreTake(ctx->output_lock, portMAX_DELAY);
    if (ctx->uplink_enabled && output_generation == ctx->output_generation) {
        /* 输出缓冲在 PSRAM，满时明确报错，不能阻塞 fetch 导致采集停顿。 */
        const size_t bytes = afe_result->data_size;
        if (xStreamBufferSpacesAvailable(ctx->output_stream) >= bytes) {
            xStreamBufferSend(ctx->output_stream, afe_result->data, bytes, 0);
            __atomic_add_fetch(&ctx->output_samples, bytes / sizeof(int16_t), __ATOMIC_RELAXED);
        } else {
            stats_inc(&ctx->output_drop);
        }
    }
    xSemaphoreGive(ctx->output_lock);
}

static void detection_task(void *arg)
{
    (void)arg;
    wake_word_ctx_t *ctx = &s_ww;

    ESP_LOGI(TAG, "检测任务已启动，feed_samples_per_channel=%u fetch_samples=%u",
             (unsigned)(ctx->feed_chunk_size / ctx->channels),
             (unsigned)ctx->fetch_chunk_size);

    uint32_t wake_generation = UINT32_MAX;
    while (ctx->afe_data != NULL) {
        /* 模型先于 I2S 初始化，首包到达前不空转 fetch。 */
        xEventGroupWaitBits(ctx->event_group, WAKE_EVENT_FEED_STARTED,
                           pdFALSE, pdTRUE, portMAX_DELAY);
        if (__atomic_load_n(&ctx->stopping, __ATOMIC_ACQUIRE)) break;
        /* WakeNet 控制仅在 fetch 所属任务应用，避免其他任务与 fetch/reset 并发。
         * AFE 持续运行，停止唤醒检测并不停止双麦语音处理。 */
        const uint32_t requested = __atomic_load_n(&ctx->wake_generation, __ATOMIC_ACQUIRE);
        const bool detecting = (xEventGroupGetBits(ctx->event_group) & WAKE_EVENT_RUNNING) != 0;
        if (requested != wake_generation) {
            if (detecting) {
                ctx->preroll_count = 0;
                ctx->preroll_write = 0;
                int result = ctx->afe_iface->enable_wakenet(ctx->afe_data);
                ESP_LOGI(TAG, "WakeNet enabled result=%d input_channels=%d", result, ctx->channels);
            } else {
                ctx->afe_iface->disable_wakenet(ctx->afe_data);
            }
            wake_generation = requested;
        }
        xSemaphoreTake(ctx->output_lock, portMAX_DELAY);
        const uint32_t output_generation = ctx->output_generation;
        xSemaphoreGive(ctx->output_lock);

        /* 有界等待便于应用 WakeNet 状态变更；不在其他任务 reset AFE。 */
        afe_fetch_result_t *afe_result = ctx->afe_iface->fetch_with_delay(
            ctx->afe_data, pdMS_TO_TICKS(WAKE_FETCH_TIMEOUT_MS));

        if (afe_result == NULL) {
            stats_inc(&ctx->fetch_null_count);
            stats_inc(&ctx->ringbuffer_underflow);
            continue;
        }
        if (afe_result->ret_value != ESP_OK || afe_result->data == NULL || afe_result->data_size <= 0) {
            stats_inc(&ctx->fetch_errors);
            if (afe_result->ret_value == ESP_FAIL || afe_result->data == NULL || afe_result->data_size <= 0) {
                stats_inc(&ctx->ringbuffer_underflow);
            }
            continue;
        }
        /* 只统计有效音频；超时返回非空对象也不能被当作成功 fetch。 */
        stats_inc(&ctx->fetch_count);
        uint32_t peak = 0;
        const size_t samples = afe_result->data_size / sizeof(int16_t);
        for (size_t i = 0; i < samples; ++i) {
            int32_t sample = afe_result->data[i];
            uint32_t magnitude = sample < 0 ? -sample : sample;
            if (magnitude > peak) peak = magnitude;
        }
        uint32_t previous = __atomic_load_n(&ctx->processed_peak, __ATOMIC_RELAXED);
        while (peak > previous && !__atomic_compare_exchange_n(
                   &ctx->processed_peak, &previous, peak, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
        __atomic_add_fetch(&ctx->processed_samples, samples, __ATOMIC_RELAXED);
        audio_probe_afe(afe_result->data, (size_t)afe_result->data_size);
        handle_voice_result(ctx, afe_result, output_generation);

        if (!detecting || requested != __atomic_load_n(&ctx->wake_generation, __ATOMIC_ACQUIRE)) {
            continue;
        }
        store_preroll(ctx, afe_result->data,
                      afe_result->data_size / sizeof(int16_t));

        if (afe_result->wakeup_state == WAKENET_DETECTED) {
            int model_idx = afe_result->wakenet_model_index;
            if (model_idx > 0) {
                select_detected_word(ctx, model_idx);
            } else {
                strncpy(ctx->last_wake_word, "unknown",
                        sizeof(ctx->last_wake_word) - 1);
            }

            ctx->detected = true;
            stats_inc(&ctx->wake_detect_count);
            ESP_LOGI(TAG, "检测到唤醒词: %s", ctx->last_wake_word);

            wake_word_stop();

            if (ctx->callback != NULL) {
                ctx->callback(ctx->last_wake_word, ctx->callback_user_data);
            }
        }
    }

    ESP_LOGW(TAG, "检测任务已退出");
    xEventGroupSetBits(ctx->event_group, WAKE_EVENT_EXITED);
    vTaskDelete(NULL);
}

esp_err_t wake_word_init(int channels)
{
    if (channels != 1 && channels != 3) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_ww.afe_data != NULL) {
        ESP_LOGW(TAG, "唤醒词引擎已初始化，跳过");
        return ESP_OK;
    }

    esp_err_t lifecycle_err = wake_lifecycle_init();
    if (lifecycle_err != ESP_OK) return lifecycle_err;

    memset(&s_ww, 0, sizeof(s_ww));
    s_ww.channels = channels <= 0 ? 1 : channels;

    /* 从 SPIFFS 加载语音识别模型。YOLO 使用 model 分区，
     * ESP-SR 唤醒模型由构建脚本独立烧录到 storage 分区。 */
    esp_err_t model_err = wake_lifecycle_call(true);
    if (model_err != ESP_OK) return model_err;
    if (s_ww.models == NULL || s_ww.models->num <= 0) {
        ESP_LOGE(TAG, "加载 Wakenet 模型失败");
        wake_word_deinit();
        return ESP_ERR_NOT_FOUND;
    }

    /* 遍历模型列表，找到 wakenet 模型并提取唤醒词 */
    for (int i = 0; i < s_ww.models->num; i++) {
        ESP_LOGI(TAG, "发现模型 %d: %s", i, s_ww.models->model_name[i]);
        if (strstr(s_ww.models->model_name[i], ESP_WN_PREFIX) != NULL) {
            s_ww.wakenet_model_name = s_ww.models->model_name[i];
            char *words = esp_srmodel_get_wake_words(s_ww.models,
                                                     s_ww.wakenet_model_name);
            if (words != NULL) {
                strncpy(s_ww.wake_words_str, words,
                        sizeof(s_ww.wake_words_str) - 1);
                ESP_LOGI(TAG, "唤醒词: %s", s_ww.wake_words_str);
            }
            break;
        }
    }

    if (s_ww.wakenet_model_name == NULL) {
        ESP_LOGE(TAG, "未找到 Wakenet 模型（前缀 %s）", ESP_WN_PREFIX);
        wake_word_deinit();
        return ESP_ERR_NOT_SUPPORTED;
    }

    /* 三通道必须是两只麦克风加参考，不能误配成三只麦克风 MMM。 */
    const char *input_format = channels == 3 ? "MMR" : "M";

    afe_config_t *afe_config = afe_config_init(
        input_format, s_ww.models, AFE_TYPE_SR, AFE_MODE_HIGH_PERF);
    if (afe_config == NULL) {
        ESP_LOGE(TAG, "创建 AFE 配置失败");
        wake_word_deinit();
        return ESP_FAIL;
    }

    afe_config->aec_init = channels == 3;
    afe_config->wakenet_init = true;
    afe_config->wakenet_model_name = s_ww.wakenet_model_name;
    afe_config->fixed_first_channel = false;
    afe_config->afe_perferred_core = 1;
    /* AFE 内部处理线程必须优先于 YOLO/home_info，避免 feed 持续写入而
     * fetch 长时间得不到调度。 */
    afe_config->afe_perferred_priority = 10;
    afe_config->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;

    mem_contig_log("AFE_CREATE_BEFORE");
    s_ww.afe_iface = esp_afe_handle_from_config(afe_config);
    if (s_ww.afe_iface != NULL) {
        s_ww.afe_data = s_ww.afe_iface->create_from_config(afe_config);
    }
    afe_config_free(afe_config);
    mem_contig_log("AFE_CREATE_AFTER");
    if (s_ww.afe_data == NULL) {
        ESP_LOGE(TAG, "创建 AFE 数据实例失败");
        wake_word_deinit();
        return ESP_FAIL;
    }

    /* API 返回每通道 int16 样本数，不能当成字节数或固定假定为 512。
     * accumulator 容量使用交错样本数，传入 feed 时再由 AFE 消费整块。 */
    const int feed_samples_per_channel = s_ww.afe_iface->get_feed_chunksize(s_ww.afe_data);
    if (feed_samples_per_channel <= 0) {
        wake_word_deinit();
        return ESP_ERR_INVALID_SIZE;
    }
    s_ww.feed_chunk_size = (size_t)feed_samples_per_channel * s_ww.channels;
    s_ww.fetch_chunk_size =
        s_ww.afe_iface->get_fetch_chunksize(s_ww.afe_data);
    if (s_ww.afe_iface->get_feed_channel_num(s_ww.afe_data) != channels) {
        ESP_LOGE(TAG, "AFE 输入通道数不匹配，停止初始化");
        wake_word_deinit();
        return ESP_ERR_INVALID_STATE;
    }
    s_ww.afe_iface->print_pipeline(s_ww.afe_data);
    ESP_LOGI(TAG, "AFE_ROUTE input=%s 16000Hz output=mono AEC=%d", input_format, channels == 3);
    ESP_LOGI(TAG, "AFE_CHUNK feed_samples_per_channel=%d channels=%d feed_i16=%u feed_bytes=%u fetch_samples=%u expected_feed_calls_5s=%.2f",
             feed_samples_per_channel, channels, (unsigned)s_ww.feed_chunk_size,
             (unsigned)(s_ww.feed_chunk_size * sizeof(int16_t)),
             (unsigned)s_ww.fetch_chunk_size, 80000.0 / feed_samples_per_channel);

    /* 喂入缓冲 */
    s_ww.feed_buffer = (int16_t *)heap_caps_malloc(
        s_ww.feed_chunk_size * sizeof(int16_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_ww.preroll_capacity = WAKE_PREROLL_SAMPLES;
    s_ww.preroll_buffer = (int16_t *)heap_caps_malloc(
        s_ww.preroll_capacity * sizeof(int16_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_ww.feed_buffer == NULL || s_ww.preroll_buffer == NULL) {
        ESP_LOGE(TAG, "分配唤醒词音频缓冲失败");
        wake_word_deinit();
        return ESP_ERR_NO_MEM;
    }

    s_ww.event_group = xEventGroupCreate();
    s_ww.output_lock = xSemaphoreCreateMutex();
    s_ww.output_storage = heap_caps_malloc(AFE_OUTPUT_BYTES + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_ww.output_storage != NULL) {
        s_ww.output_stream = xStreamBufferCreateStatic(
            AFE_OUTPUT_BYTES + 1, 1, s_ww.output_storage, &s_ww.output_stream_control);
    }
    if (s_ww.event_group == NULL || s_ww.output_lock == NULL || s_ww.output_stream == NULL) {
        ESP_LOGE(TAG, "创建事件组失败");
        wake_word_deinit();
        return ESP_ERR_NO_MEM;
    }

    BaseType_t task_ok = xTaskCreatePinnedToCore(
        detection_task, "wake_detect", 4096, NULL, 11,
        &s_ww.detection_task, 1);
    if (task_ok != pdPASS) {
        ESP_LOGE(TAG, "创建检测任务失败");
        wake_word_deinit();
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG,
             "唤醒词引擎初始化完成: %s, channels=%d, feed_i16=%u, fetch_mono_samples=%u, "
             "AFE=CPU1/P10, fetch_task=P11",
             s_ww.wake_words_str, s_ww.channels,
             (unsigned)s_ww.feed_chunk_size,
             (unsigned)s_ww.fetch_chunk_size);
    mem_contig_log("AFE_INIT_DONE");
#if CONFIG_WAKE_CLEANUP_TRANSIENT
    /* 初始化中途保留 worker，失败时无需再次申请 INTERNAL 栈即可安全清理。
     * 成功后也立即回收，普通待机不常驻清理任务；RTC 清理时再按需创建。 */
    xSemaphoreTake(s_lifecycle_lock, portMAX_DELAY);
    mem_contig_log("WAKE_CLEANUP_INIT_DELETE_BEFORE");
    vTaskDeleteWithCaps(s_lifecycle_task);
    s_lifecycle_task = NULL;
    mem_contig_log("WAKE_CLEANUP_INIT_DELETE_AFTER");
    xSemaphoreGive(s_lifecycle_lock);
#endif
    return ESP_OK;
}

void wake_word_set_callback(wake_word_callback_t callback, void *user_data)
{
    s_ww.callback = callback;
    s_ww.callback_user_data = user_data;
}

void wake_word_start(void)
{
    if (s_ww.afe_data == NULL || s_ww.event_group == NULL) {
        return;
    }
    s_ww.detected = false;
    xEventGroupSetBits(s_ww.event_group, WAKE_EVENT_RUNNING);
    __atomic_add_fetch(&s_ww.wake_generation, 1U, __ATOMIC_RELEASE);
    ESP_LOGD(TAG, "唤醒词检测已启动");
}

void wake_word_stop(void)
{
    if (s_ww.event_group == NULL) {
        return;
    }
    xEventGroupClearBits(s_ww.event_group, WAKE_EVENT_RUNNING);
    __atomic_add_fetch(&s_ww.wake_generation, 1U, __ATOMIC_RELEASE);
    ESP_LOGD(TAG, "唤醒词检测已停止");
}

size_t wake_word_get_feed_size(void)
{
    return s_ww.feed_chunk_size;
}

void wake_word_feed(const int16_t *data, size_t count)
{
    if (s_ww.afe_data == NULL || s_ww.event_group == NULL ||
        s_ww.feed_buffer == NULL) {
        return;
    }
    if (data == NULL || count % (size_t)s_ww.channels != 0) {
        stats_inc(&s_ww.feed_fail);
        return;
    }

    size_t remaining = count;
    const int16_t *src = data;

    /* 持久 accumulator 与 Opus 帧独立：每批全部拷入，满一块就 feed，
     * 循环处理下一块；不足一块的尾部留在 feed_buffer_count 中。 */
    while (remaining > 0) {
        size_t space = s_ww.feed_chunk_size - s_ww.feed_buffer_count;
        size_t copy = remaining < space ? remaining : space;

        memcpy(s_ww.feed_buffer + s_ww.feed_buffer_count, src,
               copy * sizeof(int16_t));
        s_ww.feed_buffer_count += copy;
        src += copy;
        remaining -= copy;

        if (s_ww.feed_buffer_count >= s_ww.feed_chunk_size) {
            xEventGroupSetBits(s_ww.event_group, WAKE_EVENT_FEED_STARTED);
            const int fed = s_ww.afe_iface->feed(s_ww.afe_data,
                                                 s_ww.feed_buffer);
            if (fed > 0) {
                stats_inc(&s_ww.feed_count);
                __atomic_add_fetch(&s_ww.feed_samples,
                    s_ww.feed_chunk_size / s_ww.channels, __ATOMIC_RELAXED);
            } else {
                stats_inc(&s_ww.feed_fail);
            }
            s_ww.feed_buffer_count = 0;
        }
    }
    __atomic_store_n(&s_ww.accumulator_remaining_samples,
                     s_ww.feed_buffer_count / s_ww.channels, __ATOMIC_RELAXED);
}

void wake_word_enable_voice_processing(bool enabled)
{
    if (s_ww.output_lock == NULL) return;
    xSemaphoreTake(s_ww.output_lock, portMAX_DELAY);
    if (s_ww.uplink_enabled != enabled) {
        s_ww.uplink_enabled = enabled;
        s_ww.output_generation++;
        /* 读写双方均持锁且不阻塞，切换时可以安全清空旧会话的输出。 */
        xStreamBufferReset(s_ww.output_stream);
    }
    xSemaphoreGive(s_ww.output_lock);
}

void wake_word_set_uplink(bool enabled)
{
    wake_word_enable_voice_processing(enabled);
}

bool wake_word_voice_detected(void)
{
    return __atomic_load_n(&s_ww.voice_detected, __ATOMIC_RELAXED);
}

size_t wake_word_read_pcm(int16_t *output, size_t samples)
{
    if (s_ww.output_lock == NULL || output == NULL) return 0;
    size_t bytes = samples * sizeof(int16_t);
    size_t received = 0;
    xSemaphoreTake(s_ww.output_lock, portMAX_DELAY);
    if (s_ww.uplink_enabled && xStreamBufferBytesAvailable(s_ww.output_stream) >= bytes) {
        received = xStreamBufferReceive(s_ww.output_stream, output, bytes, 0);
    }
    xSemaphoreGive(s_ww.output_lock);
    return received / sizeof(int16_t);
}

bool wake_word_is_detected(void)
{
    return s_ww.detected;
}

const char *wake_word_get_last(void)
{
    return s_ww.last_wake_word[0] != '\0' ? s_ww.last_wake_word : "unknown";
}

void wake_word_get_stats(wake_word_stats_t *stats)
{
    if (stats == NULL) {
        return;
    }
    stats->feed_count = __atomic_load_n(&s_ww.feed_count, __ATOMIC_RELAXED);
    stats->fetch_count = __atomic_load_n(&s_ww.fetch_count, __ATOMIC_RELAXED);
    stats->fetch_null_count =
        __atomic_load_n(&s_ww.fetch_null_count, __ATOMIC_RELAXED);
    stats->feed_fail = __atomic_load_n(&s_ww.feed_fail, __ATOMIC_RELAXED);
    stats->wake_detect_count =
        __atomic_load_n(&s_ww.wake_detect_count, __ATOMIC_RELAXED);
    stats->output_samples = __atomic_load_n(&s_ww.output_samples, __ATOMIC_RELAXED);
    stats->output_drop = __atomic_load_n(&s_ww.output_drop, __ATOMIC_RELAXED);
    stats->fetch_errors = __atomic_load_n(&s_ww.fetch_errors, __ATOMIC_RELAXED);
    stats->processed_samples = __atomic_load_n(&s_ww.processed_samples, __ATOMIC_RELAXED);
    stats->processed_peak = __atomic_exchange_n(&s_ww.processed_peak, 0U, __ATOMIC_RELAXED);
    stats->speech_frames = __atomic_load_n(&s_ww.speech_frames, __ATOMIC_RELAXED);
    stats->silence_frames = __atomic_load_n(&s_ww.silence_frames, __ATOMIC_RELAXED);
    stats->speech_begin_count = __atomic_load_n(&s_ww.speech_begin_count, __ATOMIC_RELAXED);
    stats->speech_end_count = __atomic_load_n(&s_ww.speech_end_count, __ATOMIC_RELAXED);
    stats->voice_detected = __atomic_load_n(&s_ww.voice_detected, __ATOMIC_RELAXED);
    stats->feed_samples = __atomic_load_n(&s_ww.feed_samples, __ATOMIC_RELAXED);
    stats->accumulator_remaining_samples = __atomic_load_n(&s_ww.accumulator_remaining_samples, __ATOMIC_RELAXED);
    stats->ringbuffer_underflow = __atomic_load_n(&s_ww.ringbuffer_underflow, __ATOMIC_RELAXED);
}

size_t wake_word_copy_preroll(int16_t *output, size_t max_samples)
{
    size_t samples = s_ww.preroll_count;
    if (output == NULL || max_samples == 0 || samples == 0 ||
        s_ww.preroll_capacity == 0) {
        return samples;
    }
    if (samples > max_samples) {
        samples = max_samples;
    }

    const size_t oldest =
        (s_ww.preroll_write + s_ww.preroll_capacity - s_ww.preroll_count) %
        s_ww.preroll_capacity;
    const size_t skip = s_ww.preroll_count - samples;
    size_t read = (oldest + skip) % s_ww.preroll_capacity;
    for (size_t i = 0; i < samples; i++) {
        output[i] = s_ww.preroll_buffer[read];
        read = (read + 1) % s_ww.preroll_capacity;
    }
    return samples;
}

esp_err_t wake_word_deinit(void)
{
    /* 尚未初始化时没有模型需要释放；失败清理同样禁止退回 PSRAM 栈 munmap。 */
    if (s_lifecycle_lock == NULL) return ESP_OK;
    return wake_lifecycle_call(false);
}

static void wake_word_deinit_internal(void)
{
    /* 局部变量地址反映实际运行栈；只观测，不关闭或绕过 IDF 的 cache 安全断言。 */
    const int64_t cleanup_begin_us = esp_timer_get_time();
    ESP_LOGI(TAG, "WAKE_CLEANUP begin task=%s stack_caps=%s stack_high_water=%u",
             pcTaskGetName(NULL),
             esp_ptr_internal(&cleanup_begin_us) ? "INTERNAL" : "EXTERNAL",
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
    if (s_ww.detection_task != NULL) {
        /* 调用方已停止 feed；等待 fetch 自行退出，不能删除仍持输出锁的任务。 */
        __atomic_store_n(&s_ww.stopping, true, __ATOMIC_RELEASE);
        xEventGroupSetBits(s_ww.event_group, WAKE_EVENT_FEED_STARTED);
        xEventGroupWaitBits(s_ww.event_group, WAKE_EVENT_EXITED,
                           pdFALSE, pdTRUE, portMAX_DELAY);
        s_ww.detection_task = NULL;
    }
    if (s_ww.event_group != NULL) {
        vEventGroupDelete(s_ww.event_group);
        s_ww.event_group = NULL;
    }
    if (s_ww.output_stream != NULL) {
        vStreamBufferDelete(s_ww.output_stream);
        s_ww.output_stream = NULL;
    }
    heap_caps_free(s_ww.output_storage);
    s_ww.output_storage = NULL;
    if (s_ww.output_lock != NULL) {
        vSemaphoreDelete(s_ww.output_lock);
        s_ww.output_lock = NULL;
    }
    if (s_ww.feed_buffer != NULL) {
        heap_caps_free(s_ww.feed_buffer);
        s_ww.feed_buffer = NULL;
    }
    if (s_ww.preroll_buffer != NULL) {
        heap_caps_free(s_ww.preroll_buffer);
        s_ww.preroll_buffer = NULL;
    }
    if (s_ww.afe_data != NULL) {
        s_ww.afe_iface->destroy(s_ww.afe_data);
        s_ww.afe_data = NULL;
    }
    if (s_ww.models != NULL) {
        ESP_LOGI(TAG, "WAKE_CLEANUP srmodel_deinit begin");
        esp_srmodel_deinit(s_ww.models);
        ESP_LOGI(TAG, "WAKE_CLEANUP srmodel_deinit done");
        s_ww.models = NULL;
    }
    ESP_LOGI(TAG, "WAKE_CLEANUP total_us=%lld stack_high_water=%u",
             (long long)(esp_timer_get_time() - cleanup_begin_us),
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
    ESP_LOGI(TAG, "唤醒词引擎已释放");
    mem_contig_log("AFE_WAKE_RELEASED");
}
