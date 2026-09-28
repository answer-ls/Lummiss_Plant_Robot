#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 按键 / TTP223 触摸 / WS2812 氛围灯 / DRV8833 的临时自检入口（档位 9 使用）。
 *
 * 只做三件事：把三个驱动初始化起来、登记回调打印事件态、循环跑一遍固定的
 * 灯效序列。它不实现任何业务规则，也不引用机器人状态名
 * （HOME/LISTEN/THINK/REPLY/WARNING/FAULT/SLEEP）；状态映射留给后续阶段。
 *
 * 档位 9 输入验证：
 *   1. GPIO22 按下/释放 → 驱动日志 "BUTTON: pressed" / "BUTTON: released"
 *   2. GPIO0 限位闭合/断开 → "LIMIT1 ... TRIGGERED/RELEASED"
 *   3. GPIO53 限位闭合/断开 → "LIMIT2 ... TRIGGERED/RELEASED"
 *   4. GPIO52 触摸/释放 → 驱动日志 "TOUCH: touch1 pressed" / "...touch1 released"
 *   5. GPIO51 触摸/释放 → 驱动日志 "TOUCH: touch2 pressed" / "...touch2 released"
 * 灯带和电机测试代码仍保留，但当前 LCD/低负载诊断宏会跳过这些输出负载。
 * 屏幕测试由 UI Task 调用 display_driver_show_test_pattern()，不在本任务操作 LVGL。
 */
esp_err_t peripheral_test_start(void);

/* 档位12：启动单次电机往返任务，完成后关闭线圈。 */
esp_err_t stepper_test_start(void);

#ifdef __cplusplus
}
#endif
