#include "ambient_led.h"

#include <stddef.h>
#include <string.h>
#include <strings.h>

#include "board_pins.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip.h"

static const char *TAG = "AMBIENT_LED";

/* 呼吸曲线查表：半余弦叠加 gamma，保证暗部变化平滑。 */
#define AMBIENT_LED_BREATH_LUT_STEPS 64
static const uint8_t s_breath_lut[AMBIENT_LED_BREATH_LUT_STEPS + 1] = {
      0,   0,   0,   0,   0,   1,   1,   2,
      4,   6,   9,  14,  19,  26,  34,  44,
     55,  68,  82,  97, 113, 130, 147, 164,
    180, 196, 210, 223, 234, 243, 250, 254,
    255, 254, 250, 243, 234, 223, 210, 196,
    180, 164, 147, 130, 113,  97,  82,  68,
     55,  44,  34,  26,  19,  14,   9,   6,
      4,   2,   1,   1,   0,   0,   0,   0,
      0,
};

typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
    uint8_t brightness;
    ambient_led_effect_t effect;
    uint32_t period_ms;
    bool active;
} led_layer_t;

typedef struct {
    gpio_num_t gpio;
    uint32_t led_count;
    led_strip_handle_t strip;
    led_layer_t manual;
} ambient_led_channel_t;

typedef struct {
    uint32_t led_count;
    led_strip_handle_t strip;
    led_layer_t layer;
} render_snapshot_t;

static ambient_led_channel_t s_channels[AMBIENT_LED_COUNT] = {
    { .gpio = BOARD_WS2812_A_GPIO, .led_count = BOARD_WS2812_A_LED_COUNT },
    { .gpio = BOARD_WS2812_B_GPIO, .led_count = BOARD_WS2812_B_LED_COUNT },
};

/* 所有 API 只更新这组状态，RMT 刷新统一在 ambient_led_task 中完成。 */
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t s_task;
static uint8_t s_brightness = 255;
static uint32_t s_tick;
static uint32_t s_breath_period_ms = AMBIENT_LED_BREATH_PERIOD_MS;
static ambient_led_state_t s_state = AMBIENT_LED_STATE_WAKE_IDLE;
static bool s_emotion_active;
static char s_emotion[20];
static bool s_manual_off;
static int64_t s_off_deadline_us;
static int64_t s_manual_deadline_us;
static uint8_t s_manual_deadline_mask;
static bool s_initialized;
static bool s_refresh_error_logged;

static uint8_t scale_u8(uint8_t value, uint8_t scale)
{
    return (uint8_t)(((uint32_t)value * (uint32_t)scale + 127u) / 255u);
}

static uint8_t breath_level(uint8_t phase)
{
    const uint32_t steps = AMBIENT_LED_BREATH_LUT_STEPS;
    const uint32_t scaled = (uint32_t)phase * steps;
    const uint32_t index = scaled >> 8;
    if (index >= steps) {
        return s_breath_lut[steps];
    }
    const uint32_t fraction = scaled & 0xFFu;
    const int32_t base = (int32_t)s_breath_lut[index];
    const int32_t delta = (int32_t)s_breath_lut[index + 1] - base;
    return (uint8_t)(base + ((delta * (int32_t)fraction) >> 8));
}

