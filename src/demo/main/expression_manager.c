#include "expression_manager.h"

#include <stdint.h>
#include <string.h>
#include <strings.h>

#include "anim_bin_player.h"
#include "ambient_led.h"
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

/* ===================== 情绪词 -> 表情素材映射 =====================
 *
 * 表里左侧是服务端下发的情绪词，右侧是 SD 卡上的素材。
 * exp_0N 与画面的对应关系由 src/demo/tools/gif 下的源图逐帧比对确认：
 *
 *   exp_01 乐   | exp_02 哀   | exp_03 喜   | exp_04 怒
 *   exp_05 思考 | exp_06 惊讶 | exp_07 疑惑 | exp_08 眨眼
 *
 * 小智官方词表有二十多个取值，本地只有 8 张素材，所以多个词会落到同一张
 * 素材上（例如 laughing/funny/silly 共用「乐」）。**表里没有的词统一回退
 * neutral**，也就是「眨眼」这张中性脸——这是官方约定，不会出现收到未知
 * 情绪后屏幕不动的空窗。
 *
 * 注意：这张表只是「词→素材」的查表，真正切屏发生在 LVGL 定时器
 * expression_timer 里。WebSocket 回调只负责往队列投词，不碰任何 LVGL 对象。 */
typedef struct {
    const char *emotion;   /* 服务端下发的 emotion 取值（大小写不敏感） */
    const char *exp_name;  /* 日志用符号名，便于和服务端日志逐行对账 */
    const char *file;      /* SD 卡素材绝对路径 */
} emotion_entry_t;

static const emotion_entry_t k_emotions[] = {
    /* ---- 小智官方词表 ---- */
    { "neutral",     "EXP_NEUTRAL",     EXPRESSION_DIR "/exp_08.bin" },
    { "happy",       "EXP_HAPPY",       EXPRESSION_DIR "/exp_03.bin" },
    { "laughing",    "EXP_LAUGHING",    EXPRESSION_DIR "/exp_01.bin" },
    { "funny",       "EXP_FUNNY",       EXPRESSION_DIR "/exp_01.bin" },
    { "silly",       "EXP_SILLY",       EXPRESSION_DIR "/exp_01.bin" },
    { "sad",         "EXP_SAD",         EXPRESSION_DIR "/exp_02.bin" },
    { "crying",      "EXP_CRYING",      EXPRESSION_DIR "/exp_02.bin" },
    { "angry",       "EXP_ANGRY",       EXPRESSION_DIR "/exp_04.bin" },
    { "surprised",   "EXP_SURPRISED",   EXPRESSION_DIR "/exp_06.bin" },
    { "shocked",     "EXP_SHOCKED",     EXPRESSION_DIR "/exp_06.bin" },
    { "confused",    "EXP_CONFUSED",    EXPRESSION_DIR "/exp_07.bin" },
    { "embarrassed", "EXP_EMBARRASSED", EXPRESSION_DIR "/exp_07.bin" },
    { "thinking",    "EXP_THINKING",    EXPRESSION_DIR "/exp_05.bin" },
    { "loving",      "EXP_LOVING",      EXPRESSION_DIR "/exp_03.bin" },
    { "kissy",       "EXP_KISSY",       EXPRESSION_DIR "/exp_03.bin" },
    { "delicious",   "EXP_DELICIOUS",   EXPRESSION_DIR "/exp_03.bin" },
    { "confident",   "EXP_CONFIDENT",   EXPRESSION_DIR "/exp_03.bin" },
    { "winking",     "EXP_WINKING",     EXPRESSION_DIR "/exp_08.bin" },
    { "relaxed",     "EXP_RELAXED",     EXPRESSION_DIR "/exp_08.bin" },
    { "sleepy",      "EXP_SLEEPY",      EXPRESSION_DIR "/exp_08.bin" },
    { "cool",        "EXP_COOL",        EXPRESSION_DIR "/exp_08.bin" },

    /* ---- Lummiss 自有词表：保留兼容，官方模式下不会出现 ---- */
    { "joy",         "EXP_JOY",         EXPRESSION_DIR "/exp_01.bin" },
};

#define EMOTION_COUNT (sizeof(k_emotions) / sizeof(k_emotions[0]))

static const emotion_entry_t *emotion_lookup(const char *emotion)
{
    if (emotion == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < EMOTION_COUNT; i++) {
        /* 大小写不敏感：官方侧是小写，但没必要对服务端的大小写写法做假设。 */
        if (strcasecmp(k_emotions[i].emotion, emotion) == 0) {
            return &k_emotions[i];
        }
    }
    return NULL;
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
            /* 词表外的取值回退 neutral。日志刻意打成
             *   EXPRESSION: emotion happy -> EXP_HAPPY
             * 这种一行式，方便和 xiaozhi_audio 侧的
             *   XIAOZHI: LLM emotion=happy text=😀
             * 对着看，确认「收到什么词」和「切了哪张素材」一致。 */
            const emotion_entry_t *entry = emotion_lookup(event.value);
            if (entry == NULL) {
                ESP_LOGW(TAG, "emotion %s unknown -> neutral", event.value);
                entry = emotion_lookup("neutral");
            } else {
                ESP_LOGI(TAG, "emotion %s -> %s", event.value, entry->exp_name);
            }
            /* 表情和氛围灯共用同一个事件入口，灯效层不会阻塞 LVGL 定时器。 */
            ambient_led_set_emotion(event.value);
            play_file(entry->file, 1800);
        } else if (strcmp(event.value, "listen") == 0) {
            s_emotion_deadline = 0;
            ambient_led_clear_emotion();
            ambient_led_set_state(AMBIENT_LED_STATE_LISTENING);
            play_file(EXPRESSION_DIR "/exp_08.bin", 0);
        } else if (strcmp(event.value, "thinking") == 0) {
            s_emotion_deadline = 0;
            ambient_led_clear_emotion();
            ambient_led_set_state(AMBIENT_LED_STATE_THINKING);
            play_file(EXPRESSION_DIR "/exp_05.bin", 0);
        } else if (strcmp(event.value, "home") == 0 ||
                   strcmp(event.value, "tts_stop") == 0) {
            ambient_led_clear_emotion();
            ambient_led_set_state(AMBIENT_LED_STATE_WAKE_IDLE);
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
