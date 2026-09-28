#ifndef LUMMISS_EXPRESSION_MANAGER_H
#define LUMMISS_EXPRESSION_MANAGER_H

#include <stdbool.h>

#include "esp_err.h"
#include "lvgl.h"

/* 初始化事件驱动表情管理器；必须在 LVGL 线程中调用。 */
esp_err_t expression_manager_init(void);
void expression_manager_post_state(const char *state);
void expression_manager_post_emotion(const char *emotion);

/* WHIP 建链前切回 HOME，并等待动画临时 PSRAM Buffer 安全释放。 */
esp_err_t expression_manager_prepare_for_webrtc(uint32_t timeout_ms);

/* RTC 结束后解除动画门控；建链时由 prepare_for_webrtc 设置。 */
void expression_manager_set_rtc_active(bool active);

#endif
