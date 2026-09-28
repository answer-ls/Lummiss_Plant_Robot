#include "lifecycle_probe.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "mbedtls/base64.h"
#include "soc/i2s_struct.h"
#include "soc/hp_sys_clkrst_struct.h"

#define N 9
#define BYTES (24000 * 8)
/* 每个标记后取1秒；短状态会跨越后续切换，必须据crossed判断，不能冒充纯状态。 */
static const char *names[N] = {"AUDIO_INIT_DONE","TX_NO_APP_WRITE","TX_ZERO","TX_PLAY_OUTPUT",
    "WAKE_IDLE","LISTENING","SPEAKING","POST_TTS","CONTINUOUS_LISTENING"};
struct snapshot { int64_t us; int16_t reg[77]; uint32_t hw[10]; };
static struct record { uint8_t *data; size_t used; int64_t mark, first, last; unsigned crossed;
    bool pending, after_pending, have_before, have_after; int error; struct snapshot before, after; } records[N];
static SemaphoreHandle_t mutex;
static const audio_codec_ctrl_if_t *control;
static bool running;
static int64_t last_mark;
static struct {int64_t us; char name[32];} events[64];
static unsigned event_count;

static void snapshot(struct snapshot *s)
{
    s->us=esp_timer_get_time();
    for (int r=0;r<77;r++) {
        s->reg[r]=-2;
        if ((r>13 && r<16)||(r>35 && r<64)) continue;
        uint8_t v=0;
        s->reg[r]=control->read_reg(control,r,1,&v,1)==0?v:-1;
    }
    s->hw[0]=I2S0.rx_conf.val;s->hw[1]=I2S0.rx_conf1.val;s->hw[2]=I2S0.rx_tdm_ctrl.val;
    s->hw[3]=I2S0.tx_conf.val;s->hw[4]=I2S0.tx_conf1.val;s->hw[5]=I2S0.tx_tdm_ctrl.val;
    s->hw[6]=HP_SYS_CLKRST.peri_clk_ctrl11.val;s->hw[7]=HP_SYS_CLKRST.peri_clk_ctrl12.val;
    s->hw[8]=HP_SYS_CLKRST.peri_clk_ctrl13.val;s->hw[9]=HP_SYS_CLKRST.peri_clk_ctrl14.val;
}

void lifecycle_mark(const char *name)
{
    if (!mutex) return;
    xSemaphoreTake(mutex,portMAX_DELAY);
    if (running) {
        if (!strcmp(name,"POST_TTS") && records[7].mark) {xSemaphoreGive(mutex);return;}
        int64_t now=esp_timer_get_time();
        if(event_count<64){events[event_count].us=now;snprintf(events[event_count].name,32,"%s",name);event_count++;}
        for (unsigned i=4;i<N;i++) if (records[i].mark && records[i].used<BYTES) records[i].crossed++;
        for (unsigned i=4;i<N;i++) if (!strcmp(name,names[i]) && !records[i].mark) {
            records[i].mark=now;records[i].pending=true;last_mark=now;
        }
    }
    xSemaphoreGive(mutex);
}
void lifecycle_raw(const void *data,size_t bytes)
{
    if (!mutex || bytes%8) return;
    xSemaphoreTake(mutex,portMAX_DELAY);
    if (running) for (unsigned i=4;i<N;i++) {
        struct record *r=&records[i];
        if (!r->mark || r->used==BYTES) continue;
        size_t n=BYTES-r->used;if(n>bytes)n=bytes;
        int64_t now=esp_timer_get_time();
        if(!r->used)r->first=now;
        memcpy(r->data+r->used,data,n);r->used+=n;r->last=now;
        if(r->used==BYTES)r->after_pending=true;
    }
    xSemaphoreGive(mutex);
}

