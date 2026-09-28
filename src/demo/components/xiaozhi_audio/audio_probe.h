#pragma once
#include <stddef.h>
#include <stdint.h>
/* 回答结束后单次诊断：缓存原始四槽、AFE输出和Opus实际输入。 */
void audio_probe_start(void);
void audio_probe_raw(const int16_t *data, size_t bytes);
void audio_probe_afe(const int16_t *data, size_t bytes);

void audio_probe_uplink(const int16_t *data, size_t bytes);

/* 同一批raw与重采样MMR绑定保存，frames为每通道16k样本数。 */
void audio_probe_input(const int16_t *raw, size_t raw_bytes, const int16_t *mmr, size_t frames);
