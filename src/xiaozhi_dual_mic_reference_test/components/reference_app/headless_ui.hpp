#pragma once
// 不链接LVGL，不创建刷新任务。
struct lv_obj_t {};
struct lv_timer_t {};
inline void lv_timer_pause(lv_timer_t *) {}
inline void lv_timer_resume(lv_timer_t *) {}