/* 启动阶段独占RX/TX。RX为双工从通道，必须保留TX主时钟，不改变格式或ADC寄存器。 */
esp_err_t lifecycle_prepare(const audio_codec_ctrl_if_t *ctrl,i2s_chan_handle_t rx,i2s_chan_handle_t tx)
{
    control=ctrl;if(!ctrl)return ESP_ERR_INVALID_ARG;
    uint8_t *memory=heap_caps_malloc(N*BYTES,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    mutex=xSemaphoreCreateMutex();if(!memory||!mutex){free(memory);return ESP_ERR_NO_MEM;}
    for(unsigned i=0;i<N;i++)records[i].data=memory+i*BYTES;
    int16_t tone[240],zero[240]={0};uint8_t discard[1920];
    for(unsigned i=0;i<240;i++)tone[i]=(int16_t)(512*sinf(2*3.14159265358979323846f*i/24));
    ESP_LOGW("LIFE","PA_MUTED=UNSUPPORTED：功放CTRL未接GPIO，不以DAC静音替代");
    for(unsigned phase=0;phase<4;phase++) {
        struct record *r=&records[phase];r->mark=esp_timer_get_time();
        /* 此阶段不调用write，TX仍运行并自动清空DMA；不是硬件TX_OFF。 */
        if(phase==1) ESP_LOGW("LIFE","TX_OFF=UNSUPPORTED shared_clock: TX_NO_APP_WRITE tx_enabled=1 app_writes=0 auto_clear=1");
        snapshot(&r->before);r->have_before=true;
        for(unsigned j=0;j<20;j++){size_t got=0;esp_err_t e=i2s_channel_read(rx,discard,sizeof(discard),&got,100);if(e!=ESP_OK)break;}
        for(size_t pos=0;pos<BYTES;pos+=1920){
            size_t got=0;
            if(phase>=2){esp_err_t e=i2s_channel_write(tx,phase==2?zero:tone,sizeof(tone),&got,100);if(e!=ESP_OK||got!=sizeof(tone)){r->error=e?e:ESP_FAIL;break;}}
            esp_err_t e=i2s_channel_read(rx,r->data+pos,1920,&got,100);
            if(e!=ESP_OK||got!=1920){r->error=e?e:ESP_FAIL;break;}
            if(!r->used) { r->first=esp_timer_get_time(); }
            r->used+=got;r->last=esp_timer_get_time();
        }
        snapshot(&r->after);r->have_after=true;
        ESP_LOGI("LIFE","BOOT_WINDOW %s bytes=%u ret=%d",names[phase],(unsigned)r->used,r->error);
    }
    for(unsigned i=0;i<20;i++){size_t n=0;esp_err_t e=i2s_channel_write(tx,zero,sizeof(zero),&n,100);if(e!=ESP_OK||n!=sizeof(zero))return ESP_FAIL;}
    return ESP_OK;
}

static void emit_snapshot(unsigned id,const char *edge,struct snapshot *s)
{
    printf("LC_SNAPSHOT %u %s %lld",id,edge,(long long)s->us);
    for(unsigned k=0;k<10;k++) { printf(" %08lx",(unsigned long)s->hw[k]); }
    printf("\n");
    for(unsigned k=0;k<77;k++)if(s->reg[k]!=-2)printf("LC_REG %u %s %02x %d\n",id,edge,k,s->reg[k]);
}
static void worker(void *arg)
{
    (void)arg;int64_t limit=esp_timer_get_time()+180000000;
    for(;;){
        for(unsigned i=4;i<N;i++)for(unsigned edge=0;edge<2;edge++){
            xSemaphoreTake(mutex,portMAX_DELAY);
            bool pending=edge?records[i].after_pending:records[i].pending;
            if(edge)records[i].after_pending=false;else records[i].pending=false;
            xSemaphoreGive(mutex);
            if(pending){snapshot(edge?&records[i].after:&records[i].before);if(edge)records[i].have_after=true;else records[i].have_before=true;}
        }
        xSemaphoreTake(mutex,portMAX_DELAY);
        bool end=(records[8].used==BYTES && esp_timer_get_time()-last_mark>35000000)||esp_timer_get_time()>limit;
        if(end)running=false;
        xSemaphoreGive(mutex);if(end)break;vTaskDelay(pdMS_TO_TICKS(10));
    }
    ESP_LOGI("LIFE","EXPORT 开始；短状态窗口可能跨状态，详见crossed和时间戳");
    for(unsigned i=0;i<event_count;i++)printf("LC_EVENT %lld %s\n",(long long)events[i].us,events[i].name);
    for(unsigned i=0;i<N;i++){
        struct record *r=&records[i];
        printf("LC_BEGIN %u %s %u %lld %lld %lld %u %d\n",i,names[i],(unsigned)r->used,(long long)r->mark,(long long)r->first,(long long)r->last,r->crossed,r->error);
        if(r->have_before)emit_snapshot(i,"BEFORE",&r->before);
        if(r->have_after)emit_snapshot(i,"AFTER",&r->after);
        uint32_t hash=2166136261u;
        for(size_t pos=0;pos<r->used;){size_t len=r->used-pos;if(len>240)len=240;unsigned char text[324];size_t out;
            mbedtls_base64_encode(text,sizeof(text),&out,r->data+pos,len);
            for(size_t k=0;k<len;k++)hash=(hash^r->data[pos+k])*16777619u;
            printf("LC_DATA %u %u %.*s\n",i,(unsigned)pos,(int)out,text);pos+=len;vTaskDelay(1);
        }
        printf("LC_END %u %08lx\n",i,(unsigned long)hash);
    }
    printf("LC_DONE\n");free(records[0].data);vTaskDelete(NULL);
}
void lifecycle_start(void)
{
    if(!mutex)return;
    running=true;last_mark=esp_timer_get_time();
    if(xTaskCreate(worker,"life_probe",4096,NULL,1,NULL)!=pdPASS){running=false;ESP_LOGE("LIFE","worker创建失败");return;}
    lifecycle_mark("WAKE_IDLE");
}
