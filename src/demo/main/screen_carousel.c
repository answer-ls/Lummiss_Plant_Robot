#include "screen_carousel.h"

#include <dirent.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "anim_bin_player.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

#include "sd_card.h"

static const char *TAG = "CAROUSEL";

#define CAROUSEL_TASK_STACK       6144
#define CAROUSEL_TASK_PRIORITY    5
#define CAROUSEL_SLOT_MS          5000
#define CAROUSEL_TIMER_PERIOD_MS  50
#define CAROUSEL_DIR_PRIMARY      SD_CARD_MOUNT_POINT "/expressions"
#define CAROUSEL_DIR_FALLBACK     SD_CARD_MOUNT_POINT
#define CAROUSEL_MAX_ANIMATIONS   64
#define CAROUSEL_PATH_MAX         128

static char s_animation_paths[CAROUSEL_MAX_ANIMATIONS][CAROUSEL_PATH_MAX];
static uint32_t s_animation_count;
static uint32_t s_next_animation;
static bool s_showing_home = true;
static bool s_started;
static int64_t s_switch_deadline_us;

static lv_obj_t *s_home_screen;
static lv_obj_t *s_animation_screen;
static lv_timer_t *s_carousel_timer;

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

static bool has_bin_suffix(const char *name)
{
    const size_t len = strlen(name);
    return len >= 4 && compare_names(name + len - 4, ".bin") == 0;
}

static void sort_animation_paths(void)
{
    for (uint32_t i = 1; i < s_animation_count; i++) {
        char key[CAROUSEL_PATH_MAX];
        memcpy(key, s_animation_paths[i], sizeof(key));
        uint32_t j = i;
        while (j > 0 && compare_names(s_animation_paths[j - 1], key) > 0) {
            memcpy(s_animation_paths[j], s_animation_paths[j - 1],
                   sizeof(s_animation_paths[j]));
            j--;
        }
        memcpy(s_animation_paths[j], key, sizeof(key));
    }
}

static void collect_animations_from(const char *directory)
{
    DIR *handle = opendir(directory);
    if (handle == NULL) {
        ESP_LOGW(TAG, "打不开目录 %s", directory);
        return;
    }

    struct dirent *entry;
    while ((entry = readdir(handle)) != NULL) {
        if (!has_bin_suffix(entry->d_name)) {
            continue;
        }
        if (s_animation_count >= CAROUSEL_MAX_ANIMATIONS) {
            ESP_LOGW(TAG, "BIN 数量超过上限 %d，后续文件被忽略",
                     CAROUSEL_MAX_ANIMATIONS);
            break;
        }
        const int written = snprintf(s_animation_paths[s_animation_count],
                                     CAROUSEL_PATH_MAX, "%s/%s",
                                     directory, entry->d_name);
        if (written <= 0 || written >= CAROUSEL_PATH_MAX) {
            ESP_LOGW(TAG, "路径过长，跳过 %s/%s", directory, entry->d_name);
            continue;
        }
        s_animation_count++;
    }
    closedir(handle);
}

static void collect_animations(void)
{
    s_animation_count = 0;
    collect_animations_from(CAROUSEL_DIR_PRIMARY);
    if (s_animation_count == 0) {
        collect_animations_from(CAROUSEL_DIR_FALLBACK);
    }
    sort_animation_paths();
}

static void load_home_after_error(esp_err_t error)
{
    ESP_LOGE(TAG, "BIN 播放失败，返回首页：%s", esp_err_to_name(error));
    anim_bin_player_stop();
    lv_scr_load(s_home_screen);
    s_showing_home = true;
    s_next_animation = (s_next_animation + 1U) % s_animation_count;
    s_switch_deadline_us = esp_timer_get_time() +
                           (int64_t)CAROUSEL_SLOT_MS * 1000LL;
}

static bool start_animation(uint32_t index)
{
    const esp_err_t error = anim_bin_player_play(s_animation_paths[index]);
    if (error != ESP_OK) {
        load_home_after_error(error);
        return false;
    }
    lv_scr_load(s_animation_screen);
    s_showing_home = false;
    ESP_LOGI(TAG, "切换到表情 [%u/%u]：%s",
             (unsigned)(index + 1U), (unsigned)s_animation_count,
             s_animation_paths[index]);
    return true;
}

static void carousel_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    if (!s_showing_home && !anim_bin_player_is_loading() &&
        !anim_bin_player_is_playing()) {
        const esp_err_t error = anim_bin_player_get_last_error();
        if (error != ESP_OK) {
            load_home_after_error(error);
            return;
        }
    }

    const int64_t now_us = esp_timer_get_time();
    if (now_us < s_switch_deadline_us) {
        return;
    }

    if (s_showing_home) {
        start_animation(s_next_animation);
    } else {
        anim_bin_player_stop();
        s_next_animation++;
        if (s_next_animation >= s_animation_count) {
            s_next_animation = 0;
            lv_scr_load(s_home_screen);
            s_showing_home = true;
            ESP_LOGI(TAG, "一轮表情完成，返回天气首页");
        } else {
            start_animation(s_next_animation);
        }
    }
    s_switch_deadline_us = now_us + (int64_t)CAROUSEL_SLOT_MS * 1000LL;
}

static esp_err_t build_animation_screen(void)
{
    s_home_screen = lv_scr_act();
    s_animation_screen = lv_obj_create(NULL);
    if (s_animation_screen == NULL) {
        return ESP_ERR_NO_MEM;
    }
    lv_obj_set_style_bg_color(s_animation_screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_animation_screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_animation_screen, LV_OBJ_FLAG_SCROLLABLE);
    return anim_bin_player_init(s_animation_screen);
}

static void carousel_task(void *arg)
{
    (void)arg;

    if (sd_card_mount() != ESP_OK) {
        ESP_LOGE(TAG, "TF 卡挂载失败，屏幕停在天气首页");
        vTaskDelete(NULL);
        return;
    }

    collect_animations();
    if (s_animation_count == 0) {
        ESP_LOGE(TAG, "%s 和 %s 下都没有 .bin 文件，屏幕停在天气首页",
                 CAROUSEL_DIR_PRIMARY, CAROUSEL_DIR_FALLBACK);
        vTaskDelete(NULL);
        return;
    }

    lvgl_port_lock(0);
    esp_err_t error = build_animation_screen();
    if (error == ESP_OK) {
        s_switch_deadline_us = esp_timer_get_time() +
                               (int64_t)CAROUSEL_SLOT_MS * 1000LL;
        s_carousel_timer = lv_timer_create(
            carousel_timer_cb, CAROUSEL_TIMER_PERIOD_MS, NULL);
        if (s_carousel_timer == NULL) {
            error = ESP_ERR_NO_MEM;
        }
    }
    lvgl_port_unlock();

    if (error != ESP_OK) {
        ESP_LOGE(TAG, "BIN 播放器初始化失败：%s", esp_err_to_name(error));
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG,
             "找到 %u 个 LUM1 BIN：天气首页 %d ms → 每个表情 %d ms → 回到首页",
             (unsigned)s_animation_count, CAROUSEL_SLOT_MS, CAROUSEL_SLOT_MS);
    vTaskDelete(NULL);
}

void screen_carousel_start(void)
{
    if (s_started) {
        return;
    }
    s_started = true;
    const BaseType_t created = xTaskCreate(
        carousel_task, "screen_carousel", CAROUSEL_TASK_STACK, NULL,
        CAROUSEL_TASK_PRIORITY, NULL);
    if (created != pdPASS) {
        s_started = false;
        ESP_LOGE(TAG, "轮播任务创建失败，屏幕停在天气首页");
    }
}
