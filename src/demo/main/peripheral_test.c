#include "peripheral_test.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ambient_led.h"
#include "board_pins.h"
#include "mech_button.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "touch_key.h"

static const char *TAG = "PERIPH_TEST";

#define PERIPH_TEST_TASK_STACK     4096
#define PERIPH_TEST_TASK_PRIORITY  3

/* 开机静默期自检：一次性的窗口，结束时打印结论。窗口长度必须大于长按阈值
 * （TOUCH_KEY_LONG_PRESS_MS = 900 ms），否则会把幽灵长按漏在窗口外——这正是
 * 2026-09-18 手工看日志时踩的坑（日志只截到 1.3 s，判定点在 2.1 s）。 */
#define PERIPH_QUIET_WINDOW_MS     3000
#define PERIPH_QUIET_TASK_STACK    3072

/* 通道位掩码：一步里要同时点亮的通道。掩码里没有的通道会被熄灭，
 * 这样每一步都是自解释的，不会因为上一步残留而看错。 */
#define PERIPH_CH_A  0x1u
#define PERIPH_CH_B  0x2u

typedef struct {
    uint8_t channel_mask;
    uint8_t r;
    uint8_t g;
    uint8_t b;
    uint8_t brightness;             /* 全局亮度，255 = 不缩放 */
    ambient_led_effect_t effect;
    uint32_t hold_ms;
    const char *name;
} periph_test_step_t;

/* 固定序列，循环播放。前 8 步单路验证红绿蓝，随后是两路同时、
 * 亮度缩放、呼吸、流水、闪烁。 */
static const periph_test_step_t s_steps[] = {
    { PERIPH_CH_A, 255,   0,   0, 255, AMBIENT_LED_EFFECT_STATIC, 1500, "A 单独：红" },
    { PERIPH_CH_A,   0, 255,   0, 255, AMBIENT_LED_EFFECT_STATIC, 1500, "A 单独：绿" },
    { PERIPH_CH_A,   0,   0, 255, 255, AMBIENT_LED_EFFECT_STATIC, 1500, "A 单独：蓝" },
    { PERIPH_CH_A,   0,   0,   0, 255, AMBIENT_LED_EFFECT_STATIC,  600, "A 单独：熄灭" },

    { PERIPH_CH_B, 255,   0,   0, 255, AMBIENT_LED_EFFECT_STATIC, 1500, "B 单独：红" },
    { PERIPH_CH_B,   0, 255,   0, 255, AMBIENT_LED_EFFECT_STATIC, 1500, "B 单独：绿" },
    { PERIPH_CH_B,   0,   0, 255, 255, AMBIENT_LED_EFFECT_STATIC, 1500, "B 单独：蓝" },
    { PERIPH_CH_B,   0,   0,   0, 255, AMBIENT_LED_EFFECT_STATIC,  600, "B 单独：熄灭" },

    { PERIPH_CH_A | PERIPH_CH_B, 255, 255, 255, 255, AMBIENT_LED_EFFECT_STATIC, 2000,
      "A+B 同时：白（两路并行）" },
    { PERIPH_CH_A | PERIPH_CH_B, 255,   0,   0, 255, AMBIENT_LED_EFFECT_STATIC, 2000,
      "A+B 同时：红（两路并行）" },

    { PERIPH_CH_A, 255,   0,   0, 128, AMBIENT_LED_EFFECT_STATIC, 1500, "亮度 50%：A 红" },
    { PERIPH_CH_A, 255,   0,   0,  32, AMBIENT_LED_EFFECT_STATIC, 1500, "亮度 12%：A 红" },
    { PERIPH_CH_A | PERIPH_CH_B, 0, 128, 255, 255, AMBIENT_LED_EFFECT_BREATH, 6000,
      "A+B 呼吸：青" },
    { PERIPH_CH_A, 255, 128,   0, 255, AMBIENT_LED_EFFECT_FLOW, 6000, "A 流水：橙" },
    { PERIPH_CH_B, 128,   0, 255, 255, AMBIENT_LED_EFFECT_FLOW, 6000, "B 流水：紫" },
    { PERIPH_CH_A | PERIPH_CH_B, 255, 165,   0, 255, AMBIENT_LED_EFFECT_BLINK, 4000,
      "A+B 闪烁：橙" },
    { PERIPH_CH_A | PERIPH_CH_B,   0,   0,   0, 255, AMBIENT_LED_EFFECT_STATIC, 800,
      "全部熄灭" },
};

