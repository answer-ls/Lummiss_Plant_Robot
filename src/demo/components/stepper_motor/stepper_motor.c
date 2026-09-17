#include "stepper_motor.h"

#include "board_pins.h"

#if BOARD_HAS_STEPPER

#include "driver/gpio.h"
#include <stddef.h>

#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

/* 四列顺序严格对应 AIN1、AIN2、BIN2、BIN1。 */
static const gpio_num_t s_input_pins[] = {
    BOARD_STEPPER_AIN1,
    BOARD_STEPPER_AIN2,
    BOARD_STEPPER_BIN2,
    BOARD_STEPPER_BIN1,
};

static const uint8_t s_full_step_phase[4][4] = {
    {1, 0, 0, 1}, /* Phase0 */
    {0, 1, 0, 1}, /* Phase1 */
    {0, 1, 1, 0}, /* Phase2 */
    {1, 0, 1, 0}, /* Phase3 */
};

static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;
static SemaphoreHandle_t s_move_mutex;
static SemaphoreHandle_t s_step_event;
static esp_timer_handle_t s_step_timer;

static bool s_initialized;
static bool s_enabled;
static bool s_moving;
static bool s_stop_requested;
static bool s_fault_latched;
static bool s_phase_valid;
static uint8_t s_phase_index;

static esp_err_t set_all_inputs_low(void)
{
    esp_err_t first_error = ESP_OK;

    for (size_t i = 0; i < sizeof(s_input_pins) / sizeof(s_input_pins[0]); ++i) {
        esp_err_t err = gpio_set_level(s_input_pins[i], 0);
        if (first_error == ESP_OK && err != ESP_OK) {
            first_error = err;
        }
    }

    return first_error;
}

static esp_err_t disable_hardware(void)
{
    /* 先关断四个 H 桥输入，再让 DRV8833 进入休眠。 */
    esp_err_t err = set_all_inputs_low();
    esp_err_t sleep_err = gpio_set_level(BOARD_STEPPER_SLEEP, 0);
    if (err == ESP_OK) {
        err = sleep_err;
    }

    s_enabled = false;
    s_phase_valid = false;
    return err;
}

static esp_err_t write_phase(uint8_t phase)
{
    /* 先全部拉低，再按相序置高，避免切相时短暂叠加旧相位。 */
    esp_err_t err = set_all_inputs_low();
    if (err != ESP_OK) {
        return err;
    }

    for (size_t i = 0; i < sizeof(s_input_pins) / sizeof(s_input_pins[0]); ++i) {
        if (s_full_step_phase[phase][i] != 0) {
            err = gpio_set_level(s_input_pins[i], 1);
            if (err != ESP_OK) {
                return err;
            }
        }
    }

    return ESP_OK;
}

static bool fault_latched(void)
{
    bool latched;

    portENTER_CRITICAL(&s_state_lock);
    latched = s_fault_latched;
    portEXIT_CRITICAL(&s_state_lock);

    return latched;
}

static bool fault_active(void)
{
    return gpio_get_level(BOARD_STEPPER_FAULT) == 0 || fault_latched();
}

static bool stop_requested(void)
{
    bool requested;

    portENTER_CRITICAL(&s_state_lock);
    requested = s_stop_requested;
    portEXIT_CRITICAL(&s_state_lock);

    return requested;
}

static void step_timer_callback(void *arg)
{
    (void)arg;
    xSemaphoreGive(s_step_event);
}

static void fault_gpio_isr(void *arg)
{
    (void)arg;
    BaseType_t higher_priority_task_woken = pdFALSE;

    portENTER_CRITICAL_ISR(&s_state_lock);
    s_fault_latched = true;
    const bool moving = s_moving;
    portEXIT_CRITICAL_ISR(&s_state_lock);

    if (moving) {
        xSemaphoreGiveFromISR(s_step_event, &higher_priority_task_woken);
        if (higher_priority_task_woken == pdTRUE) {
            portYIELD_FROM_ISR();
        }
    }
}

