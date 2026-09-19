#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 两路 TTP223 电容触摸按键（BOARD_TOUCH_1_GPIO / BOARD_TOUCH_2_GPIO）。
 *
 * TTP223 模块自身完成电容检测并输出数字电平，ESP32-P4 这里只把它当普通 GPIO
 * 输入读：不使用 P4 内部 touch 外设，也不占用 P4 的触摸通道，所以通道数、
 * 触摸校准、防水配置这些都不适用。
 *
 * 电平定义由 BOARD_TOUCH_ACTIVE_LOW 决定（见 board_pins.h）：
 *   0（默认，模块背面 A 焊盘断开）= 空闲低电平，触摸时输出高电平；
 *   1（A 焊盘短接）              = 空闲高电平，触摸时输出低电平。
 * 上电时驱动按极性打开内部对侧上/下拉兜底，模块缺电或线松时不会读到浮空翻转。
 *
 * 命名说明：GPIO52/51 与"左/右"的对应关系目前没有确定，所以驱动层只用
 * TOUCH_1 / TOUCH_2，不擅自叫左右。业务层确定后再在事件映射处对应到
 * 盆栽陪伴机器人_开发文档.md 第 5 节的 App Event Bus，例如：
 *   TOUCH_KEY_1 + TOUCH_EVENT_CLICK → APP_EVT_TOUCH_LEFT
 *   TOUCH_KEY_2 + TOUCH_EVENT_CLICK → APP_EVT_TOUCH_RIGHT
 *   TOUCH_KEY_2 + TOUCH_EVENT_DOUBLE_CLICK → APP_EVT_TOUCH_RIGHT_DOUBLE
 * 本组件不引用这些事件名，避免在左右关系未定时写死映射。
 */

/* 去抖确认时间：电平必须稳定这么久才认作变化，避免干扰导致的误触发。 */
#define TOUCH_KEY_DEBOUNCE_MS      40
/* 长按阈值：按住保持这么久触发一次长按。 */
#define TOUCH_KEY_LONG_PRESS_MS    900
/* 双击窗口：第一次抬起后这么久内再次按下才算双击。 */
#define TOUCH_KEY_DOUBLE_CLICK_MS  350
/* 采样周期：同时决定长按和双击窗口的判定精度。 */
#define TOUCH_KEY_POLL_PERIOD_MS   5
/* 采样任务栈与优先级。 */
#define TOUCH_KEY_TASK_STACK       3072
#define TOUCH_KEY_TASK_PRIORITY    4

typedef enum {
    TOUCH_KEY_1 = 0,    /* GPIO52 → TTP223_OUT1 */
    TOUCH_KEY_2,        /* GPIO51 → TTP223_OUT2 */
    TOUCH_KEY_COUNT
} touch_key_id_t;

typedef enum {
    TOUCH_EVENT_PRESSED = 0,    /* 去抖后确认触摸 */
    TOUCH_EVENT_RELEASED,       /* 去抖后确认松手 */
    TOUCH_EVENT_CLICK,          /* 单击（双击窗口内没有第二次按下） */
    TOUCH_EVENT_LONG_PRESS,     /* 长按，已报过一次 */
    TOUCH_EVENT_DOUBLE_CLICK,   /* 双击 */
} touch_event_t;

/* 回调在 touch_key 采样任务上下文执行，必须短小、不能阻塞。
 * 单击是在双击窗口过期后才补发的，所以比抬起晚 TOUCH_KEY_DOUBLE_CLICK_MS。 */
typedef void (*touch_key_cb_t)(touch_key_id_t key, touch_event_t event, void *user_ctx);

/* 配置两路 GPIO 并启动采样任务。可重复调用，第二次直接返回 ESP_OK。 */
esp_err_t touch_key_init(void);

/* 返回去抖后的当前状态：true = 正在触摸。越界或未初始化返回 false。 */
bool touch_key_is_pressed(touch_key_id_t key);

/* 注册事件回调；重复注册会覆盖上一个。传 NULL 可注销。 */
esp_err_t touch_key_register_callback(touch_key_cb_t callback, void *user_ctx);

#ifdef __cplusplus
}
#endif
