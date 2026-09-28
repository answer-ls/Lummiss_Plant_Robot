#include "touch_key.h"

#include <stddef.h>

#include "board_pins.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "TOUCH";

/* 去抖按"连续采样次数"实现，换算只在这里做一次。 */
#define TOUCH_KEY_DEBOUNCE_SAMPLES (TOUCH_KEY_DEBOUNCE_MS / TOUCH_KEY_POLL_PERIOD_MS)
/* 双击窗口换算成微秒，避免每次比较都乘一遍。 */
#define TOUCH_KEY_DOUBLE_CLICK_US  ((int64_t)TOUCH_KEY_DOUBLE_CLICK_MS * 1000)
#define TOUCH_KEY_LONG_PRESS_US    ((int64_t)TOUCH_KEY_LONG_PRESS_MS * 1000)

/* 电平约定：触摸态与空闲态互为反向，只由 BOARD_TOUCH_ACTIVE_LOW 决定。
 * 初始化日志用它把"读到 0/1"翻成人能看的状态，免得对着数字猜。 */
#define TOUCH_KEY_TOUCH_LEVEL (BOARD_TOUCH_ACTIVE_LOW ? 0 : 1)
#define TOUCH_KEY_IDLE_LEVEL  (BOARD_TOUCH_ACTIVE_LOW ? 1 : 0)

/* 只有采样任务写这些状态，别的上下文只读 touch_key_is_pressed()，用临界区保护即可。 */
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

typedef struct {
    gpio_num_t pin;
    bool raw_pressed;           /* 上一次原始采样 */
    bool stable_pressed;        /* 去抖后的状态 */
    uint8_t stable_samples;     /* 当前原始电平已连续保持的采样数 */
    bool long_press_reported;   /* 本次按住是否已经报过长按 */
    bool pending_click;         /* 单击挂起，等双击窗口过期再补发 */
    bool suppress_click;        /* 双击的第二次抬起不再单独算单击 */
    bool boot_hold;             /* 上电时该脚已处于激活态：这段按住整体作废，不产生任何手势 */
    int64_t press_started_us;
    int64_t released_at_us;     /* 上一次抬起时刻，用于双击窗口判断 */
} touch_key_state_t;

static touch_key_state_t s_keys[TOUCH_KEY_COUNT] = {
    { .pin = BOARD_TOUCH_1_GPIO },
    { .pin = BOARD_TOUCH_2_GPIO },
};

/* 日志里用 touch1 / touch2，不用左右。 */
static const char *const s_key_names[TOUCH_KEY_COUNT] = {
    "touch1",
    "touch2",
};

static TaskHandle_t s_task;
static touch_key_cb_t s_callback;
static void *s_callback_ctx;
static bool s_initialized;

static bool read_raw_pressed(gpio_num_t pin)
{
#if BOARD_TOUCH_ACTIVE_LOW
    /* A 焊盘短接：触摸时输出低电平。 */
    return gpio_get_level(pin) == 0;
#else
    /* 模块出厂默认：空闲低电平，触摸时输出高电平。 */
    return gpio_get_level(pin) != 0;
#endif
}

static void notify(touch_key_id_t key, touch_event_t event)
{
    touch_key_cb_t callback;
    void *ctx;

    portENTER_CRITICAL(&s_lock);
    callback = s_callback;
    ctx = s_callback_ctx;
    portEXIT_CRITICAL(&s_lock);

    if (callback != NULL) {
        callback(key, event, ctx);
    }
}

static void log_event(touch_key_id_t key, touch_event_t event)
{
    switch (event) {
    case TOUCH_EVENT_PRESSED:
        ESP_LOGI(TAG, "%s pressed", s_key_names[key]);
        break;
    case TOUCH_EVENT_RELEASED:
        ESP_LOGI(TAG, "%s released", s_key_names[key]);
        break;
    case TOUCH_EVENT_CLICK:
        ESP_LOGI(TAG, "%s click", s_key_names[key]);
        break;
    case TOUCH_EVENT_LONG_PRESS:
        ESP_LOGI(TAG, "%s long press (%d ms)", s_key_names[key], TOUCH_KEY_LONG_PRESS_MS);
        break;
    case TOUCH_EVENT_DOUBLE_CLICK:
        ESP_LOGI(TAG, "%s double click", s_key_names[key]);
        break;
    }
}

static void emit(touch_key_id_t key, touch_event_t event)
{
    log_event(key, event);
    notify(key, event);
}