static const char *button_event_name(button_event_t event)
{
    switch (event) {
    case BUTTON_EVENT_PRESSED:
        return "PRESSED";
    case BUTTON_EVENT_RELEASED:
        return "RELEASED";
    case BUTTON_EVENT_CLICK:
        return "CLICK";
    case BUTTON_EVENT_LONG_PRESS:
        return "LONG_PRESS";
    default:
        return "UNKNOWN";
    }
}

static const char *touch_event_name(touch_event_t event)
{
    switch (event) {
    case TOUCH_EVENT_PRESSED:
        return "PRESSED";
    case TOUCH_EVENT_RELEASED:
        return "RELEASED";
    case TOUCH_EVENT_CLICK:
        return "CLICK";
    case TOUCH_EVENT_LONG_PRESS:
        return "LONG_PRESS";
    case TOUCH_EVENT_DOUBLE_CLICK:
        return "DOUBLE_CLICK";
    default:
        return "UNKNOWN";
    }
}

/* 开机静默期的事件计数：这段窗口里没人碰按键/触摸，所以正常应当全是 0。
 * 2026-09-18 那个"只有 long press、没有 pressed"的幽灵事件就落在这个窗口内，
 * 用末尾那一行结论把它钉死，省得每次都要人工翻整段串口输出。
 * 只在回调里自增，单字读写，不需要加锁。 */
static volatile uint32_t s_button_event_count;
static volatile uint32_t s_touch_event_count[TOUCH_KEY_COUNT];

/* 驱动自己已经打了 "BUTTON: ..." / "TOUCH: ..."；这里额外打印一次状态快照，
 * 用来确认回调通路和 button_is_pressed()/touch_key_is_pressed() 的读数一致。 */
static void button_event_handler(button_event_t event, void *user_ctx)
{
    (void)user_ctx;
    s_button_event_count++;
    ESP_LOGI(TAG, "回调校验：button event=%s is_pressed=%d",
             button_event_name(event), button_is_pressed() ? 1 : 0);
}

static void touch_event_handler(touch_key_id_t key, touch_event_t event, void *user_ctx)
{
    (void)user_ctx;
    s_touch_event_count[key]++;
    ESP_LOGI(TAG, "回调校验：touch%d event=%s is_pressed=%d", (int)key + 1,
             touch_event_name(event), touch_key_is_pressed(key) ? 1 : 0);
}

static void apply_step(const periph_test_step_t *step)
{
    ambient_led_set_brightness(step->brightness);

    for (ambient_led_id_t id = AMBIENT_LED_A; id < AMBIENT_LED_COUNT; ++id) {
        const uint8_t bit = (uint8_t)(1u << (uint32_t)id);
        if ((step->channel_mask & bit) != 0u) {
            /* set_rgb 会顺带切到常亮，所以必须再切一次灯效。 */
            const ambient_led_target_t target =
                id == AMBIENT_LED_A ? AMBIENT_LED_TARGET_A : AMBIENT_LED_TARGET_B;
            ambient_led_set_rgb(target, step->r, step->g, step->b,
                                step->brightness);
            ambient_led_set_effect(id, step->effect);
        } else {
            const ambient_led_target_t target =
                id == AMBIENT_LED_A ? AMBIENT_LED_TARGET_A : AMBIENT_LED_TARGET_B;
            ambient_led_set_rgb(target, 0, 0, 0, 0);
        }
    }
}

static void peripheral_test_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "灯效序列任务启动（CPU%d，栈 %d 字节，优先级 %d）",
             xPortGetCoreID(), PERIPH_TEST_TASK_STACK, PERIPH_TEST_TASK_PRIORITY);

    const size_t step_count = sizeof(s_steps) / sizeof(s_steps[0]);
    size_t index = 0;

    while (true) {
        const periph_test_step_t *step = &s_steps[index];
        ESP_LOGI(TAG, "灯效 %u/%u：%s（%u ms，亮度 %u）", (unsigned)(index + 1),
                 (unsigned)step_count, step->name, (unsigned)step->hold_ms,
                 (unsigned)step->brightness);

        apply_step(step);
        vTaskDelay(pdMS_TO_TICKS(step->hold_ms));

        index = (index + 1u) % step_count;
    }
}

/* 一次性任务：等静默期过完，把窗口内的输入事件数打出来并给结论，然后自杀。
 * 目的是把"上电即激活的引脚会不会冒出幽灵事件"变成一个无需人工翻日志的判据。 */
