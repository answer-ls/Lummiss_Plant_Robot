#pragma once
#include "esp_err.h"
#include "driver/i2s_std.h"
#include "audio_codec_ctrl_if.h"

/* 独占 RX 的原始四槽诊断；调用方不得启动正常采集任务。 */
esp_err_t raw_adc_probe_run(const audio_codec_ctrl_if_t *ctrl, i2s_chan_handle_t rx, i2s_chan_handle_t tx);