static void handle_level_change(touch_key_id_t id, bool pressed, int64_t now_us)
{
    touch_key_state_t *key = &s_keys[id];

    key->stable_pressed = pressed;

    if (key->boot_hold) {
        /* 上电瞬间就已经处在激活态的那一段"按住"整体作废：不补发 pressed，
         * 等它真正回到空闲态时也不发 released。否则会冒出"没有 pressed 的
         * 长按"或"没有 pressed 的 released"这类幽灵事件，把下游状态机带乱。
         * 只在它确实回到空闲态时清标志，之后的手势恢复正常识别。 */
        if (!pressed) {
            key->boot_hold = false;
        }
        return;
    }

    if (pressed) {
        const bool within_double_click_window =
            key->pending_click && (now_us - key->released_at_us) <= TOUCH_KEY_DOUBLE_CLICK_US;

        key->long_press_reported = false;
        key->press_started_us = now_us;

        if (within_double_click_window) {
            /* 第二次按下落在双击窗口内：合并成一次双击，第二次抬起不再算单击。 */
            key->pending_click = false;
            key->suppress_click = true;
            emit(id, TOUCH_EVENT_DOUBLE_CLICK);
        } else {
            if (key->pending_click) {
                /* 第一击的窗口已经过期，但采样周期还没轮到补发：在这里补发，
                 * 否则这次按下会把上一击的单击事件吃掉。 */
                key->pending_click = false;
                emit(id, TOUCH_EVENT_CLICK);
            }
            key->suppress_click = false;
            emit(id, TOUCH_EVENT_PRESSED);
        }
        return;
    }

    emit(id, TOUCH_EVENT_RELEASED);

    if (key->long_press_reported) {
        /* 长按已经算一次完整手势，抬起不再算单击。 */
        key->long_press_reported = false;
        key->suppress_click = false;
        return;
    }

    if (key->suppress_click) {
        key->suppress_click = false;
        return;
    }

    /* 先挂起：等过了双击窗口没有第二次按下，才补发单击。 */
    key->pending_click = true;
    key->released_at_us = now_us;
}

static void touch_key_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "采样任务启动（CPU%d，周期 %d ms，栈 %d 字节，优先级 %d）",
             xPortGetCoreID(), TOUCH_KEY_POLL_PERIOD_MS, TOUCH_KEY_TASK_STACK,
             TOUCH_KEY_TASK_PRIORITY);

    TickType_t last_wake = xTaskGetTickCount();
    while (true) {
        const int64_t now_us = esp_timer_get_time();

        for (size_t i = 0; i < TOUCH_KEY_COUNT; ++i) {
            touch_key_state_t *key = &s_keys[i];
            const touch_key_id_t id = (touch_key_id_t)i;
            const bool raw = read_raw_pressed(key->pin);

            if (raw != key->raw_pressed) {
                /* 原始电平刚变化：重新开始计稳定时间，抖动被吃掉。 */
                key->raw_pressed = raw;
                key->stable_samples = 0;
            } else if (key->stable_samples < TOUCH_KEY_DEBOUNCE_SAMPLES) {
                key->stable_samples++;
            }

            if (key->stable_samples >= TOUCH_KEY_DEBOUNCE_SAMPLES &&
                raw != key->stable_pressed) {
                handle_level_change(id, raw, now_us);
                continue;
            }

            if (!key->boot_hold && key->stable_pressed && !key->long_press_reported &&
                (now_us - key->press_started_us) >= TOUCH_KEY_LONG_PRESS_US) {
                /* 长按只报一次；它已经算完整手势，顺手取消挂起的单击判定。 */
                key->long_press_reported = true;
                key->pending_click = false;
                emit(id, TOUCH_EVENT_LONG_PRESS);
                continue;
            }

            if (key->pending_click &&
                (now_us - key->released_at_us) > TOUCH_KEY_DOUBLE_CLICK_US) {
                key->pending_click = false;
                emit(id, TOUCH_EVENT_CLICK);
            }
        }

        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(TOUCH_KEY_POLL_PERIOD_MS));
    }
}

