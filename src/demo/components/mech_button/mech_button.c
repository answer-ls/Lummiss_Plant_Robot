#include "mech_button.h"

#include <stddef.h>

#include "board_pins.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "BUTTON";

/* 去抖按"连续采样次数"实现，换算只在这里做一次，避免两处各自算。 */
#define BUTTON_DEBOUNCE_SAMPLES (BUTTON_DEBOUNCE_MS / BUTTON_POLL_PERIOD_MS)

/* 只有采样任务写状态，别的上下文只读，所以用临界区保护读写即可。 */
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

static TaskHandle_t s_task;
static button_event_cb_t s_callback;
static void *s_callback_ctx;

static bool s_initialized;
static bool s_stable_pressed;       /* 去抖后的状态：true = 按下 */
static bool s_raw_pressed;          /* 上一次原始采样值 */
static uint8_t s_stable_samples;    /* 当前原始电平已连续保持的采样数 */
static bool s_long_press_reported;  /* 本次按住是否已经报过长按 */
static bool s_boot_hold;            /* 上电时该脚已处于按下态：这段按住整体作废，不产生任何手势 */
static int64_t s_press_started_us;

static bool read_raw_pressed(void)
{
    /* 外部上拉 + 按下拉低：低电平 = 按下。 */
    return gpio_get_level(BOARD_KEY_GPIO) == 0;
}

static void notify(button_event_t event)
{
    button_event_cb_t callback;
    void *ctx;

    portENTER_CRITICAL(&s_lock);
    callback = s_callback;
    ctx = s_callback_ctx;
    portEXIT_CRITICAL(&s_lock);

    if (callback != NULL) {
        callback(event, ctx);
    }
}

static void commit_state(bool pressed)
{
    bool long_press_reported;

    portENTER_CRITICAL(&s_lock);
    s_stable_pressed = pressed;
    long_press_reported = s_long_press_reported;
    if (pressed) {
        s_long_press_reported = false;
        s_press_started_us = esp_timer_get_time();
    }
    portEXIT_CRITICAL(&s_lock);

    if (s_boot_hold) {
        /* 上电时就已按住的那一段整体作废：不补发 pressed，抬起也不发
         * released / click。否则会冒出"没有 pressed 的长按"这种幽灵事件。
         * s_boot_hold 只由初始化和本任务（唯一的采样上下文）读写。 */
        if (!pressed) {
            s_boot_hold = false;
        }
        return;
    }

    /* 事件在临界区外回调，避免占用者把中断关得太久。 */
    if (pressed) {
        ESP_LOGI(TAG, "pressed");
        notify(BUTTON_EVENT_PRESSED);
    } else {
        ESP_LOGI(TAG, "released");
        notify(BUTTON_EVENT_RELEASED);
        if (!long_press_reported) {
            /* 抬起时还没报过长按 → 这是一次单击。 */
            ESP_LOGI(TAG, "click");
            notify(BUTTON_EVENT_CLICK);
        }
    }
}

static void button_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "采样任务启动（CPU%d，周期 %d ms，栈 %d 字节，优先级 %d）",
             xPortGetCoreID(), BUTTON_POLL_PERIOD_MS, BUTTON_TASK_STACK,
             BUTTON_TASK_PRIORITY);

    TickType_t last_wake = xTaskGetTickCount();
    while (true) {
        const bool raw = read_raw_pressed();

        if (raw != s_raw_pressed) {
            /* 原始电平刚变化：重新开始计稳定时间。 */
            s_raw_pressed = raw;
            s_stable_samples = 0;
        } else if (s_stable_samples < BUTTON_DEBOUNCE_SAMPLES) {
            s_stable_samples++;
        }

        if (s_stable_samples >= BUTTON_DEBOUNCE_SAMPLES && raw != s_stable_pressed) {
            commit_state(raw);
        } else if (!s_boot_hold && s_stable_pressed && !s_long_press_reported) {
            /* 长按只报一次：按住不放不会每个周期重复触发。 */
            const int64_t held_us = esp_timer_get_time() - s_press_started_us;
            if (held_us >= (int64_t)BUTTON_LONG_PRESS_MS * 1000) {
                s_long_press_reported = true;
                ESP_LOGI(TAG, "long press (%d ms)", BUTTON_LONG_PRESS_MS);
                notify(BUTTON_EVENT_LONG_PRESS);
            }
        }

        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(BUTTON_POLL_PERIOD_MS));
    }
}

