#include "rtc_heap_diag.h"

#include <stdint.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_heap_trace.h"
#include "esp_log.h"
#include "esp_memory_utils.h"

static const char *TAG = "RTC_HEAP";

typedef struct {
    void *ptr;
    uint32_t caps;
} cap_record_t;

/* Hook 不分配内存、不打印日志；只在 peer-open 窗口写静态数组。 */
#define CAP_RECORD_MAX 512U
static cap_record_t *s_caps;
static volatile uint32_t s_cap_count;
static volatile uint32_t s_cap_overflow;
static volatile bool s_hook_active;

#if CONFIG_HEAP_USE_HOOKS
void esp_heap_trace_alloc_hook(void *ptr, size_t size, uint32_t caps)
{
    (void)size;
    if (!s_hook_active || ptr == NULL || s_caps == NULL) return;
    const uint32_t index = __atomic_fetch_add(&s_cap_count, 1U, __ATOMIC_RELAXED);
    if (index < CAP_RECORD_MAX) {
        s_caps[index].ptr = ptr;
        s_caps[index].caps = caps;
    } else {
        __atomic_add_fetch(&s_cap_overflow, 1U, __ATOMIC_RELAXED);
    }
}

void esp_heap_trace_free_hook(void *ptr)
{
    (void)ptr;
}
#endif

typedef struct {
    size_t size;
    uint32_t caps;
    const char *function_name;
    size_t dma_free, dma_largest, int_free, int_largest, psram_free, psram_largest;
} first_failure_t;

static first_failure_t s_first_failure;
static volatile bool s_failure_captured;
static volatile bool s_failure_reported;
static volatile bool s_failure_ready;

static void allocation_failed(size_t size, uint32_t caps, const char *function_name)
{
    /* 只锁定第一次 DMA 申请失败；普通 PSRAM 可选分配失败不能抢占诊断位。 */
    if ((caps & MALLOC_CAP_DMA) == 0) return;
    /* 首次失败当场采样；延后在 RTC 控制任务打印，避免失败回调里产生日志分配。 */
    if (__atomic_exchange_n(&s_failure_captured, true, __ATOMIC_ACQ_REL)) return;
    s_first_failure.size = size;
    s_first_failure.caps = caps;
    s_first_failure.function_name = function_name;
    s_first_failure.dma_free = heap_caps_get_free_size(MALLOC_CAP_DMA);
    s_first_failure.dma_largest = heap_caps_get_largest_free_block(MALLOC_CAP_DMA);
    s_first_failure.int_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    s_first_failure.int_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    s_first_failure.psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    s_first_failure.psram_largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    __atomic_store_n(&s_failure_ready, true, __ATOMIC_RELEASE);
}

esp_err_t rtc_heap_diag_init(void)
{
    /* Hook 可在关缓存路径触发，不能放 PSRAM；RTCRAM 不占 L2 DMA 堆。
     * 不回退到 DMA-capable INTERNAL，诊断缓冲失败则禁用 cap 对照表。 */
    s_caps = heap_caps_calloc(CAP_RECORD_MAX, sizeof(*s_caps), MALLOC_CAP_RTCRAM | MALLOC_CAP_8BIT);
    ESP_LOGI(TAG, "HOOK_CAP_TABLE bytes=%u ptr=%p region=RTCRAM available=%d",
             (unsigned)(CAP_RECORD_MAX * sizeof(*s_caps)), s_caps, s_caps != NULL);
    return heap_caps_register_failed_alloc_callback(allocation_failed);
}

void rtc_heap_diag_report_first_failure(void)
{
    if (!__atomic_load_n(&s_failure_ready, __ATOMIC_ACQUIRE) ||
        __atomic_exchange_n(&s_failure_reported, true, __ATOMIC_ACQ_REL)) return;
    ESP_LOGE(TAG, "FIRST_ALLOC_FAIL size=%u caps=0x%08x function=%s DMA=%u/%u INT=%u/%u PSRAM=%u/%u",
             (unsigned)s_first_failure.size, (unsigned)s_first_failure.caps,
             s_first_failure.function_name ? s_first_failure.function_name : "?",
             (unsigned)s_first_failure.dma_free, (unsigned)s_first_failure.dma_largest,
             (unsigned)s_first_failure.int_free, (unsigned)s_first_failure.int_largest,
             (unsigned)s_first_failure.psram_free, (unsigned)s_first_failure.psram_largest);
}

#if CONFIG_HEAP_TRACING_STANDALONE
#define PEER_TRACE_RECORD_MAX 512U
static heap_trace_record_t *s_records;
static bool s_trace_started;