static void clear_fault_latch(void)
{
    if (gpio_get_level(BOARD_STEPPER_FAULT) != 0) {
        portENTER_CRITICAL(&s_state_lock);
        s_fault_latched = false;
        portEXIT_CRITICAL(&s_state_lock);
    }
}

static void release_move_mutex(void)
{
    portENTER_CRITICAL(&s_state_lock);
    s_moving = false;
    s_stop_requested = false;
    portEXIT_CRITICAL(&s_state_lock);
    xSemaphoreGive(s_move_mutex);
}

esp_err_t stepper_motor_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    s_move_mutex = xSemaphoreCreateMutex();
    s_step_event = xSemaphoreCreateBinary();
    if (s_move_mutex == NULL || s_step_event == NULL) {
        goto no_memory;
    }

    const esp_timer_create_args_t timer_args = {
        .callback = step_timer_callback,
        .name = "stepper_step",
    };
    esp_err_t err = esp_timer_create(&timer_args, &s_step_timer);
    if (err != ESP_OK) {
        goto init_failed;
    }

    const uint64_t output_mask =
        (1ULL << BOARD_STEPPER_AIN1) |
        (1ULL << BOARD_STEPPER_AIN2) |
        (1ULL << BOARD_STEPPER_BIN2) |
        (1ULL << BOARD_STEPPER_BIN1) |
        (1ULL << BOARD_STEPPER_SLEEP);
    const gpio_config_t output_config = {
        .pin_bit_mask = output_mask,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    err = gpio_config(&output_config);
    if (err != ESP_OK) {
        goto init_failed;
    }

    err = disable_hardware();
    if (err != ESP_OK) {
        goto init_failed;
    }

    const gpio_config_t fault_config = {
        .pin_bit_mask = 1ULL << BOARD_STEPPER_FAULT,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    err = gpio_config(&fault_config);
    if (err != ESP_OK) {
        goto init_failed;
    }

    err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        goto init_failed;
    }

    err = gpio_isr_handler_add(BOARD_STEPPER_FAULT, fault_gpio_isr, NULL);
    if (err != ESP_OK) {
        goto init_failed;
    }

    s_initialized = true;
    return ESP_OK;

no_memory:
    err = ESP_ERR_NO_MEM;
init_failed:
    if (s_step_timer != NULL) {
        esp_timer_delete(s_step_timer);
        s_step_timer = NULL;
    }
    if (s_step_event != NULL) {
        vSemaphoreDelete(s_step_event);
        s_step_event = NULL;
    }
    if (s_move_mutex != NULL) {
        vSemaphoreDelete(s_move_mutex);
        s_move_mutex = NULL;
    }
    return err;
}

esp_err_t stepper_motor_enable(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_move_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_FAIL;
    }

    if (gpio_get_level(BOARD_STEPPER_FAULT) == 0) {
        /* 故障已存在时也要确保桥臂输入和 nSLEEP 都处于关闭状态。 */
        disable_hardware();
        xSemaphoreGive(s_move_mutex);
        return ESP_FAIL;
    }
    clear_fault_latch();

    esp_err_t err = set_all_inputs_low();
    if (err == ESP_OK) {
        err = gpio_set_level(BOARD_STEPPER_SLEEP, 1);
    }
    if (err == ESP_OK) {
        /* nSLEEP 拉高后留出芯片唤醒时间，再允许后续步进。 */
        esp_rom_delay_us(2000);
        if (fault_active()) {
            err = ESP_FAIL;
        }
    }

    if (err == ESP_OK) {
        s_enabled = true;
        s_phase_valid = false;
    } else {
        disable_hardware();
    }

    xSemaphoreGive(s_move_mutex);
    return err;
}

