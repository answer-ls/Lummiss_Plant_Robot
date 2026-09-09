#ifndef LUMMISS_DISPLAY_DRIVER_H
#define LUMMISS_DISPLAY_DRIVER_H

/* 初始化 ST7789、LVGL 和当前表情动画播放器。
 * 该函数只能由 UI Task 调用。 */
void display_driver_start(void);

#endif /* LUMMISS_DISPLAY_DRIVER_H */
