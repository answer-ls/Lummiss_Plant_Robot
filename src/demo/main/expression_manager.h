#ifndef LUMMISS_EXPRESSION_MANAGER_H
#define LUMMISS_EXPRESSION_MANAGER_H

#include <stdbool.h>

#include "esp_err.h"
#include "lvgl.h"

/* 初始化事件驱动表情管理器；必须在 LVGL 线程中调用。 */
esp_err_t expression_manager_init(void);
void expression_manager_post_state(const char *state);
void expression_manager_post_emotion(const char *emotion);

#endif
