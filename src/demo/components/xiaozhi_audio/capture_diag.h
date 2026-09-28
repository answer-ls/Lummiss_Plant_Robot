#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "driver/i2s_std.h"

typedef enum { CD_RESAMPLE, CD_FEED, CD_OPUS, CD_GUARD, CD_UDP, CD_METRICS } cd_metric_t;
esp_err_t capture_diag_init(void);
bool capture_diag_overflow(i2s_chan_handle_t channel, i2s_event_data_t *event, void *ctx);
void capture_diag_state(bool continuous_listening);
void capture_diag_begin(int64_t begin);
void capture_diag_read_end(int64_t end, int result, const void *raw, size_t bytes);
void capture_diag_metric(cd_metric_t metric, int64_t elapsed);
bool capture_diag_sink(size_t bytes);
void capture_diag_udp_result(int result, size_t bytes);

/* 本轮专用诊断：避免旧探针只在A侧复制/导出大量录音，污染单变量对照。 */
#define CAPTURE_UPLINK_AB_DIAGNOSTIC 1
