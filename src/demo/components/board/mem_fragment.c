#include "mem_contig.h"
#include "esp_heap_trace.h"
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* 普通运行可关闭两个开关；定向模式只聚合目标region的owner，
 * 全量模式仅用于明确要求的布局采集，避免串口刷屏阻塞RTC创建。 */
#if CONFIG_H264_FRAGMENT_VERBOSE || CONFIG_H264_FRAGMENT_FOCUSED

/* 只诊断一次；数据在PSRAM，不挤占待测INTERNAL区域。
 * PSRAM trace无法覆盖ISR，启动前已有块也没有caller，分析时必须保留未知。 */
#define BLOCK_MAX 4096
#define TRACE_MAX 4096
typedef struct {
    uintptr_t start, end, ptr;
    size_t size;
    bool used;
    void *caller[CONFIG_HEAP_TRACING_STACK_DEPTH];
    bool reported;
} block_t;
static block_t *s_blocks;
static size_t s_count, s_dropped;
static uintptr_t s_target_start;
static size_t s_api_largest;
static bool s_done;
#if CONFIG_HEAP_TRACING_STANDALONE
static heap_trace_record_t *s_trace;
static bool s_tracing;
#endif

void mem_fragment_begin(void)
{
    s_blocks = heap_caps_calloc(BLOCK_MAX, sizeof(*s_blocks), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#if CONFIG_HEAP_TRACING_STANDALONE
    s_trace = heap_caps_calloc(TRACE_MAX, sizeof(*s_trace), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_trace && heap_trace_init_standalone(s_trace, TRACE_MAX) == ESP_OK) {
        s_tracing = heap_trace_start(HEAP_TRACE_LEAKS) == ESP_OK;
        /* 启动失败时先解除分配器对缓冲的引用，避免后续释放后悬空。 */
        if (!s_tracing) heap_trace_init_standalone(NULL, 0);
    }
    ESP_LOGI("FRAG", "TRACE_BEGIN active=%d capacity=%d depth=%d ISR=not_recorded pre_app=unknown",
             s_tracing, TRACE_MAX, CONFIG_HEAP_TRACING_STACK_DEPTH);
#else
    ESP_LOGW("FRAG", "TRACE disabled; owners unknown");
#endif
}

static bool collect_block(walker_heap_into_t heap, walker_block_info_t b, void *ctx)
{
    (void)ctx;
    /* 持heap锁期间不打印、不分配、不调用其他heap API。保存全部INTERNAL区域，
     * 避免重新链接导致边界偏移后按旧地址过滤遗漏。 */
    if (s_count == BLOCK_MAX) { ++s_dropped; return true; }
    s_blocks[s_count++] = (block_t){.start=heap.start, .end=heap.end,
        .ptr=(uintptr_t)b.ptr, .size=b.size, .used=b.used};
    return true;
}

static bool target_block(const block_t *b)
{
    /* reserve是父heap中的嵌套region，不能用地址包含判断而混入父heap。 */
    return s_target_start && b->start == s_target_start;
}

static void caller_text(const block_t *b, char *out, size_t capacity)
{
    size_t used = 0;
    out[0] = 0;
    for (size_t i = 0; i < CONFIG_HEAP_TRACING_STACK_DEPTH; ++i) {
        if (!b->caller[i]) continue;
        int n = snprintf(out + used, capacity - used, "%s%08x", used ? "," : "",
                         (unsigned)(uintptr_t)b->caller[i]);
        if (n < 0 || (size_t)n >= capacity - used) break;
        used += (size_t)n;
    }
    /* 不加0x前缀，防止IDF监视器把每个PC再展开成多行；PC端用匹配ELF解码。 */
}

static void backward_chain(size_t best)
{
    /* 复用同一份heap快照；只追溯最大块左侧，直到前一个FREE或region边界。 */
    size_t first = best;
    while (first && target_block(&s_blocks[first - 1]) && s_blocks[first - 1].used) --first;
    bool has_left_free = first && target_block(&s_blocks[first - 1]) && !s_blocks[first - 1].used;
    size_t begin = has_left_free ? first - 1 : first;
    uintptr_t end = s_blocks[best].ptr + s_blocks[best].size;
    ESP_LOGI("FRAG", "BACKWARD_CHAIN_BEGIN allocations=%u left_free=%d complete=%d",
             (unsigned)(best - first), has_left_free, s_dropped == 0);
    for (size_t i = begin; i <= best; ++i) {
        const block_t *b = &s_blocks[i];
        /* 地址差包含块间metadata和对齐空间，不以payload求和代替物理跨度。
         * 最后FREE使用payload末端，保留region尾部哨兵。 */
        uintptr_t next = i < best ? s_blocks[i + 1].ptr : end;
        char pcs[CONFIG_HEAP_TRACING_STACK_DEPTH * 9 + 1];
        caller_text(b, pcs, sizeof(pcs));
        ESP_LOGI("FRAG", "BACKWARD_BLOCK index=%u state=%s address=%08x payload_size=%u block_span=%u pcs=%s",
                 (unsigned)(i - begin), b->used ? "ALLOC" : "FREE", (unsigned)b->ptr,
                 (unsigned)b->size, (unsigned)(next - b->ptr),
                 b->used ? (pcs[0] ? pcs : "unknown") : "none");
    }
    size_t first_min = 0, first_target = 0;
    for (size_t removed = 1; removed <= best - first; ++removed) {
        size_t i = best - removed;
        /* 删除整条链后，前一FREE也会合并；删除部分链时不能跨过仍存活的块。 */
        uintptr_t start = i == first && has_left_free ? s_blocks[first - 1].ptr : s_blocks[i].ptr;
        size_t merged = end - start;
        if (!first_min && merged >= 92224) first_min = removed;
        if (!first_target && merged >= 98304) first_target = removed;
        ESP_LOGI("FRAG", "BACKWARD_RECLAIM blocks_removed=%u range_start=%08x range_end=%08x reclaimed_span=%u new_contiguous_size=%u meet_92224=%d meet_98304=%d",
                 (unsigned)removed, (unsigned)start, (unsigned)end,
                 (unsigned)(s_blocks[best].ptr - start), (unsigned)merged,
                 merged >= 92224, merged >= 98304);
    }
    /* 阈值只说明物理连续跨度；TLSF档位、申请对齐与后续并发分配仍需实际验证。 */
    ESP_LOGI("FRAG", "BACKWARD_CHAIN_END min_blocks_92224=%u min_blocks_98304=%u zero=not_reached physical_span_only=1",
             (unsigned)first_min, (unsigned)first_target);
}

static void largest_neighbors(void)
{
    size_t best = SIZE_MAX;
    for (size_t i = 0; i < s_count; ++i) {
        if (target_block(&s_blocks[i]) && !s_blocks[i].used &&
            (best == SIZE_MAX || s_blocks[i].size > s_blocks[best].size)) best = i;
    }
    if (best == SIZE_MAX) {
        ESP_LOGW("FRAG", "LARGEST unavailable");
        return;
    }
    const block_t *largest = &s_blocks[best];
    backward_chain(best);
    /* walker是原始payload，heap API还按TLSF可匹配大小取整，不能硬写77824。
     * API在遍历前采样；其他任务并发分配时两者也可能有瞬态差异。 */
    ESP_LOGI("FRAG", "LARGEST start=%08x raw_size=%u end_exclusive=%08x api_internal_largest=%u",
             (unsigned)largest->ptr, (unsigned)largest->size,
             (unsigned)(largest->ptr + largest->size), (unsigned)s_api_largest);
    for (int offset = -3; offset <= 3; ++offset) {
        ptrdiff_t index = (ptrdiff_t)best + offset;
        if (index < 0 || (size_t)index >= s_count || !target_block(&s_blocks[index])) {
            ESP_LOGI("FRAG", "NEIGHBOR offset=%d state=REGION_BOUNDARY", offset);
            continue;
        }
        const block_t *b = &s_blocks[index];
        char pcs[CONFIG_HEAP_TRACING_STACK_DEPTH * 9 + 1];
        caller_text(b, pcs, sizeof(pcs));
        ESP_LOGI("FRAG", "NEIGHBOR offset=%d state=%s ptr=%08x size=%u pcs=%s",
                 offset, b->used ? "ALLOC" : "FREE", (unsigned)b->ptr, (unsigned)b->size,
                 b->used ? (pcs[0] ? pcs : "unknown") : "none");
        if ((offset == -1 || offset == 1) && b->used) {
            size_t i = (size_t)index;
            size_t left = i && target_block(&s_blocks[i-1]) && !s_blocks[i-1].used ? s_blocks[i-1].size : 0;
            size_t right = i+1 < s_count && target_block(&s_blocks[i+1]) && !s_blocks[i+1].used ? s_blocks[i+1].size : 0;
            size_t merged = left + b->size + right;
            ESP_LOGI("FRAG", "CANDIDATE side=%s ptr=%08x size=%u left_free=%u right_free=%u merged_payload=%u meet_92224=%d meet_98304=%d",
                     offset < 0 ? "LEFT" : "RIGHT", (unsigned)b->ptr, (unsigned)b->size,
                     (unsigned)left, (unsigned)right, (unsigned)merged, merged >= 92224, merged >= 98304);
        }
    }
}

static void focused_report(void)
{
    for (size_t i = 0; i < s_count; ++i) {
        const block_t *b = &s_blocks[i];
        if (b->start == 0x4ff861c0U) { s_target_start = b->start; break; }
        /* 当前200000字节独立reserve；链接起点变化时按实际region长度选择。
         * 若reserve拆成多区而无法匹配，宁可报未找到，不猜其他heap。 */
        if (b->end - b->start == 199999U) s_target_start = b->start;
    }
    ESP_LOGI("FRAG", "FOCUSED_REGION start=%08x found=%d", (unsigned)s_target_start, s_target_start != 0);
    largest_neighbors();
    size_t live = 0, bytes = 0, unknown = 0, boundaries = 0;
    for (size_t i = 0; i < s_count; ++i) {
        const block_t *b = &s_blocks[i];
        if (!target_block(b)) continue;
        if (!b->used) continue;
        ++live; bytes += b->size;
        if (!b->caller[0]) unknown += b->size;
        size_t left = i && s_blocks[i-1].start == b->start && !s_blocks[i-1].used ? s_blocks[i-1].size : 0;
        size_t right = i+1 < s_count && s_blocks[i+1].start == b->start && !s_blocks[i+1].used ? s_blocks[i+1].size : 0;
        if (!(left || right)) continue;
        ++boundaries;
        char pcs[CONFIG_HEAP_TRACING_STACK_DEPTH * 9 + 1];
        caller_text(b, pcs, sizeof(pcs));
        ESP_LOGI("FRAG", "EDGE ptr=%08x size=%u left=%u right=%u merged=%u pcs=%s",
                 (unsigned)b->ptr, (unsigned)b->size, (unsigned)left, (unsigned)right,
                 (unsigned)(left+b->size+right), pcs[0] ? pcs : "unknown");
    }
    /* 同一调用栈聚合；只输出占用最多的12组，不按所有对象逐个刷屏。 */
    for (size_t rank = 0; rank < 12; ++rank) {
        size_t best = SIZE_MAX, best_bytes = 0, best_count = 0;
        for (size_t i = 0; i < s_count; ++i) {
            block_t *b = &s_blocks[i];
            if (!target_block(b) || !b->used || b->reported) continue;
            size_t sum = 0, count = 0;
            for (size_t j = i; j < s_count; ++j) {
                block_t *other = &s_blocks[j];
                if (other->used && target_block(other) && !other->reported &&
                    memcmp(b->caller, other->caller, sizeof(b->caller)) == 0) {
                    sum += other->size; ++count;
                }
            }
            if (sum > best_bytes) { best = i; best_bytes = sum; best_count = count; }
        }
        if (best == SIZE_MAX) break;
        block_t *b = &s_blocks[best];
        char pcs[CONFIG_HEAP_TRACING_STACK_DEPTH * 9 + 1];
        caller_text(b, pcs, sizeof(pcs));
        ESP_LOGI("FRAG", "GROUP rank=%u bytes=%u count=%u example=%08x pcs=%s",
                 (unsigned)(rank+1), (unsigned)best_bytes, (unsigned)best_count,
                 (unsigned)b->ptr, pcs[0] ? pcs : "unknown");
        for (size_t j = 0; j < s_count; ++j) {
            block_t *other = &s_blocks[j];
            if (other->used && target_block(other) && memcmp(b->caller, other->caller, sizeof(b->caller)) == 0) other->reported = true;
        }
    }
    ESP_LOGI("FRAG", "FOCUSED_DONE live=%u bytes=%u unknown_bytes=%u edges=%u edge_limit=none group_limit=12",
             (unsigned)live, (unsigned)bytes, (unsigned)unknown, (unsigned)boundaries);
}

void mem_fragment_dump_once(void)
{
    if (s_done) return;
    s_done = true;
#if CONFIG_HEAP_TRACING_STANDALONE
    if (s_tracing) heap_trace_stop();
#endif
    s_api_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    if (s_blocks) heap_caps_walk(MALLOC_CAP_INTERNAL, collect_block, NULL);
    ESP_LOGI("FRAG", "LAYOUT_BEGIN blocks=%u dropped=%u available=%d", (unsigned)s_count, (unsigned)s_dropped, s_blocks != NULL);
#if CONFIG_H264_FRAGMENT_VERBOSE
    for (size_t i = 0; i < s_count; ++i) {
        const block_t *b = &s_blocks[i];
        ESP_LOGI("FRAG", "BLOCK heap=0x%08x end=0x%08x ptr=0x%08x size=%u used=%d",
                 (unsigned)b->start, (unsigned)b->end, (unsigned)b->ptr, (unsigned)b->size, b->used);
    }
#endif
#if CONFIG_HEAP_TRACING_STANDALONE
    if (s_tracing) {
        heap_trace_summary_t summary = {0};
        heap_trace_summary(&summary);
        ESP_LOGI("FRAG", "TRACE_END live=%u overflow=%u", (unsigned)summary.count, (unsigned)summary.has_overflowed);
        for (size_t i = 0; i < heap_trace_get_count(); ++i) {
            heap_trace_record_t rec = {0};
            if (heap_trace_get(i, &rec) != ESP_OK || rec.freed || !rec.address) continue;
            /* 只输出本次布局中仍存活的块；对齐分配的用户指针可位于block内部。 */
            for (size_t j = 0; j < s_count; ++j) {
                const block_t *b = &s_blocks[j];
                uintptr_t p = (uintptr_t)rec.address;
                if (!b->used || p < b->ptr || p >= b->ptr + b->size) continue;
                memcpy(s_blocks[j].caller, rec.alloced_by, sizeof(s_blocks[j].caller));
#if CONFIG_H264_FRAGMENT_VERBOSE
                ESP_LOGI("FRAG", "OWNER block=0x%08x ptr=%p requested=%u", (unsigned)b->ptr, rec.address, (unsigned)rec.size);
                for (size_t k = 0; k < CONFIG_HEAP_TRACING_STACK_DEPTH; ++k) {
                    if (rec.alloced_by[k]) ESP_LOGI("FRAG", "CALLER block=0x%08x depth=%u pc=%p", (unsigned)b->ptr, (unsigned)k, rec.alloced_by[k]);
                }
#endif
                break;
            }
        }
        heap_trace_init_standalone(NULL, 0);
    }
    heap_caps_free(s_trace);
    s_trace = NULL;
    s_tracing = false;
#endif
    focused_report();
    ESP_LOGI("FRAG", "LAYOUT_END snapshot_not_atomic_across_heaps=1");
    heap_caps_free(s_blocks);
    s_blocks = NULL;
}

void mem_fragment_dump_retry_neighbors(void)
{
    /* 二次预览失败时仅重采样最大空闲块及其邻居，不重新开启长时heap trace。 */
    if (!s_done) return;
    s_blocks = heap_caps_calloc(BLOCK_MAX, sizeof(*s_blocks), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_blocks == NULL) return;
    s_count = s_dropped = 0;
    s_target_start = 0;
    s_api_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    heap_caps_walk(MALLOC_CAP_INTERNAL, collect_block, NULL);
    for (size_t i = 0; i < s_count; ++i) {
        if (s_blocks[i].end - s_blocks[i].start == 199999U) {
            s_target_start = s_blocks[i].start;
            break;
        }
    }
    size_t best = SIZE_MAX;
    for (size_t i = 0; i < s_count; ++i) {
        if (target_block(&s_blocks[i]) && !s_blocks[i].used &&
            (best == SIZE_MAX || s_blocks[i].size > s_blocks[best].size)) best = i;
    }
    ESP_LOGW("FRAG", "RETRY_LARGEST region=%08x api=%u raw=%u dropped=%u",
             (unsigned)s_target_start, (unsigned)s_api_largest,
             best == SIZE_MAX ? 0U : (unsigned)s_blocks[best].size, (unsigned)s_dropped);
    if (best != SIZE_MAX) {
        for (int offset = -3; offset <= 3; ++offset) {
            const ptrdiff_t index = (ptrdiff_t)best + offset;
            if (index < 0 || (size_t)index >= s_count || !target_block(&s_blocks[index])) continue;
            const block_t *b = &s_blocks[index];
            ESP_LOGW("FRAG", "RETRY_NEIGHBOR offset=%d state=%s ptr=%08x size=%u",
                     offset, b->used ? "ALLOC" : "FREE", (unsigned)b->ptr,
                     (unsigned)b->size);
        }
    }
    heap_caps_free(s_blocks);
    s_blocks = NULL;
}
#else
void mem_fragment_begin(void) {}
void mem_fragment_dump_once(void) {}
void mem_fragment_dump_retry_neighbors(void) {}
#endif
