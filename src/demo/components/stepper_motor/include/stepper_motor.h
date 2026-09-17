#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    STEPPER_DIR_CW = 0,
    STEPPER_DIR_CCW,
} stepper_direction_t;

/* DEV_BOARD 未安装 DRV8833：初始化及控制 API 返回 ESP_ERR_NOT_SUPPORTED。 */
esp_err_t stepper_motor_init(void);
esp_err_t stepper_motor_enable(void);
esp_err_t stepper_motor_disable(void);

/* 阻塞执行指定步数；间隔单位为微秒。主动 stop 返回 ESP_ERR_INVALID_STATE，nFAULT 返回 ESP_FAIL。 */
esp_err_t stepper_motor_move_steps(
    uint32_t steps,
    stepper_direction_t direction,
    uint32_t step_interval_us);

/* 这些控制接口用于任务上下文；hold 保持当前相位，release 断开线圈电流。 */
void stepper_motor_stop(void);
void stepper_motor_hold(void);
void stepper_motor_release(void);
bool stepper_motor_is_fault(void);

#ifdef __cplusplus
}
#endif
