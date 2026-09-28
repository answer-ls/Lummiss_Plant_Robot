#include "peripheral_test.h"
#include "test_profile.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ambient_led.h"
#include "board_pins.h"
#include "mech_button.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "stepper_motor.h"
#include "touch_key.h"

static const char *TAG = "PERIPH_TEST";

#define PERIPH_TEST_TASK_STACK     4096
#define PERIPH_TEST_TASK_PRIORITY  3

/* 新 PCB 当前只排查 LCD。电机和两路 WS2812 都会产生明显的瞬时电流，
 * 实机日志已确认电机开始 CW 后立即触发 BOD，导致 UI Task 尚未运行就复位。
 * 屏幕验证结束后改回 0，即可恢复电机和灯带综合自检。 */
#define PERIPH_LCD_DIAGNOSTIC_ONLY  1

#if BOARD_HAS_STEPPER && (!PERIPH_LCD_DIAGNOSTIC_ONLY || CAMERA_TEST_PROFILE == CAMERA_TEST_STEPPER_ONLY)
#define STEPPER_TEST_TASK_STACK       3072
#define STEPPER_TEST_TASK_PRIORITY    4
#define STEPPER_TEST_STEPS            20U
#define STEPPER_TEST_HOLD_MS          500U
#define STEPPER_TEST_REPEAT_DELAY_MS  5000U
#define STEPPER_TEST_CORE             1
#endif

/* 开机静默期自检：一次性的窗口，结束时打印结论。窗口长度必须大于长按阈值
 * （TOUCH_KEY_LONG_PRESS_MS = 900 ms），否则会把幽灵长按漏在窗口外——这正是
 * 2026-09-18 手工看日志时踩的坑（日志只截到 1.3 s，判定点在 2.1 s）。 */
#define PERIPH_QUIET_WINDOW_MS     3000
#define PERIPH_QUIET_TASK_STACK    3072

/* 机械限位使用轮询去抖，避免触点抖动连续打印触发/释放。 */
#define LIMIT_POLL_PERIOD_MS       10
#define LIMIT_DEBOUNCE_MS          30
#define LIMIT_DEBOUNCE_SAMPLES     (LIMIT_DEBOUNCE_MS / LIMIT_POLL_PERIOD_MS)
#define LIMIT_TASK_STACK           3072
#define LIMIT_TASK_PRIORITY        4

typedef struct {
    gpio_num_t gpio;
    const char *name;
    bool raw_active;
    bool stable_active;
    uint8_t stable_samples;
} limit_input_t;

static limit_input_t s_limits[] = {
    { .gpio = BOARD_LIMIT_1_GPIO, .name = "LIMIT1" },
    { .gpio = BOARD_LIMIT_2_GPIO, .name = "LIMIT2" },
};

static volatile uint32_t s_limit_event_count[2];

static bool limit_read_active(gpio_num_t gpio)
{
#if BOARD_LIMIT_ACTIVE_LOW
    return gpio_get_level(gpio) == 0;
#else
    return gpio_get_level(gpio) != 0;
#endif
}

static void limit_input_task(void *arg)
{
    (void)arg;
    TickType_t last_wake = xTaskGetTickCount();

    ESP_LOGI(TAG, "限位采样任务启动：周期=%d ms，去抖=%d ms，低电平触发",
             LIMIT_POLL_PERIOD_MS, LIMIT_DEBOUNCE_MS);

    while (true) {
        for (size_t i = 0; i < sizeof(s_limits) / sizeof(s_limits[0]); ++i) {
            limit_input_t *limit = &s_limits[i];
            const bool active = limit_read_active(limit->gpio);

            if (active != limit->raw_active) {
                limit->raw_active = active;
                limit->stable_samples = 0;
            } else if (limit->stable_samples < LIMIT_DEBOUNCE_SAMPLES) {
                limit->stable_samples++;
            }

            if (limit->stable_samples >= LIMIT_DEBOUNCE_SAMPLES &&
                active != limit->stable_active) {
                limit->stable_active = active;
                s_limit_event_count[i]++;
                ESP_LOGI(TAG, "%s GPIO%d %s（电平=%d）",
                         limit->name, (int)limit->gpio,
                         active ? "TRIGGERED/已触发" : "RELEASED/已释放",
                         gpio_get_level(limit->gpio));
            }
        }

        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(LIMIT_POLL_PERIOD_MS));
    }
}