static uint8_t layer_level(const led_layer_t *layer, uint32_t index,
                           uint32_t led_count, uint32_t tick)
{
    uint32_t period = layer->period_ms != 0 ? layer->period_ms : AMBIENT_LED_BREATH_PERIOD_MS;
    switch (layer->effect) {
    case AMBIENT_LED_EFFECT_BREATH: {
        const uint32_t ticks = period / AMBIENT_LED_TICK_MS;
        const uint8_t phase = (uint8_t)((tick * 256u) / (ticks != 0 ? ticks : 1u));
        return breath_level(phase);
    }
    case AMBIENT_LED_EFFECT_BLINK: {
        const uint32_t ticks = period / AMBIENT_LED_TICK_MS;
        const uint32_t half = (ticks != 0 ? ticks : 1u) / 2u;
        return (((tick / (half != 0 ? half : 1u)) & 1u) == 0u) ? 255u : 0u;
    }
    case AMBIENT_LED_EFFECT_FLOW: {
        const uint32_t ticks = period / AMBIENT_LED_TICK_MS;
        const uint8_t head = (uint8_t)((tick * 256u) / (ticks != 0 ? ticks : 1u));
        /* 流水效果按灯珠位置均匀错相，再随 tick 移动。 */
        const uint32_t phase_offset = led_count != 0 ? (index * 256u) / led_count : 0;
        return breath_level((uint8_t)(phase_offset - head));
    }
    case AMBIENT_LED_EFFECT_STATIC:
    default:
        return 255u;
    }
}

static led_layer_t make_layer(uint8_t r, uint8_t g, uint8_t b,
                              uint8_t brightness, ambient_led_effect_t effect,
                              uint32_t period_ms)
{
    led_layer_t layer = {
        .r = r, .g = g, .b = b, .brightness = brightness,
        .effect = effect, .period_ms = period_ms, .active = true,
    };
    return layer;
}

static led_layer_t state_layer(ambient_led_state_t state)
{
    switch (state) {
    case AMBIENT_LED_STATE_LISTENING:
        return make_layer(0, 80, 255, 180, AMBIENT_LED_EFFECT_BREATH, 3000);
    case AMBIENT_LED_STATE_THINKING:
        return make_layer(180, 0, 255, 160, AMBIENT_LED_EFFECT_BREATH, 3000);
    case AMBIENT_LED_STATE_SPEAKING:
        return make_layer(0, 220, 200, 135, AMBIENT_LED_EFFECT_BREATH, 3000);
    case AMBIENT_LED_STATE_ERROR:
        return make_layer(255, 0, 0, 220, AMBIENT_LED_EFFECT_BLINK, 600);
    case AMBIENT_LED_STATE_NETWORK_CONNECTING:
        return make_layer(255, 180, 0, 160, AMBIENT_LED_EFFECT_BREATH, 3000);
    case AMBIENT_LED_STATE_WAKE_IDLE:
    default:
        /* 暖白低亮：RGB 混色模拟待机暖光。 */
        return make_layer(255, 170, 80, 56, AMBIENT_LED_EFFECT_STATIC, 0);
    }
}

static led_layer_t emotion_layer(const char *emotion)
{
    if (emotion != NULL && strcasecmp(emotion, "happy") == 0) {
        return make_layer(255, 170, 20, 180, AMBIENT_LED_EFFECT_STATIC, 0);
    }
    if (emotion != NULL && strcasecmp(emotion, "sad") == 0) {
        return make_layer(30, 80, 255, 140, AMBIENT_LED_EFFECT_BREATH, 3200);
    }
    if (emotion != NULL && strcasecmp(emotion, "angry") == 0) {
        return make_layer(255, 20, 0, 220, AMBIENT_LED_EFFECT_BLINK, 500);
    }
    if (emotion != NULL && strcasecmp(emotion, "surprised") == 0) {
        return make_layer(180, 0, 255, 180, AMBIENT_LED_EFFECT_BREATH, 1800);
    }
    /* neutral 以及未知情绪统一回到暖白。 */
    return make_layer(255, 170, 80, 80, AMBIENT_LED_EFFECT_STATIC, 0);
}

static void render_channel(const render_snapshot_t *snapshot, uint32_t tick)
{
    const led_layer_t *layer = &snapshot->layer;
    for (uint32_t i = 0; i < snapshot->led_count; ++i) {
        const uint8_t level = scale_u8(layer_level(layer, i, snapshot->led_count, tick),
                                       layer->brightness);
        led_strip_set_pixel(snapshot->strip, i,
                            scale_u8(layer->r, level),
                            scale_u8(layer->g, level),
                            scale_u8(layer->b, level));
    }
    const esp_err_t err = led_strip_refresh(snapshot->strip);
    if (err != ESP_OK) {
        if (!s_refresh_error_logged) {
            s_refresh_error_logged = true;
            ESP_LOGW(TAG, "刷新灯带失败：%s", esp_err_to_name(err));
        }
    } else {
        s_refresh_error_logged = false;
    }
}

