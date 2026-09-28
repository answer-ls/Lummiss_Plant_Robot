#include "capture_diag.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"

/* 两次独立的连续监听入口分别采A/B；每次稳定500ms后观察4秒。
 * 不强制保持语音状态：状态变更立即失效，不能把不同状态混作有效对照。 */
#define WINDOW_US 4000000LL
#define CAPACITY 2048
#define RAW_CAPACITY (24000 * 8 * 5)
typedef struct { int64_t begin, end, next; int result; } read_row_t;
typedef struct { unsigned count; uint32_t values[CAPACITY]; } series_t;
typedef struct {
    read_row_t rows[CAPACITY];
    series_t metric[CD_METRICS];
    uint8_t raw[RAW_CAPACITY];
    size_t raw_bytes;
    unsigned rows_used, truncated, read_errors, packets, bytes;
    unsigned overflow_start, overflow_end, epoch;
    int64_t start, finish;
    bool crossed, done;
} window_t;
static window_t *windows;
static uint32_t overflow_count, state_epoch;
static bool continuous;
static int active = -1, completed;
static unsigned armed_epoch;
static int64_t armed_at;
static bool row_recorded;
static const char *metric_names[] = {"resample_us", "afe_feed_us", "opus_encode_us", "capture_guard_wait_us", "udp_send_us"};

/* ISR仅更新内部RAM中的原子计数，不访问PSRAM、不打印。 */
bool IRAM_ATTR capture_diag_overflow(i2s_chan_handle_t channel, i2s_event_data_t *event, void *ctx)
{
    (void)channel; (void)event; (void)ctx;
    __atomic_fetch_add(&overflow_count, 1, __ATOMIC_RELAXED);
    return false;
}

void capture_diag_state(bool listening)
{
    __atomic_store_n(&continuous, listening, __ATOMIC_RELEASE);
    __atomic_add_fetch(&state_epoch, 1, __ATOMIC_RELEASE);
}

void capture_diag_begin(int64_t begin)
{
    row_recorded = false;
    if (!windows || completed == 2) return;
    unsigned epoch = __atomic_load_n(&state_epoch, __ATOMIC_ACQUIRE);
    bool listening = __atomic_load_n(&continuous, __ATOMIC_ACQUIRE);
    if (active >= 0) {
        window_t *w = &windows[active];
        if (w->rows_used) w->rows[w->rows_used - 1].next = begin;
        if (!listening || epoch != w->epoch || begin - w->start >= WINDOW_US) {
            w->crossed |= !listening || epoch != w->epoch;
            w->finish = begin;
            w->overflow_end = __atomic_load_n(&overflow_count, __ATOMIC_RELAXED);
            __atomic_store_n(&w->done, true, __ATOMIC_RELEASE);
            active = -1;
            completed++;
            /* B必须来自下一个连续监听入口，不紧接A在同一轮服务器状态下启动。 */
            armed_epoch = epoch;
            armed_at = 0;
            return;
        }
    } else {
        if (!listening) return;
        if (epoch != armed_epoch) { armed_epoch = epoch; armed_at = begin; }
        if (!armed_at || begin - armed_at < 500000) return;
        active = completed;
        window_t *w = &windows[active];
        w->start = begin;
        w->epoch = epoch;
        w->overflow_start = __atomic_load_n(&overflow_count, __ATOMIC_RELAXED);
    }
    window_t *w = &windows[active];
    if (w->rows_used == CAPACITY) { w->truncated++; return; }
    w->rows[w->rows_used++].begin = begin;
    row_recorded = true;
}

void capture_diag_read_end(int64_t end, int result, const void *raw, size_t bytes)
{
    if (active < 0 || !windows) return;
    window_t *w = &windows[active];
    if (!row_recorded) return;
    read_row_t *row = &w->rows[w->rows_used - 1];
    row->end = end; row->result = result;
    if (result) { w->read_errors++; return; }
    /* 保留原始顺序与缺口，不补零，也不冒称codec API返回了actual bytes_read。 */
    if (w->raw_bytes + bytes > RAW_CAPACITY) { w->truncated++; return; }
    memcpy(w->raw + w->raw_bytes, raw, bytes);
    w->raw_bytes += bytes;
}

void capture_diag_metric(cd_metric_t metric, int64_t elapsed)
{
    if (active < 0 || !windows) return;
    window_t *w = &windows[active];
    /* B窗口混入实际网络发送，不能作为干净对照。 */
    if (metric == CD_UDP && active == 1) w->crossed = true;
    series_t *s = &w->metric[metric];
    if (s->count < CAPACITY) s->values[s->count++] = elapsed > UINT32_MAX ? UINT32_MAX : (uint32_t)elapsed;
    else w->truncated++;
}

bool capture_diag_sink(size_t bytes)
{
    if (active != 1 || !windows) return false;
    window_t *w = &windows[1];
    /* 截止点在实际send前再次核实，不能因一次慢处理把屏蔽延长到窗口外。 */
    if (esp_timer_get_time() - w->start >= WINDOW_US ||
        !__atomic_load_n(&continuous, __ATOMIC_ACQUIRE) ||
        __atomic_load_n(&state_epoch, __ATOMIC_ACQUIRE) != w->epoch) return false;
    w->packets++; w->bytes += bytes;
    return true;
}

