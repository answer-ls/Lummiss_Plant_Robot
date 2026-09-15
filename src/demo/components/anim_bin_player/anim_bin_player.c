#include "anim_bin_player.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "ANIM_BIN";

#define ANIM_MAGIC                 "LUM1"
#define ANIM_VERSION               1U
#define ANIM_PIXEL_FORMAT_RGB565   1U
#define ANIM_MAX_WIDTH             320U
#define ANIM_MAX_HEIGHT            240U
#define ANIM_BUFFER_SIZE           (ANIM_MAX_WIDTH * ANIM_MAX_HEIGHT * 2U)
#define ANIM_SD_READ_CHUNK         (4U * 1024U)
#define ANIM_MAX_FRAMES            1024U
#define ANIM_PATH_MAX              128U
#define ANIM_TASK_STACK            8192U
#define ANIM_TASK_PRIORITY         3U
#define ANIM_TASK_CORE             1U
#define ANIM_UI_TIMER_MS           5U
#define ANIM_WAIT_SLICE_MS         20U
#define ANIM_STATS_PERIOD_US       (5LL * 1000LL * 1000LL)

#pragma pack(push, 1)
typedef struct {
    char magic[4];           /* 固定为 LUM1 */
    uint16_t version;        /* 当前版本为 1 */
    uint16_t width;          /* 完整逻辑帧宽度 */
    uint16_t height;         /* 完整逻辑帧高度 */
    uint16_t frame_count;    /* Frame Table 条目数 */
    uint8_t pixel_format;    /* 1 表示 RGB565 */
    uint8_t flags;           /* 版本 1 必须为 0 */
    uint16_t loop_count;     /* 0 表示无限循环 */
} anim_bin_header_t;

typedef struct {
    uint32_t offset;         /* 帧数据相对文件头的绝对偏移 */
    uint32_t size;           /* 完整 RGB565 帧字节数 */
    uint16_t delay_ms;       /* 当前帧显示时长 */
    uint16_t reserved;       /* 版本 1 必须为 0 */
} anim_bin_frame_t;
#pragma pack(pop)

_Static_assert(sizeof(anim_bin_header_t) == 16, "LUM1 header must be 16 bytes");
_Static_assert(sizeof(anim_bin_frame_t) == 12, "LUM1 frame entry must be 12 bytes");

typedef struct {
    uint64_t read_total_us;
    uint64_t read_max_us;
    uint64_t switch_total_us;
    uint64_t switch_max_us;
    uint32_t read_count;
    uint32_t switch_count;
    uint32_t deadline_miss;
    int64_t log_start_us;
} anim_stats_t;

static SemaphoreHandle_t s_state_mutex;
static SemaphoreHandle_t s_frame_ack;
static TaskHandle_t s_player_task;
static lv_obj_t *s_image;
static lv_timer_t *s_ui_timer;
static uint8_t *s_buffers[2];
static uint8_t *s_sd_read_buffer;
static lv_img_dsc_t s_descriptors[2];

static bool s_initialized;
static bool s_play_request;
static bool s_loading;
static bool s_playing;
static uint32_t s_generation;
static char s_requested_path[ANIM_PATH_MAX];
static esp_err_t s_last_error = ESP_OK;
static uint16_t s_frame_index;
static int s_active_buffer = -1;

/* 只有播放器任务写 pending；只有 LVGL Timer 消费。状态互斥量保证 stop
 * 与切帧不能交错，LVGL 永远不会看到正在被 fread 覆盖的 Buffer。 */
static bool s_frame_pending;
static int s_pending_buffer;
static uint16_t s_pending_frame_index;
static uint32_t s_pending_generation;

static bool generation_is_current(uint32_t generation)
{
    bool current;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    current = generation == s_generation;
    xSemaphoreGive(s_state_mutex);
    return current;
}

static void set_run_state(uint32_t generation, bool loading, bool playing,
                          esp_err_t error)
{
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    if (generation == s_generation) {
        s_loading = loading;
        s_playing = playing;
        s_last_error = error;
    }
    xSemaphoreGive(s_state_mutex);
}

