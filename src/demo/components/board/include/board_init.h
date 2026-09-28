#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 初始化板级启动控制 GPIO。应在其它外设初始化之前调用。 */
esp_err_t board_init_early(void);

#ifdef __cplusplus
}
#endif