static esp_err_t limit_inputs_init(void)
{
    const gpio_config_t config = {
        .pin_bit_mask = (1ULL << BOARD_LIMIT_1_GPIO) |
                        (1ULL << BOARD_LIMIT_2_GPIO),
        .mode = GPIO_MODE_INPUT,
        /* 原理图 R35/R38 已各自提供 10 kΩ 外部上拉；这里不再并联内部上拉。 */
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "限位 GPIO0/53 配置失败：%s", esp_err_to_name(err));
        return err;
    }

    for (size_t i = 0; i < sizeof(s_limits) / sizeof(s_limits[0]); ++i) {
        limit_input_t *limit = &s_limits[i];
        limit->raw_active = limit_read_active(limit->gpio);
        limit->stable_active = limit->raw_active;
        limit->stable_samples = LIMIT_DEBOUNCE_SAMPLES;
        ESP_LOGI(TAG, "%s 上电初值：GPIO%d 电平=%d → %s",
                 limit->name, (int)limit->gpio, gpio_get_level(limit->gpio),
                 limit->stable_active ? "已触发" : "未触发");
    }

    if (xTaskCreate(limit_input_task, "limit_input", LIMIT_TASK_STACK, NULL,
                    LIMIT_TASK_PRIORITY, NULL) != pdPASS) {
        ESP_LOGE(TAG, "创建限位采样任务失败");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "限位初始化完成：LIMIT1=GPIO%d LIMIT2=GPIO%d，外部10k上拉、闭合接地",
             BOARD_LIMIT_1_GPIO, BOARD_LIMIT_2_GPIO);
    return ESP_OK;
}

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

#if BOARD_HAS_STEPPER && (!PERIPH_LCD_DIAGNOSTIC_ONLY || CAMERA_TEST_PROFILE == CAMERA_TEST_STEPPER_ONLY)
/* 保持已验证的 GPIO 和相序，只改变步间隔；每档重新使能验证启动能力。
 * 无编码器，软件完成不代表没有失步，需要现场观察每档的正反转。 */
static void stepper_test_task(void *arg)
{
    (void)arg;
    static const uint32_t intervals_us[] = {100000U, 50000U, 20000U, 10000U};
    ESP_LOGI(TAG, "STEPPER_SPEED_TEST：3秒后开始，100/50/20/10ms，每档正反各20步");
    vTaskDelay(pdMS_TO_TICKS(3000));
    esp_err_t err = ESP_OK;
    unsigned completed = 0;
    for (unsigned i = 0; i < sizeof(intervals_us) / sizeof(intervals_us[0]); ++i) {
        const uint32_t interval = intervals_us[i];
        ESP_LOGI(TAG, "SPEED_BEGIN stage=%u/4 interval_us=%u steps=%u",
                 i + 1, (unsigned)interval, (unsigned)STEPPER_TEST_STEPS);
        err = stepper_motor_enable();
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "SPEED_MOVE stage=%u direction=cw", i + 1);
            err = stepper_motor_move_steps(STEPPER_TEST_STEPS, STEPPER_DIR_CW, interval);
        }
        if (err == ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(STEPPER_TEST_HOLD_MS));
            ESP_LOGI(TAG, "SPEED_MOVE stage=%u direction=ccw", i + 1);
            err = stepper_motor_move_steps(STEPPER_TEST_STEPS, STEPPER_DIR_CCW, interval);
        }
        /* 关闭前保存故障状态，避免休眠时的电平影响本档诊断。 */
        const bool fault = stepper_motor_is_fault();
        const esp_err_t disable_err = stepper_motor_disable();
        if (err == ESP_OK) {
            err = fault ? ESP_FAIL : disable_err;
        }
        ESP_LOGI(TAG, "SPEED_END stage=%u result=%s disable=%s fault=%d",
                 i + 1, esp_err_to_name(err), esp_err_to_name(disable_err), fault);
        if (err != ESP_OK) {
            /* 首次驱动失败即停止，不重试、不进入更快档位。 */
            break;
        }
        completed++;
        if (i + 1 < sizeof(intervals_us) / sizeof(intervals_us[0])) {
            ESP_LOGI(TAG, "线圈已关闭，等待5秒后开始下一档");
            vTaskDelay(pdMS_TO_TICKS(STEPPER_TEST_REPEAT_DELAY_MS));
        }
    }
    ESP_LOGI(TAG, "STEPPER_SPEED_SUMMARY completed=%u/4 result=%s（机械转动需观察）",
             completed, esp_err_to_name(err));
    vTaskDelete(NULL);
}

