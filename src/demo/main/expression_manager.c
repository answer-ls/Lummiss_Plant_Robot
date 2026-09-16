#include "expression_manager.h"

#include <stdint.h>
#include <string.h>

#include "anim_bin_player.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

static const char *TAG = "EXPRESSION";
#define EXPRESSION_QUEUE_LEN 8
#define EXPRESSION_DIR "/sdcard/expressions"

typedef enum { EVENT_STATE, EVENT_EMOTION } event_type_t;
typedef struct { event_type_t type; char value[20]; } expression_event_t;
static QueueHandle_t s_queue;
static lv_obj_t *s_home;
static lv_obj_t *s_screen;
static lv_timer_t *s_timer;
static int64_t s_emotion_deadline;
static bool s_initialized;

static const char *emotion_file(const char *emotion)
{
    if (strcmp(emotion, "sad") == 0) return EXPRESSION_DIR "/exp_01.bin";
    if (strcmp(emotion, "surprised") == 0) return EXPRESSION_DIR "/exp_02.bin";
    if (strcmp(emotion, "joy") == 0) return EXPRESSION_DIR "/exp_03.bin";
    if (strcmp(emotion, "angry") == 0) return EXPRESSION_DIR "/exp_04.bin";
    if (strcmp(emotion, "happy") == 0) return EXPRESSION_DIR "/exp_06.bin";
    if (strcmp(emotion, "confused") == 0) return EXPRESSION_DIR "/exp_07.bin";
    return EXPRESSION_DIR "/exp_08.bin";
}

static void play_file(const char *path, uint32_t duration_ms)
{
    if (anim_bin_player_play(path) != ESP_OK) {
        ESP_LOGW(TAG, "表情播放请求失败：%s", path);
        lv_scr_load(s_home);
        return;
    }
    lv_scr_load(s_screen);
    s_emotion_deadline = duration_ms ?
        esp_timer_get_time() + (int64_t)duration_ms * 1000 : 0;
}

static void expression_timer(lv_timer_t *timer)
{
    (void)timer;
    expression_event_t event;
    if (xQueueReceive(s_queue, &event, 0) == pdPASS) {
        if (event.type == EVENT_EMOTION) {
            play_file(emotion_file(event.value), 1800);
        } else if (strcmp(event.value, "listen") == 0) {
            s_emotion_deadline = 0;
            play_file(EXPRESSION_DIR "/exp_08.bin", 0);
        } else if (strcmp(event.value, "thinking") == 0) {
            s_emotion_deadline = 0;
            play_file(EXPRESSION_DIR "/exp_05.bin", 0);
        } else if (strcmp(event.value, "home") == 0 ||
                   strcmp(event.value, "tts_stop") == 0) {
            if (s_emotion_deadline == 0 || esp_timer_get_time() >= s_emotion_deadline) {
                anim_bin_player_stop();
                lv_scr_load(s_home);
            }
        }
    }
    if (s_emotion_deadline != 0 && esp_timer_get_time() >= s_emotion_deadline) {
        s_emotion_deadline = 0;
        anim_bin_player_stop();
        lv_scr_load(s_home);
    }
}

esp_err_t expression_manager_init(void)
{
    if (s_initialized) return ESP_OK;
    s_home = lv_scr_act();
    s_screen = lv_obj_create(NULL);
    if (s_screen == NULL) return ESP_ERR_NO_MEM;
    lv_obj_set_style_bg_color(s_screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    esp_err_t err = anim_bin_player_init(s_screen);
    if (err != ESP_OK) { lv_obj_del(s_screen); s_screen = NULL; return err; }
    s_queue = xQueueCreate(EXPRESSION_QUEUE_LEN, sizeof(expression_event_t));
    if (s_queue == NULL) return ESP_ERR_NO_MEM;
    s_timer = lv_timer_create(expression_timer, 50, NULL);
    if (s_timer == NULL) return ESP_ERR_NO_MEM;
    s_initialized = true;
    ESP_LOGI(TAG, "事件驱动表情管理器已就绪，默认保持 HOME");
    return ESP_OK;
}

static void post(event_type_t type, const char *value)
{
    if (!s_initialized || value == NULL) return;
    expression_event_t event = { .type = type };
    strncpy(event.value, value, sizeof(event.value) - 1);
    (void)xQueueSend(s_queue, &event, 0);
}

void expression_manager_post_state(const char *state) { post(EVENT_STATE, state); }
void expression_manager_post_emotion(const char *emotion) { post(EVENT_EMOTION, emotion); }
