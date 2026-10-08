#include <stdio.h>
#include <stdint.h>
#include <math.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/i2s_tdm.h"
#include "driver/uart.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "mbedtls/base64.h"
#include "soc/i2s_struct.h"
#include "es7210.h"

#define RATE 16000
#define SLOTS 4
#define FRAMES (RATE * 10)
#define BYTES (FRAMES * SLOTS * sizeof(int16_t))
static i2s_chan_handle_t rx;
static i2c_master_dev_handle_t adc;
static uint32_t overflows;

/* 中断只计数；是否丢历史DMA块不能由read返回ESP_OK来判断。 */
static bool IRAM_ATTR overflow_cb(i2s_chan_handle_t h, i2s_event_data_t *e, void *arg)
{
    (void)h; (void)e; (void)arg;
    __atomic_fetch_add(&overflows, 1, __ATOMIC_RELAXED);
    return false;
}

static void hardware_init(void)
{
    /* 自研板电源保持脚；不初始化屏幕、功放、ES8311或任何业务组件。 */
    ESP_ERROR_CHECK(gpio_set_direction(GPIO_NUM_8, GPIO_MODE_OUTPUT));
    ESP_ERROR_CHECK(gpio_set_level(GPIO_NUM_8, 1));
    i2c_master_bus_handle_t bus;
    i2c_master_bus_config_t b = {.i2c_port=0, .sda_io_num=34, .scl_io_num=33,
        .clk_source=I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt=7,
        .flags.enable_internal_pullup=true};
    ESP_ERROR_CHECK(i2c_new_master_bus(&b, &bus));
    int address = -1;
    for (int a=0x40; a<=0x43; a++) {
        if (i2c_master_probe(bus, a, 100)==ESP_OK) {
            ESP_ERROR_CHECK(address < 0 ? ESP_OK : ESP_ERR_INVALID_STATE);
            address=a;
        }
    }
    ESP_ERROR_CHECK(address >= 0 ? ESP_OK : ESP_ERR_NOT_FOUND);
    i2c_device_config_t d = {.dev_addr_length=I2C_ADDR_BIT_LEN_7,
        .device_address=address, .scl_speed_hz=100000};
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus, &d, &adc));
    printf("HA_BOARD adc=0x%02x SDA=34 SCL=33 MCLK=28 BCLK=29 WS=30 DIN=32 DOUT=unused TX=absent\n",address);
    i2s_chan_config_t c = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    /* 沿用参考示例的8个DMA块、每块64帧，四槽由板级TDM适配。 */
    c.dma_desc_num=8; c.dma_frame_num=64;
    ESP_ERROR_CHECK(i2s_new_channel(&c, NULL, &rx));
    i2s_tdm_config_t t = {
        .clk_cfg=I2S_TDM_CLK_DEFAULT_CONFIG(RATE),
        .slot_cfg=I2S_TDM_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
            I2S_SLOT_MODE_STEREO, I2S_TDM_SLOT0|I2S_TDM_SLOT1|I2S_TDM_SLOT2|I2S_TDM_SLOT3),
        .gpio_cfg={.mclk=28,.bclk=29,.ws=30,.dout=I2S_GPIO_UNUSED,.din=32}
    };
    t.clk_cfg.mclk_multiple=I2S_MCLK_MULTIPLE_256;
    t.slot_cfg.slot_bit_width=I2S_SLOT_BIT_WIDTH_16BIT;
    t.slot_cfg.total_slot=4;
    ESP_ERROR_CHECK(i2s_channel_init_tdm_mode(rx, &t));
    i2s_event_callbacks_t cb={.on_recv_q_ovf=overflow_cb};
    ESP_ERROR_CHECK(i2s_channel_register_event_callback(rx, &cb, NULL));
    ESP_ERROR_CHECK(uart_driver_install(UART_NUM_0, 1024, 0, 0, NULL, 0));
}

static void configure_adc(unsigned selection)
{
    audio_hal_codec_config_t config={.adc_input=AUDIO_HAL_ADC_INPUT_ALL,
        .codec_mode=AUDIO_HAL_CODEC_MODE_ENCODE,
        .i2s_iface={.mode=AUDIO_HAL_MODE_SLAVE,.fmt=AUDIO_HAL_I2S_NORMAL,
            .samples=AUDIO_HAL_16K_SAMPLES,.bits=AUDIO_HAL_BIT_LENGTH_16BITS}};
    /* 保留Waveshare原初始化、增益、START顺序；最后选择当前测试输入。 */
    ESP_ERROR_CHECK(es7210_adc_init(adc, &config));
    ESP_ERROR_CHECK(es7210_adc_config_i2s(config.codec_mode, &config.i2s_iface));
    ESP_ERROR_CHECK(es7210_adc_set_gain(ES7210_INPUT_MIC1|ES7210_INPUT_MIC2, GAIN_0DB));
    ESP_ERROR_CHECK(es7210_adc_set_gain(ES7210_INPUT_MIC3|ES7210_INPUT_MIC4, GAIN_37_5DB));
    ESP_ERROR_CHECK(es7210_adc_ctrl_state(config.codec_mode, AUDIO_HAL_CTRL_START));
    ESP_ERROR_CHECK(es7210_mic_select(selection));
    /* 实际读回：确认单麦选择没有把四槽格式一起改掉。 */
    for (unsigned r=0; r<=0x4c; r++) {
        if ((r>0x0d && r<0x10)||(r>0x23 && r<0x40)) continue;
        printf("HA_REG %02x %02x\n",r,es7210_read_reg(r));
    }
    ESP_ERROR_CHECK(es7210_read_reg(0x12)==2 ? ESP_OK : ESP_ERR_INVALID_RESPONSE);
    printf("HA_FORMAT rate=%d data_bits=%u slot_bits=%u slots=%u ws_width=%u shift=%u lsb=%u rx_conf=%08"PRIx32" rx_conf1=%08"PRIx32" tdm=%08"PRIx32"\n",
        RATE, (unsigned)I2S0.rx_conf1.rx_bits_mod+1,
        (unsigned)I2S0.rx_conf1.rx_tdm_chan_bits+1,
        (unsigned)I2S0.rx_tdm_ctrl.rx_tdm_tot_chan_num+1,
        (unsigned)I2S0.rx_conf1.rx_tdm_ws_width+1,
        (unsigned)I2S0.rx_conf.rx_msb_shift, (unsigned)I2S0.rx_conf.rx_bit_order,
        I2S0.rx_conf.val,I2S0.rx_conf1.val,I2S0.rx_tdm_ctrl.val);
}

