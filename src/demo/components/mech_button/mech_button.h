#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 普通机械按键（BOARD_KEY_GPIO）。
 *
 * 组件名叫 mech_button 而不是 button，是被迫的：espressif__esp_lvgl_port 的
 * CMakeLists 里有 `if("button" IN_LIST build_components)` 这种对"名为 button
 * 的组件"的探测（它找的是 espressif/button = iot_button）。组件一旦叫 button，
 * 就会把 esp_lvgl_port_button.c 拉进编译并链到我们这条库上，然后满屏
 * button_handle_t / iot_button_create 未定义。改名是唯一干净的解法。
 * 对外 API 仍保持 button_*，见下方声明。
 *
 * 硬件：外部上拉，按下拉低 —— 空闲 = 高电平，按下 = 低电平。
 * 去抖：独立任务按 BUTTON_POLL_PERIOD_MS 周期采样，同一电平连续保持
 *       BUTTON_DEBOUNCE_MS 才认作一次状态变化，避免按下/抬起瞬间的
 *       机械抖动被当成多次事件。
 * 手势：单击（未触发长按的抬起）、长按（按住达到阈值，只报一次）。
 *
 * 本组件只做"驱动 + 事件"，不直接控制 UI、音频或灯效。等阶段 3 的
 * App Event Bus 落地后，把 button_event_handler 里改成投递 APP_EVT_* 即可，
 * 驱动层不需要改。
 *
 * 使用方式：
 *   button_init();
 *   button_register_callback(handler, NULL);
 *   bool down = button_is_pressed();
 */

/* 去抖确认时间：电平必须稳定这么久才认作变化（建议 30~50 ms）。 */
#define BUTTON_DEBOUNCE_MS        40
/* 长按阈值：按下后保持这么久触发一次长按。 */
#define BUTTON_LONG_PRESS_MS      900
/* 采样周期：同时决定长按判定精度。 */
#define BUTTON_POLL_PERIOD_MS     5
/* 采样任务栈与优先级：低于 ui_task(6) / camera_task(7)，不抢关键任务。 */
#define BUTTON_TASK_STACK         3072
#define BUTTON_TASK_PRIORITY      4

typedef enum {
    BUTTON_EVENT_PRESSED = 0,   /* 去抖后确认按下 */
    BUTTON_EVENT_RELEASED,      /* 去抖后确认抬起 */
    BUTTON_EVENT_CLICK,         /* 抬起且未触发长按 → 单击 */
    BUTTON_EVENT_LONG_PRESS,    /* 按住达到阈值，已报过一次 */
} button_event_t;

/* 回调在 button 采样任务上下文执行，必须短小、不能阻塞。
 * 需要做耗时处理时请自行转发到别的任务。 */
typedef void (*button_event_cb_t)(button_event_t event, void *user_ctx);

/* 配置 GPIO 并启动采样任务。可重复调用，第二次直接返回 ESP_OK。 */
esp_err_t button_init(void);

/* 返回去抖后的当前状态：true = 按下。未初始化时返回 false。 */
bool button_is_pressed(void);

/* 注册事件回调；重复注册会覆盖上一个。传 NULL 可注销。 */
esp_err_t button_register_callback(button_event_cb_t callback, void *user_ctx);

#ifdef __cplusplus
}
#endif