static uint8_t target_mask(ambient_led_target_t target)
{
    return (uint8_t)target;
}

static bool target_valid(ambient_led_target_t target)
{
    const uint8_t mask = target_mask(target);
    return mask != 0 && (mask & (uint8_t)~AMBIENT_LED_TARGET_BOTH) == 0;
}

static void resolve_snapshot_locked(render_snapshot_t *snapshot, size_t index)
{
    snapshot->led_count = s_channels[index].led_count;
    snapshot->strip = s_channels[index].strip;
    if (s_manual_off) {
        snapshot->layer = make_layer(0, 0, 0, 0, AMBIENT_LED_EFFECT_STATIC, 0);
    } else if (s_channels[index].manual.active) {
        snapshot->layer = s_channels[index].manual;
    } else if (s_emotion_active) {
        snapshot->layer = emotion_layer(s_emotion);
    } else {
        snapshot->layer = state_layer(s_state);
    }
}

static void refresh_frame(void)
{
    render_snapshot_t snapshot[AMBIENT_LED_COUNT];
    const int64_t now = esp_timer_get_time();
    uint32_t tick;

    portENTER_CRITICAL(&s_lock);
    if (s_manual_deadline_us != 0 && now >= s_manual_deadline_us) {
        for (size_t i = 0; i < AMBIENT_LED_COUNT; ++i) {
            if ((s_manual_deadline_mask & (1u << i)) != 0u) {
                s_channels[i].manual.active = false;
            }
        }
        s_manual_deadline_us = 0;
        s_manual_deadline_mask = 0;
        ESP_LOGI(TAG, "临时灯效到期，恢复当前情绪/系统状态灯效");
    }
    if (s_off_deadline_us != 0 && now >= s_off_deadline_us) {
        s_manual_off = true;
        s_off_deadline_us = 0;
        ESP_LOGI(TAG, "定时关灯到期");
    }
    for (size_t i = 0; i < AMBIENT_LED_COUNT; ++i) {
        resolve_snapshot_locked(&snapshot[i], i);
    }
    tick = s_tick++;
    portEXIT_CRITICAL(&s_lock);

    for (size_t i = 0; i < AMBIENT_LED_COUNT; ++i) {
        render_channel(&snapshot[i], tick);
    }
}

static void ambient_led_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "灯效任务启动（CPU%d，周期 %d ms，优先级 %d）",
             xPortGetCoreID(), AMBIENT_LED_TICK_MS, AMBIENT_LED_TASK_PRIORITY);
    TickType_t last_wake = xTaskGetTickCount();
    while (true) {
        refresh_frame();
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(AMBIENT_LED_TICK_MS));
    }
}

