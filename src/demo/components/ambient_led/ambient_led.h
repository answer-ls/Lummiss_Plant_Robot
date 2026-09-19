#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 两路 WS2812 氛围灯。底层始终使用 ESP-IDF led_strip + RMT，不使用 bitbang。 */
#define AMBIENT_LED_TICK_MS          20
#define AMBIENT_LED_BREATH_PERIOD_MS 3000
#define AMBIENT_LED_BLINK_PERIOD_MS  600
#define AMBIENT_LED_FLOW_PERIOD_MS   1500
#define AMBIENT_LED_TASK_STACK       4096
#define AMBIENT_LED_TASK_PRIORITY    3

typedef enum {
    AMBIENT_LED_A = 0,
    AMBIENT_LED_B,
    AMBIENT_LED_COUNT
} ambient_led_id_t;

/* 对外控制目标：单路或两路同时控制。 */
typedef enum {
    AMBIENT_LED_TARGET_A = 1u << 0,
    AMBIENT_LED_TARGET_B = 1u << 1,
    AMBIENT_LED_TARGET_BOTH = AMBIENT_LED_TARGET_A | AMBIENT_LED_TARGET_B,
} ambient_led_target_t;

typedef enum {
    AMBIENT_LED_EFFECT_STATIC = 0,
    AMBIENT_LED_EFFECT_BREATH,
    AMBIENT_LED_EFFECT_BLINK,
    AMBIENT_LED_EFFECT_FLOW,
} ambient_led_effect_t;

/* 系统状态灯。状态层低于 AI 情绪层和手动/MCP 临时灯效。 */
typedef enum {
    AMBIENT_LED_STATE_WAKE_IDLE = 0,
    AMBIENT_LED_STATE_LISTENING,
    AMBIENT_LED_STATE_THINKING,
    AMBIENT_LED_STATE_SPEAKING,
    AMBIENT_LED_STATE_ERROR,
    AMBIENT_LED_STATE_NETWORK_CONNECTING,
} ambient_led_state_t;

typedef enum {
    AMBIENT_LED_TEMPERATURE_WARM = 0,
    AMBIENT_LED_TEMPERATURE_NEUTRAL = 1,
    AMBIENT_LED_TEMPERATURE_COOL = 2,
} ambient_led_temperature_t;

esp_err_t ambient_led_init(void);

/* 统一颜色接口。brightness 为 0~255，接口只更新状态，由灯效任务异步刷新。 */
esp_err_t ambient_led_set_rgb(ambient_led_target_t target,
                              uint8_t r, uint8_t g, uint8_t b,
                              uint8_t brightness);

/* 兼容旧调用：使用当前全局亮度设置单路颜色，并切换为手动常亮。 */
esp_err_t ambient_led_set_rgb_a(uint8_t r, uint8_t g, uint8_t b);
esp_err_t ambient_led_set_rgb_b(uint8_t r, uint8_t g, uint8_t b);

/* temperature_level 使用 WARM/NEUTRAL/COOL 三档 RGB 混色。 */
esp_err_t ambient_led_set_temperature(ambient_led_target_t target,
                                      ambient_led_temperature_t temperature_level,
                                      uint8_t brightness);

void ambient_led_set_brightness(uint8_t brightness);

/* 手动/MCP 临时灯效：duration_ms 到期后自动恢复情绪或系统状态灯效。 */
esp_err_t ambient_led_pulse(ambient_led_target_t target,
                            uint8_t r, uint8_t g, uint8_t b,
                            uint8_t brightness, uint32_t duration_ms);
esp_err_t ambient_led_set_effect(ambient_led_id_t id, ambient_led_effect_t effect);
esp_err_t ambient_led_set_breath_period(uint32_t period_ms);

/* 状态和情绪只写入控制状态，不在调用线程刷新 RMT。 */
esp_err_t ambient_led_set_state(ambient_led_state_t state);
esp_err_t ambient_led_set_emotion(const char *emotion);
void ambient_led_clear_emotion(void);

/* 定时开关：使用灯效任务时间戳，不阻塞调用者。 */
esp_err_t ambient_led_on(void);
esp_err_t ambient_led_off(void);
esp_err_t ambient_led_off_after(uint32_t duration_ms);

/* 旧接口保留：等价于手动熄灭并保持，下一次 set_rgb/on 会恢复控制。 */
void ambient_led_all_off(void);
uint32_t ambient_led_count(ambient_led_id_t id);

#ifdef __cplusplus
}
#endif