esp_err_t touch_key_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    uint64_t pin_mask = 0;
    for (size_t i = 0; i < TOUCH_KEY_COUNT; ++i) {
        pin_mask |= 1ULL << s_keys[i].pin;
    }

    const gpio_config_t config = {
        .pin_bit_mask = pin_mask,
        .mode = GPIO_MODE_INPUT,
        /* TTP223 是推挽输出，内部上下拉只是模块未上电/线松时的兜底，
         * 方向按模块的输出极性取反。 */
#if BOARD_TOUCH_ACTIVE_LOW
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
#else
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
#endif
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GPIO%d/GPIO%d 配置失败：%s", BOARD_TOUCH_1_GPIO,
                 BOARD_TOUCH_2_GPIO, esp_err_to_name(err));
        return err;
    }

    /* 先取一次初值，避免上电瞬间把"当前正被触摸"当成一次新的状态变化。 */
    const int64_t init_us = esp_timer_get_time();
    for (size_t i = 0; i < TOUCH_KEY_COUNT; ++i) {
        s_keys[i].raw_pressed = read_raw_pressed(s_keys[i].pin);
        s_keys[i].stable_pressed = s_keys[i].raw_pressed;
        s_keys[i].stable_samples = TOUCH_KEY_DEBOUNCE_SAMPLES;
        s_keys[i].long_press_reported = false;
        s_keys[i].pending_click = false;
        s_keys[i].suppress_click = false;
        /* 上电时就已经是激活态的脚，把这一段按住标记为作废：既不发 pressed，
         * 也不会在 900 ms 后冒出一个没有前置 pressed 的长按（见 handle_level_change）。 */
        s_keys[i].boot_hold = s_keys[i].stable_pressed;
        /* 长按计时从初始化时刻起算。若留 0（= esp_timer 的开机原点），一个上电
         * 就已经处于触摸态的脚会在第一次采样时立刻满足"已按住 900 ms"，
         * 凭空报一次长按——这正是 2026-09-18 串口日志里只有 long press、
         * 没有 pressed 的那个幽灵事件。 */
        s_keys[i].press_started_us = init_us;
        s_keys[i].released_at_us = init_us;
    }

    /* 把上电时的实际电平打出来。这是"手头没有 TTP223 也能判断接线"的关键：
     * 如果这里报"触摸态"，而此刻并没接模块，说明该脚被板级硬件拉到了触摸
     * 电平（或浮空），之后必然持续误触发，得先查线再谈调参。 */
    for (size_t i = 0; i < TOUCH_KEY_COUNT; ++i) {
        const int level = gpio_get_level(s_keys[i].pin);
        ESP_LOGI(TAG, "上电初值：%s GPIO%d 电平=%d → %s", s_key_names[i],
                 s_keys[i].pin, level,
                 s_keys[i].stable_pressed ? "触摸态" : "空闲");
        if (s_keys[i].stable_pressed) {
            ESP_LOGW(TAG, "%s 上电即为触摸态（GPIO%d 电平=%d；按当前极性，"
                          "触摸时应为 %d，闲置时应为 %d）。内部下拉已使能却仍读到"
                          "触摸电平，说明该脚被板级硬件拉高（外部上拉或别的器件输出）。"
                          "这段按住已作废，不会产生任何手势事件；请先实测该脚/查原理图",
                     s_key_names[i], s_keys[i].pin, level, TOUCH_KEY_TOUCH_LEVEL,
                     TOUCH_KEY_IDLE_LEVEL);
        }
    }

    /* 任务仅处理 GPIO/业务状态；DMA/中断资源由底层驱动独立持有。 */
    const BaseType_t created = xTaskCreateWithCaps(touch_key_task, "touch_key_task",
                                           TOUCH_KEY_TASK_STACK, NULL,
                                           TOUCH_KEY_TASK_PRIORITY, &s_task,
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (created != pdPASS) {
        ESP_LOGE(TAG, "创建采样任务失败");
        return ESP_ERR_NO_MEM;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "初始化完成：touch1=GPIO%d，touch2=GPIO%d，去抖 %d ms，"
                  "长按 %d ms，双击窗口 %d ms",
             BOARD_TOUCH_1_GPIO, BOARD_TOUCH_2_GPIO, TOUCH_KEY_DEBOUNCE_MS,
             TOUCH_KEY_LONG_PRESS_MS, TOUCH_KEY_DOUBLE_CLICK_MS);
#if BOARD_TOUCH_ACTIVE_LOW
    ESP_LOGI(TAG, "输出极性：触摸时为低电平（BOARD_TOUCH_ACTIVE_LOW=1）");
#else
    ESP_LOGI(TAG, "输出极性：触摸时为高电平（BOARD_TOUCH_ACTIVE_LOW=0，模块默认）");
#endif
    return ESP_OK;
}

bool touch_key_is_pressed(touch_key_id_t key)
{
    if (key >= TOUCH_KEY_COUNT) {
        return false;
    }

    bool pressed;

    portENTER_CRITICAL(&s_lock);
    pressed = s_keys[key].stable_pressed;
    portEXIT_CRITICAL(&s_lock);

    return pressed;
}

esp_err_t touch_key_register_callback(touch_key_cb_t callback, void *user_ctx)
{
    portENTER_CRITICAL(&s_lock);
    s_callback = callback;
    s_callback_ctx = user_ctx;
    portEXIT_CRITICAL(&s_lock);

    return ESP_OK;
}