esp_err_t ambient_led_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }
    for (size_t i = 0; i < AMBIENT_LED_COUNT; ++i) {
        ambient_led_channel_t *ch = &s_channels[i];
        if (ch->led_count == 0) {
            ESP_LOGE(TAG, "%c 路灯珠数量为 0，请检查 board_pins.h", 'A' + (int)i);
            return ESP_ERR_INVALID_STATE;
        }
        const led_strip_config_t strip_config = {
            .strip_gpio_num = ch->gpio,
            .max_leds = ch->led_count,
            .led_model = LED_MODEL_WS2812,
            .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
            .flags = { .invert_out = false },
        };
        const led_strip_rmt_config_t rmt_config = {
            .clk_src = RMT_CLK_SRC_DEFAULT,
            .resolution_hz = 10 * 1000 * 1000,
            .mem_block_symbols = 64,
            .flags = { .with_dma = false },
        };
        const esp_err_t err = led_strip_new_rmt_device(&strip_config, &rmt_config,
                                                        &ch->strip);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "%c 路灯带创建失败（GPIO%d）：%s", 'A' + (int)i,
                     ch->gpio, esp_err_to_name(err));
            for (size_t j = 0; j < i; ++j) {
                if (s_channels[j].strip != NULL) {
                    led_strip_del(s_channels[j].strip);
                    s_channels[j].strip = NULL;
                }
            }
            return err;
        }
        led_strip_clear(ch->strip);
        ESP_LOGI(TAG, "%c initialized GPIO%d（%d 颗 WS2812，RMT 后端，无 DMA）",
                 'A' + (int)i, ch->gpio, (int)ch->led_count);
    }
    s_brightness = 255;
    s_tick = 0;
    s_state = AMBIENT_LED_STATE_WAKE_IDLE;
    s_emotion_active = false;
    s_manual_off = false;
    s_off_deadline_us = 0;
    s_manual_deadline_us = 0;
    s_manual_deadline_mask = 0;
    s_refresh_error_logged = false;

    const BaseType_t created = xTaskCreate(ambient_led_task, "ambient_led",
                                           AMBIENT_LED_TASK_STACK, NULL,
                                           AMBIENT_LED_TASK_PRIORITY, &s_task);
    if (created != pdPASS) {
        ESP_LOGE(TAG, "创建灯效任务失败");
        for (size_t i = 0; i < AMBIENT_LED_COUNT; ++i) {
            if (s_channels[i].strip != NULL) {
                led_strip_del(s_channels[i].strip);
                s_channels[i].strip = NULL;
            }
        }
        return ESP_ERR_NO_MEM;
    }
    s_initialized = true;
    ESP_LOGI(TAG, "初始化完成：默认暖白待机，呼吸 %d ms，RMT 非 DMA",
             AMBIENT_LED_BREATH_PERIOD_MS);
    return ESP_OK;
}