esp_err_t stepper_motor_disable(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    stepper_motor_stop();
    if (xSemaphoreTake(s_move_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_FAIL;
    }

    esp_err_t err = disable_hardware();
    xSemaphoreGive(s_move_mutex);
    return err;
}

esp_err_t stepper_motor_move_steps(
    uint32_t steps,
    stepper_direction_t direction,
    uint32_t step_interval_us)
{
    if (steps == 0) {
        return ESP_OK;
    }
    if ((direction != STEPPER_DIR_CW && direction != STEPPER_DIR_CCW) ||
        step_interval_us == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_move_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_FAIL;
    }
    if (!s_enabled) {
        xSemaphoreGive(s_move_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    clear_fault_latch();
    portENTER_CRITICAL(&s_state_lock);
    s_moving = true;
    s_stop_requested = false;
    portEXIT_CRITICAL(&s_state_lock);

    esp_err_t result = ESP_OK;
    for (uint32_t step = 0; step < steps; ++step) {
        while (xSemaphoreTake(s_step_event, 0) == pdTRUE) {
            /* 清掉上一轮计时器或故障留下的唤醒信号。 */
        }

        if (fault_active()) {
            result = ESP_FAIL;
            disable_hardware();
            break;
        }
        if (stop_requested()) {
            result = ESP_ERR_INVALID_STATE;
            break;
        }

        if (!s_phase_valid) {
            s_phase_index = direction == STEPPER_DIR_CW ? 0 : 3;
            s_phase_valid = true;
        } else if (direction == STEPPER_DIR_CW) {
            s_phase_index = (s_phase_index + 1) & 0x03;
        } else {
            s_phase_index = (s_phase_index + 3) & 0x03;
        }

        result = write_phase(s_phase_index);
        if (result != ESP_OK) {
            disable_hardware();
            break;
        }

        result = esp_timer_start_once(s_step_timer, step_interval_us);
        if (result != ESP_OK) {
            disable_hardware();
            break;
        }

        if (xSemaphoreTake(s_step_event, portMAX_DELAY) != pdTRUE) {
            result = ESP_FAIL;
            disable_hardware();
            break;
        }
        /* 外部 stop 或 nFAULT 会提前唤醒；计时器到期则已自动结束。 */
        esp_timer_stop(s_step_timer);

        if (fault_active()) {
            result = ESP_FAIL;
            disable_hardware();
            break;
        }
        if (stop_requested()) {
            result = ESP_ERR_INVALID_STATE;
            break;
        }
    }

    release_move_mutex();
    return result;
}

void stepper_motor_stop(void)
{
    bool moving;

    portENTER_CRITICAL(&s_state_lock);
    moving = s_moving;
    if (moving) {
        s_stop_requested = true;
    }
    portEXIT_CRITICAL(&s_state_lock);

    if (moving && s_step_event != NULL) {
        xSemaphoreGive(s_step_event);
    }
}

void stepper_motor_hold(void)
{
    if (!s_initialized) {
        return;
    }

    stepper_motor_stop();
    if (xSemaphoreTake(s_move_mutex, portMAX_DELAY) == pdTRUE) {
        /* 保持当前相位通电，继续提供静态保持力矩。 */
        xSemaphoreGive(s_move_mutex);
    }
}

void stepper_motor_release(void)
{
    if (!s_initialized) {
        return;
    }

    stepper_motor_stop();
    if (xSemaphoreTake(s_move_mutex, portMAX_DELAY) == pdTRUE) {
        /* 输入全低使线圈释放，但芯片仍保持唤醒，便于后续快速动作。 */
        if (set_all_inputs_low() == ESP_OK) {
            s_phase_valid = false;
        }
        xSemaphoreGive(s_move_mutex);
    }
}

bool stepper_motor_is_fault(void)
{
    return s_initialized && fault_active();
}

#else

/* DEV_BOARD 没有 DRV8833：空实现不申请、不配置、也不访问电机 GPIO。 */
esp_err_t stepper_motor_init(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t stepper_motor_enable(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t stepper_motor_disable(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t stepper_motor_move_steps(
    uint32_t steps,
    stepper_direction_t direction,
    uint32_t step_interval_us)
{
    (void)steps;
    (void)direction;
    (void)step_interval_us;
    return ESP_ERR_NOT_SUPPORTED;
}

void stepper_motor_stop(void) {}
void stepper_motor_hold(void) {}
void stepper_motor_release(void) {}
bool stepper_motor_is_fault(void) { return false; }

#endif