void capture_diag_udp_result(int result, size_t bytes)
{
    if (active != 0 || !windows) return;
    windows[0].packets++; windows[0].bytes += bytes;
    if (result) windows[0].crossed = true;
}

static int compare_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return (x > y) - (x < y);
}

/* 录音结束后排序算精确nearest-rank分位数，不在实时路径排序/打印。 */
static void summarize(int phase, const char *name, uint32_t *v, unsigned n)
{
    if (!n) { printf("CD_STAT %d %s 0 0 0 0 0\n", phase, name); return; }
    uint64_t sum = 0;
    for (unsigned i = 0; i < n; i++) sum += v[i];
    qsort(v, n, sizeof(*v), compare_u32);
    printf("CD_STAT %d %s %u %lu %.2f %lu %lu\n", phase, name, n,
           (unsigned long)v[n-1], (double)sum/n,
           (unsigned long)v[(95*n+99)/100-1], (unsigned long)v[(99*n+99)/100-1]);
}

static void export_task(void *arg)
{
    (void)arg;
    while (!__atomic_load_n(&windows[1].done, __ATOMIC_ACQUIRE)) vTaskDelay(pdMS_TO_TICKS(250));
    /* 两段窗口都封存后才串口导出；导出期间不属于测量。 */
    for (int phase = 0; phase < 2; phase++) {
        window_t *w = &windows[phase];
        printf("CD_BEGIN %d %u %lld %lld %u %u %u %u %u %u\n", phase,
               (unsigned)w->raw_bytes, (long long)w->start, (long long)w->finish,
               w->crossed, w->truncated, w->read_errors,
               w->overflow_end - w->overflow_start, w->packets, w->bytes);
        uint32_t *interval = malloc(w->rows_used * sizeof(uint32_t));
        uint32_t *post = malloc(w->rows_used * sizeof(uint32_t));
        if (!interval || !post) { ESP_LOGE("CAP_DIAG", "导出统计内存不足，停止且不宣称采集成功"); free(interval); free(post); vTaskDeleteWithCaps(NULL); return; }
        unsigned n = 0, gaps[5] = {0};
        const unsigned limits[] = {15000, 30000, 50000, 60000, 100000};
        for (unsigned i = 0; i < w->rows_used; i++) {
            read_row_t *row = &w->rows[i];
            printf("CD_ROW %d %lld %lld %lld %d\n", phase, (long long)row->begin, (long long)row->end, (long long)row->next, row->result);
            if (row->next && row->end) {
                interval[n] = row->next - row->begin;
                post[n] = row->next - row->end;
                for (unsigned j=0;j<5;j++) gaps[j] += interval[n] > limits[j];
                n++;
            }
        }
        summarize(phase,"read_interval_us",interval,n);
        summarize(phase,"post_read_processing_us",post,n);
        free(interval);free(post);
        printf("CD_GAPS %d %u %u %u %u %u\n",phase,gaps[0],gaps[1],gaps[2],gaps[3],gaps[4]);
        for (unsigned m=0;m<CD_METRICS;m++) summarize(phase,metric_names[m],w->metric[m].values,w->metric[m].count);
        uint32_t hash = 2166136261u;
        for (size_t offset=0;offset<w->raw_bytes;) {
            size_t bytes=w->raw_bytes-offset;if(bytes>240)bytes=240;
            unsigned char encoded[324];size_t written=0;
            mbedtls_base64_encode(encoded,sizeof(encoded),&written,w->raw+offset,bytes);
            for(size_t i=0;i<bytes;i++) hash=(hash^w->raw[offset+i])*16777619u;
            printf("CD_DATA %d %u %.*s\n",phase,(unsigned)offset,(int)written,encoded);
            offset+=bytes;vTaskDelay(1);
        }
        printf("CD_END %d %08lx\n",phase,(unsigned long)hash);
    }
    printf("CD_DONE\n");
    /* 单次诊断完成后保留只读缓冲至重启，避免与capture入口读指针并发释放。 */
    vTaskDeleteWithCaps(NULL);
}

esp_err_t capture_diag_init(void)
{
    if (!CAPTURE_UPLINK_AB_DIAGNOSTIC) return ESP_OK;
    if (windows) return ESP_OK;
    windows = heap_caps_calloc(2,sizeof(*windows),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if (!windows) return ESP_ERR_NO_MEM;
    /* 导出任务只读软件诊断数据，执行排序、base64和串口输出，不直接驱动DMA，
     * 也不执行flash映射/擦写。仅将4096B栈迁PSRAM，TCB仍由IDF放INTERNAL。
     * 保持原优先级及无核绑定；退出必须使用配套WithCaps接口回收栈。 */
    if (xTaskCreateWithCaps(export_task,"capture_diag",4096,NULL,1,NULL,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)!=pdPASS) {
        free(windows);windows=NULL;return ESP_ERR_NO_MEM;
    }
    ESP_LOGI("CAP_DIAG","已启用：前两次CONTINUOUS_LISTENING各等待500ms后采4秒；A正常发送/B本地计数；换状态即无效；录完导出");
    return ESP_OK;
}
