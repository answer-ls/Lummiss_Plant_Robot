#include "h264_send_probe.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_log.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#define PROBE_BYTES (2U * 1024U * 1024U)
#define PROBE_FRAMES 192U
typedef struct {
    uint32_t offset, size, pts, hash;
    int64_t us;
    int result;
    bool idr;
} frame_t;
static uint8_t *s_data;
static frame_t *s_frames;
static uint32_t s_bytes, s_count, s_export_frame, s_export_offset;
static bool s_ready, s_done, s_header, s_frame_header, s_pending, s_limited;

static void finish(bool limited)
{
    s_ready = true; s_limited = limited;
    ESP_LOGW("H264_PROBE", "CAPTURED frames=%" PRIu32 " bytes=%" PRIu32
        " limited=%d；请停止RTC，等待HP_DONE，勿复位", s_count, s_bytes, limited);
}

void h264_send_probe_before(const uint8_t *data, size_t size, uint32_t pts, bool idr)
{
    s_pending = false;
    if (s_ready || s_done || (!s_count && !idr)) return;
    if (!s_data) {
        /* 只在第一张IDR分配，全部使用PSRAM，不挤占硬件DMA内存。 */
        s_data = heap_caps_malloc(PROBE_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        s_frames = heap_caps_calloc(PROBE_FRAMES, sizeof(frame_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_data || !s_frames) {
            heap_caps_free(s_data); heap_caps_free(s_frames);
            s_data = NULL; s_frames = NULL; s_done = true;
            ESP_LOGE("H264_PROBE", "PSRAM不足，停用采集，正常发送继续");
            return;
        }
    }
    if (s_count == PROBE_FRAMES || size > PROBE_BYTES-s_bytes) { finish(true); return; }
    frame_t *f = &s_frames[s_count];
    *f = (frame_t){.offset=s_bytes, .size=size, .pts=pts, .idr=idr,
                   .us=esp_timer_get_time(), .hash=2166136261U};
    memcpy(s_data+s_bytes, data, size);
    for (size_t i=0; i<size; ++i) f->hash = (f->hash ^ s_data[s_bytes+i])*16777619U;
    s_bytes += size;
    s_pending = true;
}

void h264_send_probe_after(int result)
{
    if (!s_pending) return;
    s_pending = false;
    s_frames[s_count].result = result;
    ++s_count;
    if (s_frames[s_count-1].us-s_frames[0].us >= 5000000) finish(false);
}

void h264_send_probe_export_step(void)
{
    if (s_done || !s_count) return;
    /* 提前停止也保留真实短窗口，并标明limited，不能伪造五秒采集。 */
    if (!s_ready) finish(true);
    if (!s_header) {
        printf("HP_BEGIN {\"frames\":%" PRIu32 ",\"bytes\":%" PRIu32
               ",\"limited\":%s,\"duration_us\":%" PRId64 "}\n",
               s_count,s_bytes,s_limited?"true":"false",s_frames[s_count-1].us-s_frames[0].us);
        s_header=true; return;
    }
    if (s_export_frame < s_count) {
        frame_t *f=&s_frames[s_export_frame];
        if (!s_frame_header) {
            printf("HP_FRAME {\"index\":%" PRIu32 ",\"offset\":%" PRIu32
                   ",\"size\":%" PRIu32 ",\"pts_ms\":%" PRIu32 ",\"us\":%" PRId64
                   ",\"idr\":%s,\"ret\":%d,\"hash\":%" PRIu32 "}\n",
                   s_export_frame,f->offset,f->size,f->pts,f->us,f->idr?"true":"false",f->result,f->hash);
            s_frame_header=true; return;
        }
        uint32_t n=f->size-s_export_offset;
        if (n>256) n=256;
        char hex[513]; const char *digits="0123456789abcdef";
        for (uint32_t i=0;i<n;++i) {
            uint8_t byte=s_data[f->offset+s_export_offset+i];
            hex[2*i]=digits[byte>>4]; hex[2*i+1]=digits[byte&15];
        }
        hex[2*n]=0;
        printf("HP_DATA %" PRIu32 " %s\n",f->offset+s_export_offset,hex);
        s_export_offset+=n;
        if (s_export_offset==f->size) {
            s_export_offset=0; ++s_export_frame; s_frame_header=false;
        }
        return;
    }
    printf("HP_DONE\n");
    heap_caps_free(s_data); heap_caps_free(s_frames);
    s_data=NULL; s_frames=NULL; s_done=true;
}
