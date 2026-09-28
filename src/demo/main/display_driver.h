#ifndef LUMMISS_DISPLAY_DRIVER_H
#define LUMMISS_DISPLAY_DRIVER_H

#include <stdbool.h>

/* 初始化 ST7789、LVGL 和动态时间天气首页。
 * 该函数只能由 UI Task 调用。 */
void display_driver_start(void);

/* 启动阶段、创建 UI 任务之前调用；NULL 表示不显示绑定页。 */
void display_driver_set_binding_code(const char *code);

/* 显示固定 RGB/黑白色条和四角标记，用于档位 9 验证颜色、方向和刷新链路。
 * 该函数只能在 display_driver_start() 成功后由 UI Task 调用。 */
void display_driver_show_test_pattern(void);

/* 档位 9 专用：让背光原始电平低/高各保持 1 秒并恢复正常点亮电平。
 * 用于区分“LCD 没刷新”和“新 PCB 背光极性/连线错误”。 */
void display_driver_run_backlight_test(void);

/* 输出 esp_lvgl_port 内部任务的剩余栈最低水位。 */
void display_driver_log_task_stack(void);

/* RTC 期间仅内容变更触发刷新，DMA不足延后，结束后恢复默认刷新。 */
void display_driver_set_rtc_preview(bool active);

#endif /* LUMMISS_DISPLAY_DRIVER_H */
