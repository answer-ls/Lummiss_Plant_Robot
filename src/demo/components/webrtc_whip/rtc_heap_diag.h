#pragma once

#include "esp_err.h"

/* 只追踪 esp_peer_open 短窗口；trace 数据存放 PSRAM。 */
esp_err_t rtc_heap_diag_init(void);
void rtc_heap_diag_peer_begin(void);
void rtc_heap_diag_peer_end(void);
void rtc_heap_diag_report_first_failure(void);
