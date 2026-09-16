#include "wake_word.h"

#include <string.h>

#include "esp_afe_sr_models.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

static const char *TAG = "WAKE_WORD";

#define WAKE_EVENT_RUNNING  BIT0
#define WAKE_PREROLL_SAMPLE_RATE 16000U
#define WAKE_PREROLL_SECONDS     2U
#define WAKE_PREROLL_SAMPLES     (WAKE_PREROLL_SAMPLE_RATE * WAKE_PREROLL_SECONDS)

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
} wake_word_ctx_t;

static wake_word_ctx_t s_ww;

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

static void detection_task(void *arg)
{
    (void)arg;
    wake_word_ctx_t *ctx = &s_ww;

    ESP_LOGI(TAG, "检测任务已启动，feed=%u fetch=%u",
             (unsigned)ctx->feed_chunk_size,
             (unsigned)ctx->fetch_chunk_size);

    while (ctx->afe_data != NULL) {
        xEventGroupWaitBits(ctx->event_group, WAKE_EVENT_RUNNING,
                            pdFALSE, pdTRUE, portMAX_DELAY);

        afe_fetch_result_t *afe_result = ctx->afe_iface->fetch_with_delay(
            ctx->afe_data, portMAX_DELAY);

        if (afe_result == NULL || afe_result->ret_value == ESP_FAIL) {
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
            ESP_LOGI(TAG, "检测到唤醒词: %s", ctx->last_wake_word);

            wake_word_stop();

            if (ctx->callback != NULL) {
                ctx->callback(ctx->last_wake_word, ctx->callback_user_data);
            }
        }
    }

    ESP_LOGW(TAG, "检测任务已退出");
    vTaskDelete(NULL);
}

esp_err_t wake_word_init(int channels)
{
    if (s_ww.afe_data != NULL) {
        ESP_LOGW(TAG, "唤醒词引擎已初始化，跳过");
        return ESP_OK;
    }

    memset(&s_ww, 0, sizeof(s_ww));
    s_ww.channels = channels <= 0 ? 1 : channels;

    /* 从 SPIFFS 加载语音识别模型。模型文件通过 esp-sr 组件提供，
     * 运行时从 "model" 前缀的 SPIFFS 分区读取。                     */
    s_ww.models = esp_srmodel_init("model");
    if (s_ww.models == NULL || s_ww.models->num <= 0) {
        ESP_LOGE(TAG, "加载 Wakenet 模型失败");
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
        esp_srmodel_deinit(s_ww.models);
        s_ww.models = NULL;
        return ESP_ERR_NOT_SUPPORTED;
    }

    /* 构造 AFE 输入格式字符串：每声道一个 'M' */
    char input_format[16];
    memset(input_format, 'M', s_ww.channels);
    input_format[s_ww.channels] = '\0';

    afe_config_t *afe_config = afe_config_init(
        input_format, s_ww.models, AFE_TYPE_SR, AFE_MODE_HIGH_PERF);
    if (afe_config == NULL) {
        ESP_LOGE(TAG, "创建 AFE 配置失败");
        esp_srmodel_deinit(s_ww.models);
        s_ww.models = NULL;
        return ESP_FAIL;
    }

    afe_config->aec_init = false;
    afe_config->afe_perferred_core = 1;
    afe_config->afe_perferred_priority = 3;
    afe_config->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;

    s_ww.afe_iface = esp_afe_handle_from_config(afe_config);
    s_ww.afe_data = s_ww.afe_iface->create_from_config(afe_config);
    if (s_ww.afe_data == NULL) {
        ESP_LOGE(TAG, "创建 AFE 数据实例失败");
        esp_srmodel_deinit(s_ww.models);
        s_ww.models = NULL;
        return ESP_FAIL;
    }

    s_ww.feed_chunk_size =
        s_ww.afe_iface->get_feed_chunksize(s_ww.afe_data) * s_ww.channels;
    s_ww.fetch_chunk_size =
        s_ww.afe_iface->get_fetch_chunksize(s_ww.afe_data);

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
    if (s_ww.event_group == NULL) {
        ESP_LOGE(TAG, "创建事件组失败");
        wake_word_deinit();
        return ESP_ERR_NO_MEM;
    }

    BaseType_t task_ok = xTaskCreatePinnedToCore(
        detection_task, "wake_detect", 4096, NULL, 3,
        &s_ww.detection_task, 1);
    if (task_ok != pdPASS) {
        ESP_LOGE(TAG, "创建检测任务失败");
        wake_word_deinit();
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "唤醒词引擎初始化完成: %s, channels=%d, feed=%u, fetch=%u",
             s_ww.wake_words_str, s_ww.channels,
             (unsigned)s_ww.feed_chunk_size,
             (unsigned)s_ww.fetch_chunk_size);
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
    s_ww.feed_buffer_count = 0;
    s_ww.preroll_count = 0;
    s_ww.preroll_write = 0;
    if (s_ww.afe_iface != NULL && s_ww.afe_data != NULL) {
        s_ww.afe_iface->reset_buffer(s_ww.afe_data);
    }
    xEventGroupSetBits(s_ww.event_group, WAKE_EVENT_RUNNING);
    ESP_LOGD(TAG, "唤醒词检测已启动");
}

void wake_word_stop(void)
{
    if (s_ww.event_group == NULL) {
        return;
    }
    xEventGroupClearBits(s_ww.event_group, WAKE_EVENT_RUNNING);
    s_ww.feed_buffer_count = 0;
    if (s_ww.afe_iface != NULL && s_ww.afe_data != NULL) {
        s_ww.afe_iface->reset_buffer(s_ww.afe_data);
    }
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

    if (!(xEventGroupGetBits(s_ww.event_group) & WAKE_EVENT_RUNNING)) {
        return;
    }

    size_t remaining = count;
    const int16_t *src = data;

    while (remaining > 0) {
        size_t space = s_ww.feed_chunk_size - s_ww.feed_buffer_count;
        size_t copy = remaining < space ? remaining : space;

        memcpy(s_ww.feed_buffer + s_ww.feed_buffer_count, src,
               copy * sizeof(int16_t));
        s_ww.feed_buffer_count += copy;
        src += copy;
        remaining -= copy;

        if (s_ww.feed_buffer_count >= s_ww.feed_chunk_size) {
            s_ww.afe_iface->feed(s_ww.afe_data, s_ww.feed_buffer);
            s_ww.feed_buffer_count = 0;
        }
    }
}

bool wake_word_is_detected(void)
{
    return s_ww.detected;
}

const char *wake_word_get_last(void)
{
    return s_ww.last_wake_word[0] != '\0' ? s_ww.last_wake_word : "unknown";
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

void wake_word_deinit(void)
{
    if (s_ww.detection_task != NULL) {
        vTaskDelete(s_ww.detection_task);
        s_ww.detection_task = NULL;
    }
    if (s_ww.event_group != NULL) {
        vEventGroupDelete(s_ww.event_group);
        s_ww.event_group = NULL;
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
        esp_srmodel_deinit(s_ww.models);
        s_ww.models = NULL;
    }
    ESP_LOGI(TAG, "唤醒词引擎已释放");
}
