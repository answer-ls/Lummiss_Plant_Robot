#pragma once
#include <stddef.h>
#include <stdint.h>
#include "audio_codec_ctrl_if.h"
#include "driver/i2s_std.h"
esp_err_t lifecycle_prepare(const audio_codec_ctrl_if_t *ctrl, i2s_chan_handle_t rx, i2s_chan_handle_t tx);
void lifecycle_start(void);
void lifecycle_mark(const char *name);
void lifecycle_raw(const void *data, size_t bytes);
