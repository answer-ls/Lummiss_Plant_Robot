#include "screen_carousel.h"

#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lvgl.h"
/* lvgl 组件同时导出了 src/，按相对路径包含即可拿到 lv_gif 的公开结构。 */
#include "extra/libs/gif/lv_gif.h"

#include "sd_card.h"

static const char *TAG = "CAROUSEL";

#define CAROUSEL_TASK_STACK     6144
#define CAROUSEL_TASK_PRIORITY  5
#define CAROUSEL_PREFETCH_STACK  8192
#define CAROUSEL_PREFETCH_PRIORITY 2
#define CAROUSEL_PREFETCH_CORE  1
#define CAROUSEL_PREFETCH_DEPTH 2
#define CAROUSEL_PREFETCH_CHUNK  (32U * 1024U)

/* 每个画面停留时长，天气首页和每个 GIF 都用这个值。 */
#define CAROUSEL_SLOT_MS        5000

/* 优先在这个子目录里找素材，没有就退回卡根目录。 */
#define CAROUSEL_DIR_PRIMARY    SD_CARD_MOUNT_POINT "/expressions"
#define CAROUSEL_DIR_FALLBACK   SD_CARD_MOUNT_POINT

/* 素材数量上限。目前是 8 个表情，留足余量到 64。
 * 上限之外的会被忽略并打一条警告，不会静默截断。 */
#define CAROUSEL_MAX_GIFS       64
/* 单个 VFS 路径长度上限，形如 /sdcard/expressions/exp_01.gif。 */
#define CAROUSEL_PATH_MAX       96

/* 轮播状态。
 *   slot == 0          显示天气首页
 *   slot == 1..gif_count  显示第 (slot - 1) 个 GIF
 * 天气首页在 display_driver_start() 里已经显示出来了，所以初值直接给 1，
 * 定时器第一次触发就是切到第一个 GIF，不用先空转一轮。 */
static uint32_t s_slot = 1;

static char s_gif_paths[CAROUSEL_MAX_GIFS][CAROUSEL_PATH_MAX];
static uint32_t s_gif_count;

static lv_obj_t *s_home_screen;   /* display_driver 建的默认屏幕 */
static lv_obj_t *s_gif_screen;    /* 本模块自建的 GIF 屏，全程复用 */
static lv_obj_t *s_gif_obj;       /* 屏里唯一的 lv_gif 对象，切素材只换 src */

/*
 * lv_gif 的文件源会在 LVGL 定时器回调里调用 lv_fs_read/lv_fs_seek，
 * 这会把 FAT/SDMMC 的长尾延迟带进 taskLVGL。预读队列传递的是完整的
 * GIF 内存块，所有权从预读任务转移给 LVGL；当前 GIF 切换完成后才释放
 * 上一个内存块，保证 gd_GIF.data 在整个播放期间有效。
 */
typedef struct {
    uint32_t index;
    uint8_t *data;
    size_t size;
    bool ready;
} gif_prefetch_item_t;

typedef enum {
    GIF_SHOW_NOT_READY = 0,
    GIF_SHOW_FAILED,
    GIF_SHOW_OK,
} gif_show_result_t;

static QueueHandle_t s_prefetch_queue;
static uint8_t *s_active_gif_data;

/* 大小写无关的字典序比较，用于让轮播顺序可预期（exp_01 排在 exp_02 前）。
 * 不用 strcasecmp 是因为它在 IDF 的 newlib 头里归属不定，
 * 这里避免引入 <strings.h> 依赖。 */