void rtc_heap_diag_peer_begin(void)
{
    s_records = heap_caps_calloc(PEER_TRACE_RECORD_MAX, sizeof(*s_records),
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_records == NULL) {
        ESP_LOGE(TAG, "PEER_TRACE buffer allocation failed");
        return;
    }
    const esp_err_t init = heap_trace_init_standalone(s_records, PEER_TRACE_RECORD_MAX);
    if (init != ESP_OK) {
        ESP_LOGE(TAG, "PEER_TRACE init=%s", esp_err_to_name(init));
        heap_caps_free(s_records);
        s_records = NULL;
        return;
    }
    __atomic_store_n(&s_cap_count, 0U, __ATOMIC_RELAXED);
    __atomic_store_n(&s_cap_overflow, 0U, __ATOMIC_RELAXED);
    const esp_err_t start = heap_trace_start(HEAP_TRACE_LEAKS);
    if (start != ESP_OK) {
        ESP_LOGE(TAG, "PEER_TRACE start=%s", esp_err_to_name(start));
        heap_trace_init_standalone(NULL, 0);
        heap_caps_free(s_records);
        s_records = NULL;
        return;
    }
    s_trace_started = true;
    __atomic_store_n(&s_hook_active, true, __ATOMIC_RELEASE);
}

void rtc_heap_diag_peer_end(void)
{
    if (!s_trace_started) return;
    __atomic_store_n(&s_hook_active, false, __ATOMIC_RELEASE);
    heap_trace_stop();
    s_trace_started = false;
    /* 在输出多条 trace 记录前先取 heap 快照，保持 peer_open 净消耗可比。 */
    const size_t dma_free = heap_caps_get_free_size(MALLOC_CAP_DMA);
    const size_t dma_largest = heap_caps_get_largest_free_block(MALLOC_CAP_DMA);
    const size_t int_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const size_t int_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    ESP_LOGI(TAG, "MEM[PEER_OPEN_RETURN] DMA=%u/%u INT=%u/%u",
             (unsigned)dma_free, (unsigned)dma_largest,
             (unsigned)int_free, (unsigned)int_largest);
    heap_trace_summary_t summary = {0};
    heap_trace_summary(&summary);
    ESP_LOGI(TAG, "PEER_TRACE total_alloc=%u total_free=%u live=%u high_water=%u overflow=%u cap_overflow=%u",
             (unsigned)summary.total_allocations, (unsigned)summary.total_frees,
             (unsigned)summary.count, (unsigned)summary.high_water_mark,
             (unsigned)summary.has_overflowed, (unsigned)s_cap_overflow);
    size_t internal_total = 0, dma_requested_total = 0, dma_capable_total = 0;
    for (size_t i = 0; i < heap_trace_get_count(); ++i) {
        heap_trace_record_t record = {0};
        if (heap_trace_get(i, &record) != ESP_OK || record.address == NULL ||
            !esp_ptr_internal(record.address)) continue;
        uint32_t caps = 0;
        bool caps_known = false;
        const uint32_t count = s_caps == NULL ? 0 : (s_cap_count < CAP_RECORD_MAX ? s_cap_count : CAP_RECORD_MAX);
        for (uint32_t j = count; j > 0; --j) {
            if (s_caps[j - 1].ptr == record.address) {
                caps = s_caps[j - 1].caps;
                caps_known = true;
                break;
            }
        }
        internal_total += record.size;
        if (caps & MALLOC_CAP_DMA) dma_requested_total += record.size;
        const bool dma_capable = esp_ptr_dma_capable(record.address);
        if (dma_capable) dma_capable_total += record.size;
        /* 仅输出内部存活块，窗口最多512条；大于等于1KB才打印调用点。 */
        if (record.size >= 1024) {
            ESP_LOGI(TAG, "PEER_LIVE size=%u ptr=%p caps=0x%x known=%d dma_addr=%d caller=%p/%p",
                     (unsigned)record.size, record.address, (unsigned)caps, caps_known, dma_capable,
                     record.alloced_by[0], record.alloced_by[1]);
        }
    }
    ESP_LOGI(TAG, "PEER_LIVE_SUM internal_requested=%u dma_capable_address=%u dma_requested=%u (不含分配器元数据)",
             (unsigned)internal_total, (unsigned)dma_capable_total,
             (unsigned)dma_requested_total);
    heap_trace_init_standalone(NULL, 0);
    heap_caps_free(s_records);
    s_records = NULL;
}
#else
void rtc_heap_diag_peer_begin(void) { ESP_LOGW(TAG, "PEER_TRACE disabled in sdkconfig"); }
void rtc_heap_diag_peer_end(void) {}
#endif
