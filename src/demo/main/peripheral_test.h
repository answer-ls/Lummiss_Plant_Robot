#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 按键 / TTP223 触摸 / WS2812 氛围灯的临时自检入口（档位 9 使用）。
 *
 * 只做三件事：把三个驱动初始化起来、登记回调打印事件态、循环跑一遍固定的
 * 灯效序列。它不实现任何业务规则，也不引用机器人状态名
 * （HOME/LISTEN/THINK/REPLY/WARNING/FAULT/SLEEP）；状态映射留给后续阶段。
 *
 * 对应本次要验证的 7 项：
 *   1. GPIO22 按下/释放 → 驱动日志 "BUTTON: pressed" / "BUTTON: released"
 *   2. GPIO52 触摸/释放 → 驱动日志 "TOUCH: touch1 pressed" / "...touch1 released"
 *   3. GPIO51 触摸/释放 → 驱动日志 "TOUCH: touch2 pressed" / "...touch2 released"
 *   4. WS2812_A 红绿蓝  → 序列前几步
 *   5. WS2812_B 红绿蓝  → 序列随后几步
 *   6. 两路同时工作     → 序列中的 "A+B" 几步
 *   7. 呼吸效果         → 序列中的 "A+B 呼吸" 一步
 */
esp_err_t peripheral_test_start(void);

#ifdef __cplusplus
}
#endif