static void quiet_check_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(PERIPH_QUIET_WINDOW_MS));

    const uint32_t button_events = s_button_event_count;
    const uint32_t touch1_events = s_touch_event_count[TOUCH_KEY_1];
    const uint32_t touch2_events = s_touch_event_count[TOUCH_KEY_2];
    const uint32_t total = button_events + touch1_events + touch2_events;

    ESP_LOGI(TAG, "开机静默期自检（%d ms 内）：按键事件 %u，touch1 事件 %u，"
                  "touch2 事件 %u；当前电平态：button=%s touch1=%s touch2=%s",
             PERIPH_QUIET_WINDOW_MS, (unsigned)button_events,
             (unsigned)touch1_events, (unsigned)touch2_events,
             button_is_pressed() ? "按下" : "松开",
             touch_key_is_pressed(TOUCH_KEY_1) ? "触摸" : "空闲",
             touch_key_is_pressed(TOUCH_KEY_2) ? "触摸" : "空闲");

    if (total == 0u) {
        ESP_LOGI(TAG, "自检结论：静默期内没有任何输入事件 → 通过（无幽灵事件）");
    } else {
        ESP_LOGW(TAG, "自检结论：静默期内出现 %u 个输入事件。若这段时间没人碰按键/"
                      "触摸，说明有引脚上电即处于激活态、或电平被板级硬件拉动，"
                      "对照上面 BUTTON:/TOUCH: 的日志定位",
                 (unsigned)total);
    }

    vTaskDelete(NULL);
}

esp_err_t peripheral_test_start(void)
{
    ESP_LOGI(TAG, "外设自检启动：KEY=GPIO%d TOUCH1=GPIO%d TOUCH2=GPIO%d "
                  "WS2812_A=GPIO%d(x%d) WS2812_B=GPIO%d(x%d)",
             BOARD_KEY_GPIO, BOARD_TOUCH_1_GPIO, BOARD_TOUCH_2_GPIO,
             BOARD_WS2812_A_GPIO, (int)BOARD_WS2812_A_LED_COUNT,
             BOARD_WS2812_B_GPIO, (int)BOARD_WS2812_B_LED_COUNT);
    ESP_LOGI(TAG, "预期日志：BUTTON: pressed / TOUCH: touch1 pressed / "
                  "AMBIENT_LED: A initialized GPIO20");
    /* 手头没有按键/触摸模块时，用一根杜邦线就能把输入通路整条验完：
     * 电平变化 → 去抖 → 事件 → 回调，全都走真实代码路径。 */
    ESP_LOGI(TAG, "无实物验证：KEY(GPIO%d) 短到 GND 应打出 BUTTON: pressed→click；"
                  "TOUCH1(GPIO%d) 短到 3V3 应打出 TOUCH: touch1 pressed；"
                  "TOUCH2(GPIO%d) 短到 3V3 同理（按当前极性）",
             BOARD_KEY_GPIO, BOARD_TOUCH_1_GPIO, BOARD_TOUCH_2_GPIO);

    esp_err_t err = button_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "按键初始化失败：%s", esp_err_to_name(err));
        return err;
    }
    button_register_callback(button_event_handler, NULL);

    err = touch_key_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "触摸初始化失败：%s", esp_err_to_name(err));
        return err;
    }
    touch_key_register_callback(touch_event_handler, NULL);

    err = ambient_led_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "氛围灯初始化失败：%s", esp_err_to_name(err));
        return err;
    }

    const BaseType_t created = xTaskCreate(peripheral_test_task, "periph_test",
                                           PERIPH_TEST_TASK_STACK, NULL,
                                           PERIPH_TEST_TASK_PRIORITY, NULL);
    if (created != pdPASS) {
        ESP_LOGE(TAG, "创建灯效序列任务失败");
        return ESP_ERR_NO_MEM;
    }

    /* 静默期自检只是诊断，起不来不该影响灯效序列，所以只告警不返回错误。 */
    const BaseType_t quiet_created = xTaskCreate(quiet_check_task, "periph_quiet",
                                                 PERIPH_QUIET_TASK_STACK, NULL,
                                                 PERIPH_TEST_TASK_PRIORITY, NULL);
    if (quiet_created != pdPASS) {
        ESP_LOGW(TAG, "创建静默期自检任务失败（不影响灯效序列）");
    }

    ESP_LOGI(TAG, "外设自检就绪：按键和触摸会实时打日志，灯效按固定序列循环，"
                  "%d ms 后打印一次静默期自检结论", PERIPH_QUIET_WINDOW_MS);
    return ESP_OK;
}