static int compare_names(const char *a, const char *b)
{
    while (*a != '\0' && *b != '\0') {
        char ca = *a;
        char cb = *b;
        if (ca >= 'A' && ca <= 'Z') {
            ca = (char)(ca - 'A' + 'a');
        }
        if (cb >= 'A' && cb <= 'Z') {
            cb = (char)(cb - 'A' + 'a');
        }
        if (ca != cb) {
            return (int)(unsigned char)ca - (int)(unsigned char)cb;
        }
        a++;
        b++;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

static bool has_gif_suffix(const char *name)
{
    const size_t len = strlen(name);
    return len >= 4 && compare_names(name + len - 4, ".gif") == 0;
}

/* 把已收集到的路径按文件名排序。插入排序，元素最多 64 个，开销可忽略。 */
static void sort_gif_paths(void)
{
    for (uint32_t i = 1; i < s_gif_count; i++) {
        char key[CAROUSEL_PATH_MAX];
        memcpy(key, s_gif_paths[i], CAROUSEL_PATH_MAX);

        uint32_t j = i;
        while (j > 0 && compare_names(s_gif_paths[j - 1], key) > 0) {
            memcpy(s_gif_paths[j], s_gif_paths[j - 1], CAROUSEL_PATH_MAX);
            j--;
        }
        memcpy(s_gif_paths[j], key, CAROUSEL_PATH_MAX);
    }
}

static void collect_gifs_from(const char *dir)
{
    DIR *handle = opendir(dir);
    if (handle == NULL) {
        ESP_LOGW(TAG, "打不开目录 %s", dir);
        return;
    }

    struct dirent *entry;
    while ((entry = readdir(handle)) != NULL) {
        if (!has_gif_suffix(entry->d_name)) {
            continue;
        }
        if (s_gif_count >= CAROUSEL_MAX_GIFS) {
            ESP_LOGW(TAG, "GIF 数量超过上限 %d，%s 及之后的都被忽略",
                     CAROUSEL_MAX_GIFS, entry->d_name);
            break;
        }

        const int written = snprintf(s_gif_paths[s_gif_count], CAROUSEL_PATH_MAX,
                                     "%s/%s", dir, entry->d_name);
        if (written <= 0 || written >= CAROUSEL_PATH_MAX) {
            ESP_LOGW(TAG, "路径过长，跳过 %s/%s", dir, entry->d_name);
            continue;
        }
        s_gif_count++;
    }
    closedir(handle);
}

static void collect_gifs(void)
{
    s_gif_count = 0;
    collect_gifs_from(CAROUSEL_DIR_PRIMARY);
    if (s_gif_count == 0) {
        /* 主目录没素材就退回卡根目录，方便直接把文件丢根目录下调试。 */
        collect_gifs_from(CAROUSEL_DIR_FALLBACK);
    }
    sort_gif_paths();
}

/*
 * 在 LVGL 任务之外把一个 GIF 顺序读入 PSRAM。
 * 这里故意不使用 fseek：预读阶段只做小块 fread，LVGL 侧完全不碰 FAT。
 */
static uint8_t *read_gif_to_psram(const char *path, size_t *size_out)
{
    struct stat file_stat;
    if (stat(path, &file_stat) != 0 || file_stat.st_size <= 0) {
        ESP_LOGW(TAG, "无法获取 GIF 大小 %s：%s", path, strerror(errno));
        return NULL;
    }

    const size_t file_size = (size_t)file_stat.st_size;
    uint8_t *data = heap_caps_malloc(file_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (data == NULL) {
        ESP_LOGW(TAG, "PSRAM 不足，无法预读 %s (%u bytes)",
                 path, (unsigned)file_size);
        return NULL;
    }

    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        ESP_LOGW(TAG, "无法打开 GIF %s：%s", path, strerror(errno));
        heap_caps_free(data);
        return NULL;
    }

    size_t offset = 0;
    while (offset < file_size) {
        const size_t remaining = file_size - offset;
        const size_t chunk = (remaining < CAROUSEL_PREFETCH_CHUNK)
                           ? remaining : CAROUSEL_PREFETCH_CHUNK;
        const size_t read_count = fread(data + offset, 1, chunk, file);
        if (read_count == 0) {
            ESP_LOGW(TAG, "读取 GIF 失败 %s：%s", path,
                     ferror(file) ? strerror(errno) : "提前到达文件末尾");
            fclose(file);
            heap_caps_free(data);
            return NULL;
        }
        offset += read_count;
    }

    fclose(file);
    *size_out = offset;
    return data;
}

static void carousel_prefetch_task(void *arg)
{
    (void)arg;

    uint32_t next_index = 0;
    for (;;) {
        gif_prefetch_item_t item = {
            .index = next_index,
            .data = NULL,
            .size = 0,
            .ready = false,
        };

        item.data = read_gif_to_psram(s_gif_paths[next_index], &item.size);
        item.ready = (item.data != NULL);
        if (item.ready) {
            ESP_LOGI(TAG, "GIF 预读完成 [%u/%u]：%u bytes",
                     (unsigned)(next_index + 1), (unsigned)s_gif_count,
                     (unsigned)item.size);
        }

        /* 队列满时只阻塞预读任务，绝不阻塞 taskLVGL。 */
        if (xQueueSend(s_prefetch_queue, &item, portMAX_DELAY) != pdTRUE) {
            if (item.data != NULL) {
                heap_caps_free(item.data);
            }
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        next_index = (next_index + 1U) % s_gif_count;
        vTaskDelay(1);
    }
}

static void pause_gif_timer(void)
{
    if (s_gif_obj == NULL) {
        return;
    }

    lv_gif_t *gifobj = (lv_gif_t *)s_gif_obj;
    if (gifobj->timer != NULL) {
        lv_timer_pause(gifobj->timer);
    }
}

/* 从预读队列取指定序号的 GIF，并在 LVGL 任务中只切换内存源。 */
static gif_show_result_t show_prefetched_gif(uint32_t index)
{
    /* 无论预读是否已经完成，都先停止旧 GIF，避免切回首页或等待时
     * 旧的 LVGL 定时器继续消耗 CPU。成功加载内存源后 lv_gif_set_src()
     * 会重新启动这个定时器。 */
    pause_gif_timer();

    gif_prefetch_item_t item;
    if (s_prefetch_queue == NULL ||
        xQueueReceive(s_prefetch_queue, &item, 0) != pdTRUE) {
        return GIF_SHOW_NOT_READY;
    }

    if (item.index != index) {
        ESP_LOGW(TAG, "GIF 预读序号错位：期望 %u，收到 %u",
                 (unsigned)index, (unsigned)item.index);
        if (item.data != NULL) {
            heap_caps_free(item.data);
        }
        return GIF_SHOW_NOT_READY;
    }

    if (!item.ready || item.data == NULL || item.size == 0) {
        ESP_LOGW(TAG, "GIF %s 预读失败，本轮跳过", s_gif_paths[index]);
        return GIF_SHOW_FAILED;
    }

    lv_gif_t *gifobj = (lv_gif_t *)s_gif_obj;

    /* lv_gif_set_src() 打开失败时直接 return，既不清空对象也不停定时器。
     * 这里已经先暂停旧定时器；成功时 lv_gif_set_src() 自己会 resume。 */
    lv_timer_pause(gifobj->timer);

    /* lv_gif_set_src() 对 LV_IMG_SRC_VARIABLE 会调用 gd_open_gif_data()，
     * 后续 GIF 帧解析只访问这块内存，不会再调用 lv_fs。 */
    lv_img_dsc_t gif_source = {0};
    gif_source.header.cf = LV_IMG_CF_RAW;
    gif_source.data_size = item.size;
    gif_source.data = item.data;
    lv_gif_set_src(s_gif_obj, &gif_source);
    if (gifobj->gif == NULL) {
        ESP_LOGW(TAG, "无法解析预读 GIF %s，本轮跳过", s_gif_paths[index]);
        if (s_active_gif_data != NULL) {
            heap_caps_free(s_active_gif_data);
            s_active_gif_data = NULL;
        }
        heap_caps_free(item.data);
        return GIF_SHOW_FAILED;
    }

    if (s_active_gif_data != NULL) {
        heap_caps_free(s_active_gif_data);
    }
    s_active_gif_data = item.data;

    const uint32_t gif_w = gifobj->gif->width;
    const uint32_t gif_h = gifobj->gif->height;
    const lv_coord_t scr_w = lv_disp_get_hor_res(NULL);
    const lv_coord_t scr_h = lv_disp_get_ver_res(NULL);

    /* 尺寸和 zoom 必须逐次重设：对象是复用的，上一个素材留下的
     * zoom/尺寸不会自动失效（lv_gif_set_src 内部的 refresh_self_size
     * 在尺寸已被显式设定后会直接返回）。 */
    if (gif_w > (uint32_t)scr_w || gif_h > (uint32_t)scr_h) {
        /* 素材比屏幕大时等比缩到放得下。lv_gif 自己完全不做缩放，
         * 不处理的话对象会按原始尺寸居中，屏幕只能看到中间一块。
         * 注意这只是显示兜底：解码仍按原始像素走，帧率会明显偏低。
         * 正解是用 tools/gif_resize_for_sd.py 在 PC 上先缩好再放卡上。 */
        const uint32_t zoom_x = (uint32_t)scr_w * 256U / gif_w;
        const uint32_t zoom_y = (uint32_t)scr_h * 256U / gif_h;
        uint32_t zoom = (zoom_x < zoom_y) ? zoom_x : zoom_y;
        if (zoom == 0) {
            zoom = 1;
        }

        ESP_LOGW(TAG, "%s 是 %ux%u，超出屏幕 %dx%d：按 %u%% 缩小显示。"
                      "请用 tools/gif_resize_for_sd.py 预处理素材，否则会掉帧",
                 s_gif_paths[index], gif_w, gif_h, scr_w, scr_h,
                 (unsigned)(zoom * 100U / 256U));

        lv_img_set_zoom(s_gif_obj, (uint16_t)zoom);
        lv_obj_set_size(s_gif_obj, (lv_coord_t)(gif_w * zoom / 256U),
                                     (lv_coord_t)(gif_h * zoom / 256U));
    } else {
        lv_img_set_zoom(s_gif_obj, LV_IMG_ZOOM_NONE);
        lv_obj_set_size(s_gif_obj, (lv_coord_t)gif_w, (lv_coord_t)gif_h);
    }
    lv_obj_center(s_gif_obj);

    return GIF_SHOW_OK;
}

static void carousel_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    if (s_slot == 0) {
        /* 隐藏 GIF 时也要暂停它的 LVGL 定时器，避免后台继续解码。 */
        pause_gif_timer();
        lv_scr_load(s_home_screen);
        s_slot = (s_gif_count > 0) ? 1 : 0;
        return;
    }

    const gif_show_result_t result = show_prefetched_gif(s_slot - 1);
    if (result == GIF_SHOW_OK) {
        lv_scr_load(s_gif_screen);
        s_slot = (s_slot >= s_gif_count) ? 0 : s_slot + 1;
    } else if (result == GIF_SHOW_FAILED) {
        /* 文件确实读失败时跳过它，避免每 5 秒反复重试同一个坏文件。 */
        lv_scr_load(s_home_screen);
        s_slot = (s_slot >= s_gif_count) ? 0 : s_slot + 1;
    } else {
        /* 预读还没完成时不阻塞 LVGL，保留当前序号，下次定时器再试。 */
        lv_scr_load(s_home_screen);
    }
}

/* 建 GIF 屏。天气首页由 display_driver 建在默认屏幕上，这里先把它的句柄
 * 存下来，等会儿轮播要切回去。lv_obj_create(NULL) 只是创建屏幕，
 * 不会改变当前活动屏幕，所以此刻 lv_scr_act() 拿到的仍是天气首页。 */
static void build_gif_screen(void)
{
    s_home_screen = lv_scr_act();

    s_gif_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_gif_screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_gif_screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_gif_screen, LV_OBJ_FLAG_SCROLLABLE);

    s_gif_obj = lv_gif_create(s_gif_screen);
    lv_obj_center(s_gif_obj);
}

static void carousel_task(void *arg)
{
    (void)arg;

    if (sd_card_mount() != ESP_OK) {
        ESP_LOGE(TAG, "TF 卡挂载失败，屏幕停在天气首页");
        vTaskDelete(NULL);
        return;
    }

    collect_gifs();
    if (s_gif_count == 0) {
        ESP_LOGE(TAG, "%s 和 %s 下都没有 .gif 文件，屏幕停在天气首页",
                 CAROUSEL_DIR_PRIMARY, CAROUSEL_DIR_FALLBACK);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "找到 %u 个 GIF：天气首页 %d ms → 每个 GIF %d ms → 回到首页，循环",
             (unsigned)s_gif_count, CAROUSEL_SLOT_MS, CAROUSEL_SLOT_MS);

    s_prefetch_queue = xQueueCreate(CAROUSEL_PREFETCH_DEPTH,
                                    sizeof(gif_prefetch_item_t));
    if (s_prefetch_queue == NULL) {
        ESP_LOGE(TAG, "GIF 预读队列创建失败，屏幕停在天气首页");
        vTaskDelete(NULL);
        return;
    }

    const BaseType_t prefetch_created = xTaskCreatePinnedToCore(
        carousel_prefetch_task, "gif_prefetch", CAROUSEL_PREFETCH_STACK, NULL,
        CAROUSEL_PREFETCH_PRIORITY, NULL, CAROUSEL_PREFETCH_CORE);
    if (prefetch_created != pdPASS) {
        ESP_LOGE(TAG, "GIF 预读任务创建失败，屏幕停在天气首页");
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "GIF 预读任务已启动：CPU%d，队列深度=%d，读取块=%u bytes",
             CAROUSEL_PREFETCH_CORE, CAROUSEL_PREFETCH_DEPTH,
             CAROUSEL_PREFETCH_CHUNK);

    lvgl_port_lock(0);
    build_gif_screen();
    lv_timer_create(carousel_timer_cb, CAROUSEL_SLOT_MS, NULL);
    lvgl_port_unlock();

    /* 轮播由 LVGL 定时器驱动，本任务到此完成。 */
    vTaskDelete(NULL);
}

void screen_carousel_start(void)
{
    const BaseType_t created = xTaskCreate(carousel_task, "screen_carousel",
                                           CAROUSEL_TASK_STACK, NULL,
                                           CAROUSEL_TASK_PRIORITY, NULL);
    if (created != pdPASS) {
        ESP_LOGE(TAG, "轮播任务创建失败，屏幕停在天气首页");
    }
}