esp_err_t button_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    const gpio_config_t config = {
        .pin_bit_mask = 1ULL << BOARD_KEY_GPIO,
        .mode = GPIO_MODE_INPUT,
        /* 外部已有上拉，这里再开内部上拉兜底：外部电阻缺失时不会读到浮空。 */
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GPIO%d 配置失败：%s", BOARD_KEY_GPIO, esp_err_to_name(err));
        return err;
    }

    /* 先取一次初值，避免上电瞬间把"当前就按着"当成一次新的状态变化。 */
    s_raw_pressed = read_raw_pressed();
    s_stable_pressed = s_raw_pressed;
    s_stable_samples = BUTTON_DEBOUNCE_SAMPLES;
    s_long_press_reported = false;
    /* 上电即按下（按键卡住，或该脚被板级硬件拉到 GND）时，把这段按住作废：
     * 不发 pressed，也不会在 900 ms 后冒出"没有 pressed 的长按"。 */
    s_boot_hold = s_stable_pressed;
    /* 长按计时从初始化时刻起算。若留 0（= esp_timer 的开机原点），一个上电
     * 就处于按下状态的引脚会在第一次采样时立刻满足"已按住 900 ms"，
     * 凭空报一次长按（且因为初值直接覆盖了 stable_pressed，不会有 pressed
     * 事件，看起来像是"没按下就长按了"）。 */
    s_press_started_us = esp_timer_get_time();

    /* 上电电平打出来：没接按键时若这里报"按下"，说明该脚被拉到了 GND，
     * 之后会持续误触发。 */
    const int level = gpio_get_level(BOARD_KEY_GPIO);
    ESP_LOGI(TAG, "上电初值：GPIO%d 电平=%d → %s", BOARD_KEY_GPIO, level,
             s_stable_pressed ? "按下" : "松开");
    if (s_stable_pressed) {
        ESP_LOGW(TAG, "GPIO%d 上电即为按下状态（电平=%d，松开时应为 1）。"
                      "内部上拉已使能却仍读到低电平，说明该脚被拉到了 GND。"
                      "这段按住已作废，不会产生按下/长按/单击事件；"
                      "请检查该脚是否短路到 GND、或外部上拉电阻缺失",
                 BOARD_KEY_GPIO, level);
    }

    const BaseType_t created = xTaskCreate(button_task, "button_task",
                                           BUTTON_TASK_STACK, NULL,
                                           BUTTON_TASK_PRIORITY, &s_task);
    if (created != pdPASS) {
        ESP_LOGE(TAG, "创建采样任务失败");
        return ESP_ERR_NO_MEM;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "初始化完成：GPIO%d（外部上拉，按下为低），去抖 %d ms，长按 %d ms",
             BOARD_KEY_GPIO, BUTTON_DEBOUNCE_MS, BUTTON_LONG_PRESS_MS);
    return ESP_OK;
}

bool button_is_pressed(void)
{
    bool pressed;

    portENTER_CRITICAL(&s_lock);
    pressed = s_stable_pressed;
    portEXIT_CRITICAL(&s_lock);

    return pressed;
}

esp_err_t button_register_callback(button_event_cb_t callback, void *user_ctx)
{
    portENTER_CRITICAL(&s_lock);
    s_callback = callback;
    s_callback_ctx = user_ctx;
    portEXIT_CRITICAL(&s_lock);

    return ESP_OK;
}