static void stats(const int16_t *pcm, size_t frames)
{
    for (unsigned ch=0; ch<SLOTS; ch++) {
        int64_t sum=0; uint64_t sq=0; unsigned peak=0, clip=0, zeros=0;
        for(size_t n=0;n<frames;n++) {
            int v=pcm[n*SLOTS+ch]; unsigned a=v<0?-v:v;
            sum+=v; sq+=(int64_t)v*v; if(a>peak)peak=a;
            clip+=(v==32767 || v==-32768); zeros+=(v==0);
        }
        double dc=frames?(double)sum/frames:0;
        double power=frames?(double)sq/frames:0;
        printf("HA_STATS slot=%u samples=%u dc=%.6f rms=%.6f ac_rms=%.6f peak=%u clipping=%u zero_count=%u\n",
            ch,(unsigned)frames,dc,sqrt(power),sqrt(fmax(0,power-dc*dc)),peak,clip,zeros);
    }
}

static void record_and_export(unsigned selection, uint8_t *buffer)
{
    configure_adc(selection);
    printf("HA_RECORD group=%u seconds=10 rate=%d slots=4 gain_mic1_mic2_db=0\n",selection,RATE);
    fflush(stdout);
    ESP_ERROR_CHECK(i2s_channel_enable(rx));
    /* 丢弃启动稳定期，同时排空驱动队列；录音阶段不打印、不做频谱运算。 */
    uint8_t discard[512]; size_t got;
    for(unsigned i=0;i<125;i++) ESP_ERROR_CHECK(i2s_channel_read(rx,discard,sizeof(discard),&got,1000));
    uint32_t ov0=__atomic_load_n(&overflows,__ATOMIC_RELAXED);
    int64_t begin=esp_timer_get_time(); size_t used=0; esp_err_t ret=ESP_OK;
    while(used<BYTES) {
        got=0;
        ret=i2s_channel_read(rx,buffer+used,512,&got,1000);
        used+=got;
        if(ret!=ESP_OK || got!=512) { if(ret==ESP_OK)ret=ESP_ERR_INVALID_SIZE; break; }
    }
    int64_t elapsed=esp_timer_get_time()-begin;
    ESP_ERROR_CHECK(i2s_channel_disable(rx));
    uint32_t ov=__atomic_load_n(&overflows,__ATOMIC_RELAXED)-ov0;
    stats((const int16_t *)buffer,used/8);
    printf("HA_BEGIN {\"group\":%u,\"bytes\":%u,\"rate\":%d,\"channels\":4,\"bits\":16,\"elapsed_us\":%lld,\"rx_overflow\":%"PRIu32",\"ret\":%d,\"valid\":%s}\n",
        selection,(unsigned)used,RATE,(long long)elapsed,ov,ret,
        ret==ESP_OK && used==BYTES && !ov ? "true":"false");
    uint32_t hash=2166136261u;
    /* 停止采集后才导出完整PCM，串口吞吐不会改变采集节奏。 */
    for(size_t offset=0;offset<used;) {
        size_t n=used-offset;if(n>192)n=192;
        unsigned char out[257];size_t length=0;
        ESP_ERROR_CHECK(mbedtls_base64_encode(out,sizeof(out),&length,buffer+offset,n)==0?ESP_OK:ESP_FAIL);
        for(size_t k=0;k<n;k++)hash=(hash^buffer[offset+k])*16777619u;
        printf("HA_DATA %u %.*s\n",(unsigned)offset,(int)length,out);
        offset+=n;
        if((offset%6144)==0)vTaskDelay(1);
    }
    printf("HA_END %08"PRIx32"\n",hash);fflush(stdout);
}

void app_main(void)
{
    hardware_init();
    uint8_t *buffer=heap_caps_malloc(BYTES,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    ESP_ERROR_CHECK(buffer?ESP_OK:ESP_ERR_NO_MEM);
    for(;;) {
        printf("HA_READY commands=1:MIC1,2:MIC2,3:DUAL\n");fflush(stdout);
        uint8_t c=0;
        do { uart_read_bytes(UART_NUM_0,&c,1,pdMS_TO_TICKS(1000)); } while(c<'1'||c>'3');
        record_and_export(c-'0',buffer);
    }
}
