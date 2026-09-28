#pragma once

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

/* 统一连续内存快照；只在生命周期边界调用，不按音频/视频帧打印。
 * 多核仍可能并发分配，各项是相邻采样，不把总空闲误当作可分配连续块。 */
static inline void mem_contig_log(const char *tag)
{
    const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    ESP_LOGI("MEM_CONTIG", "MEM_CONTIG[%s] us=%lld INT=%u/%u DMA=%u/%u PSRAM=%u/%u ref_min=92224 ref_ok=%d target_96KiB=%d",
             tag, (long long)esp_timer_get_time(),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)largest,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM),
             largest >= 92224U, largest >= 96U * 1024U);
}

/* 首次启动至首次编码器创建的诊断窗口；不改变业务资源顺序。 */
void mem_fragment_begin(void);
void mem_fragment_dump_once(void);