esp_err_t ambient_led_set_rgb(ambient_led_target_t target,
                              uint8_t r, uint8_t g, uint8_t b,
                              uint8_t brightness)
{
    if (!s_initialized || !target_valid(target)) {
        return !s_initialized ? ESP_ERR_INVALID_STATE : ESP_ERR_INVALID_ARG;
    }
    const uint8_t mask = target_mask(target);
    portENTER_CRITICAL(&s_lock);
    s_manual_off = false;
    s_off_deadline_us = 0;
    for (size_t i = 0; i < AMBIENT_LED_COUNT; ++i) {
        if ((mask & (1u << i)) != 0u) {
            s_channels[i].manual = make_layer(r, g, b, brightness,
                                               AMBIENT_LED_EFFECT_STATIC, 0);
        }
    }
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

esp_err_t ambient_led_set_rgb_a(uint8_t r, uint8_t g, uint8_t b)
{
    return ambient_led_set_rgb(AMBIENT_LED_TARGET_A, r, g, b, s_brightness);
}

esp_err_t ambient_led_set_rgb_b(uint8_t r, uint8_t g, uint8_t b)
{
    return ambient_led_set_rgb(AMBIENT_LED_TARGET_B, r, g, b, s_brightness);
}

esp_err_t ambient_led_set_temperature(ambient_led_target_t target,
                                      ambient_led_temperature_t temperature_level,
                                      uint8_t brightness)
{
    uint8_t r = 255, g = 170, b = 80;
    if (temperature_level == AMBIENT_LED_TEMPERATURE_NEUTRAL) {
        r = 255; g = 225; b = 180;
    } else if (temperature_level == AMBIENT_LED_TEMPERATURE_COOL) {
        r = 180; g = 220; b = 255;
    } else if (temperature_level != AMBIENT_LED_TEMPERATURE_WARM) {
        return ESP_ERR_INVALID_ARG;
    }
    return ambient_led_set_rgb(target, r, g, b, brightness);
}

void ambient_led_set_brightness(uint8_t brightness)
{
    if (!s_initialized) {
        return;
    }
    portENTER_CRITICAL(&s_lock);
    s_brightness = brightness;
    for (size_t i = 0; i < AMBIENT_LED_COUNT; ++i) {
        if (s_channels[i].manual.active) {
            s_channels[i].manual.brightness = brightness;
        }
    }
    portEXIT_CRITICAL(&s_lock);
}

esp_err_t ambient_led_pulse(ambient_led_target_t target,
                            uint8_t r, uint8_t g, uint8_t b,
                            uint8_t brightness, uint32_t duration_ms)
{
    if (!s_initialized || !target_valid(target)) {
        return !s_initialized ? ESP_ERR_INVALID_STATE : ESP_ERR_INVALID_ARG;
    }
    const uint8_t mask = target_mask(target);
    portENTER_CRITICAL(&s_lock);
    s_manual_off = false;
    s_off_deadline_us = 0;
    for (size_t i = 0; i < AMBIENT_LED_COUNT; ++i) {
        if ((mask & (1u << i)) != 0u) {
            s_channels[i].manual = make_layer(r, g, b, brightness,
                                               AMBIENT_LED_EFFECT_BREATH,
                                               s_breath_period_ms);
        }
    }
    s_manual_deadline_mask = duration_ms != 0 ? mask : 0;
    s_manual_deadline_us = duration_ms != 0
                               ? esp_timer_get_time() + (int64_t)duration_ms * 1000
                               : 0;
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

esp_err_t ambient_led_set_effect(ambient_led_id_t id, ambient_led_effect_t effect)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    if (id >= AMBIENT_LED_COUNT || effect > AMBIENT_LED_EFFECT_FLOW) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&s_lock);
    s_manual_off = false;
    if (!s_channels[id].manual.active) {
        s_channels[id].manual = make_layer(255, 255, 255, s_brightness,
                                           effect, s_breath_period_ms);
    } else {
        s_channels[id].manual.effect = effect;
        s_channels[id].manual.period_ms = s_breath_period_ms;
    }
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

esp_err_t ambient_led_set_breath_period(uint32_t period_ms)
{
    if (period_ms < 100 || period_ms > 60000) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&s_lock);
    s_breath_period_ms = period_ms;
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

esp_err_t ambient_led_set_state(ambient_led_state_t state)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    if (state > AMBIENT_LED_STATE_NETWORK_CONNECTING) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&s_lock);
    s_state = state;
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

esp_err_t ambient_led_set_emotion(const char *emotion)
{
    if (!s_initialized || emotion == NULL || emotion[0] == '\0') {
        return emotion == NULL || emotion[0] == '\0' ? ESP_ERR_INVALID_ARG
                                                     : ESP_ERR_INVALID_STATE;
    }
    portENTER_CRITICAL(&s_lock);
    strncpy(s_emotion, emotion, sizeof(s_emotion) - 1);
    s_emotion[sizeof(s_emotion) - 1] = '\0';
    s_emotion_active = true;
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

void ambient_led_clear_emotion(void)
{
    if (!s_initialized) return;
    portENTER_CRITICAL(&s_lock);
    s_emotion_active = false;
    s_emotion[0] = '\0';
    portEXIT_CRITICAL(&s_lock);
}

esp_err_t ambient_led_on(void)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    portENTER_CRITICAL(&s_lock);
    s_manual_off = false;
    s_off_deadline_us = 0;
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

esp_err_t ambient_led_off(void)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    portENTER_CRITICAL(&s_lock);
    s_manual_off = true;
    s_off_deadline_us = 0;
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

esp_err_t ambient_led_off_after(uint32_t duration_ms)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    if (duration_ms == 0) return ambient_led_off();
    portENTER_CRITICAL(&s_lock);
    s_manual_off = false;
    s_off_deadline_us = esp_timer_get_time() + (int64_t)duration_ms * 1000;
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

void ambient_led_all_off(void)
{
    (void)ambient_led_off();
}

uint32_t ambient_led_count(ambient_led_id_t id)
{
    return id < AMBIENT_LED_COUNT ? s_channels[id].led_count : 0;
}