#endif

/* 电机隔离测试复用同一驱动和往返任务，不启动其它外设自检。 */
esp_err_t stepper_test_start(void)
{
#if BOARD_HAS_STEPPER && (!PERIPH_LCD_DIAGNOSTIC_ONLY || CAMERA_TEST_PROFILE == CAMERA_TEST_STEPPER_ONLY)
    esp_err_t err = stepper_motor_init();
    if (err != ESP_OK) {
        return err;
    }
    if (xTaskCreatePinnedToCore(stepper_test_task, "stepper_test",
                              STEPPER_TEST_TASK_STACK, NULL,
                              STEPPER_TEST_TASK_PRIORITY, NULL,
                              STEPPER_TEST_CORE) != pdPASS) {
        (void)stepper_motor_disable();
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
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
    const uint32_t limit1_events = s_limit_event_count[0];
    const uint32_t limit2_events = s_limit_event_count[1];
    const uint32_t total = button_events + touch1_events + touch2_events +
                           limit1_events + limit2_events;

    ESP_LOGI(TAG, "开机静默期自检（%d ms 内）：按键=%u touch1=%u touch2=%u "
                  "limit1=%u limit2=%u；当前状态：button=%s touch1=%s "
                  "touch2=%s limit1=%s limit2=%s",
             PERIPH_QUIET_WINDOW_MS, (unsigned)button_events,
             (unsigned)touch1_events, (unsigned)touch2_events,
             (unsigned)limit1_events, (unsigned)limit2_events,
             button_is_pressed() ? "按下" : "松开",
             touch_key_is_pressed(TOUCH_KEY_1) ? "触摸" : "空闲",
             touch_key_is_pressed(TOUCH_KEY_2) ? "触摸" : "空闲",
             s_limits[0].stable_active ? "触发" : "释放",
             s_limits[1].stable_active ? "触发" : "释放");

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
    ESP_LOGI(TAG, "外设自检启动：KEY=GPIO%d LIMIT1=GPIO%d LIMIT2=GPIO%d "
                  "TOUCH1=GPIO%d TOUCH2=GPIO%d "
                  "WS2812_A=GPIO%d(x%d) WS2812_B=GPIO%d(x%d)",
             BOARD_KEY_GPIO, BOARD_LIMIT_1_GPIO, BOARD_LIMIT_2_GPIO,
             BOARD_TOUCH_1_GPIO, BOARD_TOUCH_2_GPIO,
             BOARD_WS2812_A_GPIO, (int)BOARD_WS2812_A_LED_COUNT,
             BOARD_WS2812_B_GPIO, (int)BOARD_WS2812_B_LED_COUNT);
#if BOARD_HAS_STEPPER && (!PERIPH_LCD_DIAGNOSTIC_ONLY || CAMERA_TEST_PROFILE == CAMERA_TEST_STEPPER_ONLY)
    esp_err_t motor_err = stepper_motor_init();
    if (motor_err != ESP_OK) {
        ESP_LOGE(TAG, "步进电机驱动初始化失败：%s", esp_err_to_name(motor_err));
    } else if (xTaskCreatePinnedToCore(
                   stepper_test_task, "stepper_test", STEPPER_TEST_TASK_STACK,
                   NULL, STEPPER_TEST_TASK_PRIORITY, NULL,
                   STEPPER_TEST_CORE) != pdPASS) {
        ESP_LOGE(TAG, "创建步进电机自检任务失败");
        (void)stepper_motor_disable();
    }
#elif BOARD_HAS_STEPPER
    ESP_LOGW(TAG,
             "LCD 诊断模式：跳过步进电机 GPIO9~13/50 初始化，避免启动电流触发欠压");
#else
    ESP_LOGW(TAG, "当前板型 BOARD_HAS_STEPPER=0，跳过 GPIO9~13/50 电机初始化");
#endif
#if PERIPH_LCD_DIAGNOSTIC_ONLY
    ESP_LOGI(TAG, "LCD 诊断模式：屏幕持续显示 R/G/B/W/K 色条；"
                  "保留 BUTTON/TOUCH 日志，停用电机和两路 WS2812 灯效");
#else
    ESP_LOGI(TAG, "预期现象：屏幕显示 R/G/B/W/K 色条；电机小角度往返；"
                  "BUTTON/TOUCH 打印事件；两路 WS2812 循环灯效");
#endif
    /* 手头没有按键/触摸模块时，用一根杜邦线就能把输入通路整条验完：
     * 电平变化 → 去抖 → 事件 → 回调，全都走真实代码路径。 */
    ESP_LOGI(TAG, "输入验证：KEY接口(CN1 pin5/GPIO%d)接 GND 应打印 BUTTON pressed→click；"
             "LIMIT1(GPIO%d)、LIMIT2(GPIO%d) 闭合接 GND 应打印 TRIGGERED；"
                  "TOUCH1(GPIO%d) 短到 3V3 应打出 TOUCH: touch1 pressed；"
                  "TOUCH2(GPIO%d) 短到 3V3 同理（按当前极性）",
             BOARD_KEY_GPIO, BOARD_LIMIT_1_GPIO, BOARD_LIMIT_2_GPIO,
             BOARD_TOUCH_1_GPIO, BOARD_TOUCH_2_GPIO);
    ESP_LOGW(TAG, "外部电源按键 D3 只控制 PWR_IO 电源锁存，不连接 GPIO22；"
                  "按 D3 不会产生 BUTTON 日志");

    esp_err_t err = button_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "按键初始化失败：%s", esp_err_to_name(err));
        return err;
    }
    button_register_callback(button_event_handler, NULL);

    err = limit_inputs_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "限位输入初始化失败：%s", esp_err_to_name(err));
        return err;
    }

    err = touch_key_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "触摸初始化失败：%s", esp_err_to_name(err));
        return err;
    }
    touch_key_register_callback(touch_event_handler, NULL);

#if !PERIPH_LCD_DIAGNOSTIC_ONLY
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
#else
    ESP_LOGW(TAG, "LCD 诊断模式：跳过 WS2812 GPIO20/21 初始化和灯效任务");
#endif

    /* 静默期自检只是诊断，起不来不该影响灯效序列，所以只告警不返回错误。 */
    const BaseType_t quiet_created = xTaskCreate(quiet_check_task, "periph_quiet",
                                                 PERIPH_QUIET_TASK_STACK, NULL,
                                                 PERIPH_TEST_TASK_PRIORITY, NULL);
    if (quiet_created != pdPASS) {
        ESP_LOGW(TAG, "创建静默期自检任务失败（不影响灯效序列）");
    }

    ESP_LOGI(TAG, "档位 9 输入诊断就绪：GPIO22 按键及 GPIO0/53 限位会实时打日志，"
                  "%d ms 后打印一次静默期自检结论", PERIPH_QUIET_WINDOW_MS);
    return ESP_OK;
}