static void ui_frame_switch_timer(lv_timer_t *timer)
{
    (void)timer;
    bool acknowledge = false;

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    if (s_frame_pending) {
        if (s_pending_generation == s_generation) {
            const int buffer_index = s_pending_buffer;
            lv_img_cache_invalidate_src(&s_descriptors[buffer_index]);
            lv_img_set_src(s_image, &s_descriptors[buffer_index]);
            lv_obj_set_size(s_image,
                            (lv_coord_t)s_descriptors[buffer_index].header.w,
                            (lv_coord_t)s_descriptors[buffer_index].header.h);
            lv_obj_center(s_image);
            s_active_buffer = buffer_index;
            s_frame_index = s_pending_frame_index;
            s_loading = false;
            s_playing = true;
        }
        s_frame_pending = false;
        acknowledge = true;
    }
    xSemaphoreGive(s_state_mutex);

    if (acknowledge) {
        xSemaphoreGive(s_frame_ack);
    }
}

static esp_err_t validate_file(FILE *file, size_t file_size,
                               anim_bin_header_t *header,
                               anim_bin_frame_t **frames_out)
{
    if (file_size < sizeof(*header) ||
        fread(header, 1, sizeof(*header), file) != sizeof(*header)) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (memcmp(header->magic, ANIM_MAGIC, sizeof(header->magic)) != 0) {
        ESP_LOGE(TAG, "LUM1 magic 错误");
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (header->version != ANIM_VERSION) {
        ESP_LOGE(TAG, "LUM1 version 不支持：%u", header->version);
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (header->pixel_format != ANIM_PIXEL_FORMAT_RGB565 || header->flags != 0) {
        ESP_LOGE(TAG, "LUM1 像素格式/flags 不支持：format=%u flags=%u",
                 header->pixel_format, header->flags);
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (header->width == 0 || header->height == 0 ||
        header->width > ANIM_MAX_WIDTH || header->height > ANIM_MAX_HEIGHT) {
        ESP_LOGE(TAG, "LUM1 尺寸非法：%ux%u", header->width, header->height);
        return ESP_ERR_INVALID_SIZE;
    }
    if (header->frame_count == 0 || header->frame_count > ANIM_MAX_FRAMES) {
        ESP_LOGE(TAG, "LUM1 帧数非法：%u", header->frame_count);
        return ESP_ERR_INVALID_SIZE;
    }

    const size_t table_size = (size_t)header->frame_count * sizeof(anim_bin_frame_t);
    const size_t table_end = sizeof(*header) + table_size;
    if (table_end > file_size) {
        ESP_LOGE(TAG, "LUM1 Frame Table 越界");
        return ESP_ERR_INVALID_SIZE;
    }

    anim_bin_frame_t *frames = heap_caps_malloc(
        table_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (frames == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (fread(frames, 1, table_size, file) != table_size) {
        heap_caps_free(frames);
        return ESP_ERR_INVALID_SIZE;
    }

    const uint32_t expected_size =
        (uint32_t)header->width * (uint32_t)header->height * 2U;
    uint64_t previous_end = table_end;
    for (uint16_t i = 0; i < header->frame_count; i++) {
        const uint64_t frame_end = (uint64_t)frames[i].offset + frames[i].size;
        if (frames[i].reserved != 0 || frames[i].size != expected_size ||
            frames[i].offset < table_end || frames[i].offset != previous_end ||
            frame_end > file_size) {
            ESP_LOGE(TAG,
                     "LUM1 第 %u 帧非法：offset=%" PRIu32 " size=%" PRIu32,
                     i, frames[i].offset, frames[i].size);
            heap_caps_free(frames);
            return ESP_ERR_INVALID_SIZE;
        }
        previous_end = frame_end;
    }
    if (previous_end != file_size) {
        ESP_LOGE(TAG, "LUM1 文件尾不匹配：table_end=%" PRIu64 " file=%u",
                 previous_end, (unsigned)file_size);
        heap_caps_free(frames);
        return ESP_ERR_INVALID_SIZE;
    }

    *frames_out = frames;
    return ESP_OK;
}

static esp_err_t read_frame(FILE *file, const anim_bin_frame_t *frame,
                            uint8_t *buffer, anim_stats_t *stats)
{
    const int64_t start_us = esp_timer_get_time();
    /* 正常帧按文件顺序连续读取，仅循环回到第 0 帧时重新定位。 */
    const long current_offset = ftell(file);
    if ((current_offset < 0 || (uint32_t)current_offset != frame->offset) &&
        fseek(file, (long)frame->offset, SEEK_SET) != 0) {
        return ESP_FAIL;
    }
    /* FatFS 会把大块 fread 的目标直接交给 SDMMC。LUM1 数据偏移通常不满足
     * PSRAM 的 128 字节 DMA 对齐，直接读整帧会临时申请接近 150 KB 的内部
     * bounce buffer。固定 4 KB 内部 DMA Buffer 可避免该申请和运行期失败。 */
    size_t total_read = 0;
    while (total_read < frame->size) {
        const size_t remaining = frame->size - total_read;
        const size_t chunk = remaining < ANIM_SD_READ_CHUNK
                                 ? remaining
                                 : ANIM_SD_READ_CHUNK;
        const size_t read_size = fread(s_sd_read_buffer, 1, chunk, file);
        if (read_size == 0) {
            break;
        }
        memcpy(buffer + total_read, s_sd_read_buffer, read_size);
        total_read += read_size;
        if (read_size != chunk) {
            break;
        }
    }
    const uint64_t elapsed_us = (uint64_t)(esp_timer_get_time() - start_us);
    stats->read_total_us += elapsed_us;
    stats->read_count++;
    if (elapsed_us > stats->read_max_us) {
        stats->read_max_us = elapsed_us;
    }
    if (total_read != frame->size) {
        ESP_LOGE(TAG, "SD 帧读取不足：需要 %" PRIu32 "，实际 %u，errno=%d",
                 frame->size, (unsigned)total_read, errno);
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

static esp_err_t publish_frame(uint32_t generation, int buffer_index,
                               uint16_t frame_index, uint16_t width,
                               uint16_t height, uint32_t frame_size,
                               anim_stats_t *stats)
{
    while (xSemaphoreTake(s_frame_ack, 0) == pdTRUE) {
    }

    s_descriptors[buffer_index].header.always_zero = 0;
    s_descriptors[buffer_index].header.w = width;
    s_descriptors[buffer_index].header.h = height;
    s_descriptors[buffer_index].header.cf = LV_IMG_CF_TRUE_COLOR;
    s_descriptors[buffer_index].data_size = frame_size;
    s_descriptors[buffer_index].data = s_buffers[buffer_index];

    const int64_t start_us = esp_timer_get_time();
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    if (generation != s_generation) {
        xSemaphoreGive(s_state_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    s_pending_buffer = buffer_index;
    s_pending_frame_index = frame_index;
    s_pending_generation = generation;
    s_frame_pending = true;
    xSemaphoreGive(s_state_mutex);

    while (generation_is_current(generation)) {
        if (xSemaphoreTake(s_frame_ack,
                           pdMS_TO_TICKS(ANIM_WAIT_SLICE_MS)) == pdTRUE) {
            if (!generation_is_current(generation)) {
                return ESP_ERR_INVALID_STATE;
            }
            const uint64_t elapsed_us =
                (uint64_t)(esp_timer_get_time() - start_us);
            stats->switch_total_us += elapsed_us;
            stats->switch_count++;
            if (elapsed_us > stats->switch_max_us) {
                stats->switch_max_us = elapsed_us;
            }
            return ESP_OK;
        }
    }
    return ESP_ERR_INVALID_STATE;
}

#ifndef CONFIG_LUMMISS_ANIM_FIRST_FRAME_ONLY
static bool wait_until(uint32_t generation, int64_t deadline_us)
{
    while (generation_is_current(generation)) {
        const int64_t remaining_us = deadline_us - esp_timer_get_time();
        if (remaining_us <= 0) {
            return true;
        }
        TickType_t ticks = pdMS_TO_TICKS((remaining_us + 999) / 1000);
        const TickType_t slice = pdMS_TO_TICKS(ANIM_WAIT_SLICE_MS);
        if (ticks > slice) {
            ticks = slice;
        }
        if (ticks == 0) {
            ticks = 1;
        }
        ulTaskNotifyTake(pdTRUE, ticks);
    }
    return false;
}

static void log_stats(const anim_stats_t *stats, uint16_t frame_index, bool force)
{
    const int64_t now_us = esp_timer_get_time();
    const int64_t elapsed_us = now_us - stats->log_start_us;
    if (!force && elapsed_us < ANIM_STATS_PERIOD_US) {
        return;
    }

    const double fps = elapsed_us > 0
                           ? (double)stats->switch_count * 1000000.0 /
                                 (double)elapsed_us
                           : 0.0;
    const double read_avg_ms = stats->read_count > 0
                                   ? (double)stats->read_total_us /
                                         (double)stats->read_count / 1000.0
                                   : 0.0;
    const double switch_avg_ms = stats->switch_count > 0
                                     ? (double)stats->switch_total_us /
                                           (double)stats->switch_count / 1000.0
                                     : 0.0;
    ESP_LOGI(TAG,
             "FPS=%.1f frame=%u SD avg/max=%.2f/%.2f ms switch avg/max=%.2f/%.2f ms "
             "deadline_miss=%" PRIu32 " PSRAM=%u KB internal=%u KB",
             fps, frame_index, read_avg_ms, (double)stats->read_max_us / 1000.0,
             switch_avg_ms, (double)stats->switch_max_us / 1000.0,
             stats->deadline_miss,
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024U),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024U));
}
#endif

static esp_err_t play_file(const char *path, uint32_t generation)
{
    struct stat file_stat;
    if (stat(path, &file_stat) != 0 || file_stat.st_size <= 0) {
        ESP_LOGE(TAG, "BIN 不存在或无法 stat：%s (%s)", path, strerror(errno));
        return ESP_ERR_NOT_FOUND;
    }
    if ((uint64_t)file_stat.st_size > UINT32_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }

    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        ESP_LOGE(TAG, "fopen 失败：%s (%s)", path, strerror(errno));
        return ESP_FAIL;
    }
    anim_bin_header_t header;
    anim_bin_frame_t *frames = NULL;
    esp_err_t result = validate_file(file, (size_t)file_stat.st_size,
                                     &header, &frames);
    if (result != ESP_OK) {
        fclose(file);
        return result;
    }

    ESP_LOGI(TAG, "LUM1 已解析：%s，%ux%u，%u 帧，loop=%u，RGB565-BE",
             path, header.width, header.height, header.frame_count,
             header.loop_count);

    anim_stats_t stats = {
        .log_start_us = esp_timer_get_time(),
    };
    uint16_t frame_index = 0;
    int active_buffer;

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    active_buffer = s_active_buffer == 0 ? 1 : 0;
    xSemaphoreGive(s_state_mutex);

    result = read_frame(file, &frames[frame_index],
                        s_buffers[active_buffer], &stats);
    if (result == ESP_OK) {
        result = publish_frame(generation, active_buffer, frame_index,
                               header.width, header.height,
                               frames[frame_index].size, &stats);
    }
    if (result == ESP_OK) {
        ESP_LOGI(TAG, "首帧已显示：frame=%u buffer=%c",
                 frame_index, active_buffer == 0 ? 'A' : 'B');
    }

#ifdef CONFIG_LUMMISS_ANIM_FIRST_FRAME_ONLY
    if (result == ESP_OK) {
        ESP_LOGW(TAG, "Stage 6 首帧验证模式：保持 Frame 0，不继续读取");
        while (generation_is_current(generation)) {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(ANIM_WAIT_SLICE_MS));
        }
        result = ESP_ERR_INVALID_STATE;
    }
#else
    uint32_t completed_loops = 0;
    int64_t displayed_at_us = esp_timer_get_time();
    while (result == ESP_OK && generation_is_current(generation)) {
        uint16_t next_frame = frame_index + 1U;
        if (next_frame >= header.frame_count) {
            next_frame = 0;
            completed_loops++;
            if (header.loop_count != 0 &&
                completed_loops >= header.loop_count) {
                const int64_t final_deadline_us = displayed_at_us +
                    (int64_t)frames[frame_index].delay_ms * 1000LL;
                wait_until(generation, final_deadline_us);
                result = ESP_OK;
                break;
            }
        }

        /* active_buffer 仍由 LVGL 使用，只能把下一帧读入另一块。 */
        const int next_buffer = active_buffer == 0 ? 1 : 0;
        result = read_frame(file, &frames[next_frame],
                            s_buffers[next_buffer], &stats);
        if (result != ESP_OK) {
            break;
        }

        const int64_t deadline_us = displayed_at_us +
                                    (int64_t)frames[frame_index].delay_ms * 1000LL;
        if (esp_timer_get_time() > deadline_us) {
            stats.deadline_miss++;
        } else if (!wait_until(generation, deadline_us)) {
            result = ESP_ERR_INVALID_STATE;
            break;
        }

        result = publish_frame(generation, next_buffer, next_frame,
                               header.width, header.height,
                               frames[next_frame].size, &stats);
        if (result != ESP_OK) {
            break;
        }
        active_buffer = next_buffer;
        frame_index = next_frame;
        displayed_at_us = esp_timer_get_time();
        log_stats(&stats, frame_index, false);
        if (esp_timer_get_time() - stats.log_start_us >= ANIM_STATS_PERIOD_US) {
            memset(&stats, 0, sizeof(stats));
            stats.log_start_us = esp_timer_get_time();
        }
    }

    /* 每个轮播槽约 5 秒，stop 可能恰好先于周期日志；退出前补打一条本段统计。 */
    if (stats.switch_count > 0 &&
        esp_timer_get_time() - stats.log_start_us > 1000000LL) {
        log_stats(&stats, frame_index, true);
    }
#endif

    heap_caps_free(frames);
    fclose(file);
    return result;
}

static bool take_play_request(char *path, uint32_t *generation)
{
    bool available = false;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    if (s_play_request) {
        memcpy(path, s_requested_path, sizeof(s_requested_path));
        *generation = s_generation;
        s_play_request = false;
        s_loading = true;
        s_playing = false;
        s_last_error = ESP_OK;
        available = true;
    }
    xSemaphoreGive(s_state_mutex);
    return available;
}

static void player_task(void *arg)
{
    (void)arg;
    char path[ANIM_PATH_MAX];

    for (;;) {
        uint32_t generation = 0;
        if (!take_play_request(path, &generation)) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }

        esp_err_t result = play_file(path, generation);
        if (!generation_is_current(generation)) {
            continue;
        }
        if (result == ESP_OK) {
            set_run_state(generation, false, false, ESP_OK);
            ESP_LOGI(TAG, "动画按 loop_count 播放完成：%s", path);
        } else if (result != ESP_ERR_INVALID_STATE) {
            set_run_state(generation, false, false, result);
            ESP_LOGE(TAG, "动画播放失败：%s", esp_err_to_name(result));
        }
    }
}

esp_err_t anim_bin_player_init(lv_obj_t *parent)
{
    if (parent == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_initialized) {
        return ESP_OK;
    }

    s_state_mutex = xSemaphoreCreateMutex();
    s_frame_ack = xSemaphoreCreateBinary();
    if (s_state_mutex == NULL || s_frame_ack == NULL) {
        goto no_memory;
    }

    s_sd_read_buffer = heap_caps_malloc(
        ANIM_SD_READ_CHUNK,
        MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    if (s_sd_read_buffer == NULL) {
        goto no_memory;
    }

    for (int i = 0; i < 2; i++) {
        s_buffers[i] = heap_caps_malloc(
            ANIM_BUFFER_SIZE,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_buffers[i] == NULL) {
            goto no_memory;
        }
        memset(s_buffers[i], 0, ANIM_BUFFER_SIZE);
    }

    s_image = lv_img_create(parent);
    if (s_image == NULL) {
        goto no_memory;
    }
    lv_obj_center(s_image);
    s_ui_timer = lv_timer_create(ui_frame_switch_timer, ANIM_UI_TIMER_MS, NULL);
    if (s_ui_timer == NULL) {
        goto no_memory;
    }

    const BaseType_t created = xTaskCreatePinnedToCore(
        player_task, "anim_player", ANIM_TASK_STACK, NULL,
        ANIM_TASK_PRIORITY, &s_player_task, ANIM_TASK_CORE);
    if (created != pdPASS) {
        goto no_memory;
    }

    s_initialized = true;
    ESP_LOGI(TAG,
             "播放器已初始化：双 Buffer A/B，各 %u bytes，PSRAM 常驻 %u KB，"
             "SD 内部读缓存 %u KB",
             ANIM_BUFFER_SIZE, (2U * ANIM_BUFFER_SIZE) / 1024U,
             ANIM_SD_READ_CHUNK / 1024U);
    return ESP_OK;

no_memory:
    if (s_ui_timer != NULL) {
        lv_timer_del(s_ui_timer);
        s_ui_timer = NULL;
    }
    if (s_image != NULL) {
        lv_obj_del(s_image);
        s_image = NULL;
    }
    for (int i = 0; i < 2; i++) {
        if (s_buffers[i] != NULL) {
            heap_caps_free(s_buffers[i]);
            s_buffers[i] = NULL;
        }
    }
    if (s_sd_read_buffer != NULL) {
        heap_caps_free(s_sd_read_buffer);
        s_sd_read_buffer = NULL;
    }
    if (s_frame_ack != NULL) {
        vSemaphoreDelete(s_frame_ack);
        s_frame_ack = NULL;
    }
    if (s_state_mutex != NULL) {
        vSemaphoreDelete(s_state_mutex);
        s_state_mutex = NULL;
    }
    return ESP_ERR_NO_MEM;
}

esp_err_t anim_bin_player_play(const char *path)
{
    if (!s_initialized || path == NULL || path[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    const size_t path_len = strlen(path);
    if (path_len >= sizeof(s_requested_path)) {
        return ESP_ERR_INVALID_SIZE;
    }

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_generation++;
    memcpy(s_requested_path, path, path_len + 1U);
    s_play_request = true;
    s_loading = true;
    s_playing = false;
    s_last_error = ESP_OK;
    s_frame_pending = false;
    xSemaphoreGive(s_state_mutex);

    xSemaphoreGive(s_frame_ack);
    xTaskNotifyGive(s_player_task);
    return ESP_OK;
}

void anim_bin_player_stop(void)
{
    if (!s_initialized) {
        return;
    }
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_generation++;
    s_play_request = false;
    s_loading = false;
    s_playing = false;
    s_last_error = ESP_OK;
    s_frame_pending = false;
    xSemaphoreGive(s_state_mutex);

    xSemaphoreGive(s_frame_ack);
    xTaskNotifyGive(s_player_task);
}

bool anim_bin_player_is_playing(void)
{
    if (!s_initialized) {
        return false;
    }
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    const bool playing = s_playing;
    xSemaphoreGive(s_state_mutex);
    return playing;
}

bool anim_bin_player_is_loading(void)
{
    if (!s_initialized) {
        return false;
    }
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    const bool loading = s_loading;
    xSemaphoreGive(s_state_mutex);
    return loading;
}

esp_err_t anim_bin_player_get_last_error(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    const esp_err_t error = s_last_error;
    xSemaphoreGive(s_state_mutex);
    return error;
}

uint16_t anim_bin_player_get_frame_index(void)
{
    if (!s_initialized) {
        return 0;
    }
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    const uint16_t frame_index = s_frame_index;
    xSemaphoreGive(s_state_mutex);
    return frame_index;
}
