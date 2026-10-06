#pragma once

#include <stddef.h>
#include <stdint.h>

/* 首次续听时只缓存真正发送成功的 Opus 包；不改变编码和发送路径。 */
void opus_probe_start(void);
void opus_probe_packet(const uint8_t *packet, size_t bytes);
