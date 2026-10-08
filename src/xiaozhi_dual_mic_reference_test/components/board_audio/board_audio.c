/* 板级接口参考Waveshare BSP 3.0.1及bsp_board_extra；仅保留音频。 */
#include "bsp_board_extra.h"
#include "esp_codec_dev_defaults.h"
#include "esp_codec_dev.h"
#include "freertos/FreeRTOS.h"
#include "driver/i2c_master.h"
#include "driver/i2s_tdm.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "soc/i2s_struct.h"
#include <math.h>
static const char *TAG="REF_BOARD";
static esp_codec_dev_handle_t play_dev_handle,record_dev_handle;
static const audio_codec_ctrl_if_t *adc_ctrl;
static i2s_chan_handle_t tx,rx;
static uint32_t feed,fetch,wake,sent,send_err;
static int last_vad;
static bool mqtt_transport;
static portMUX_TYPE mux=portMUX_INITIALIZER_UNLOCKED;
static uint64_t sq[3], sample_count;
void ref_diag_feed(void){__atomic_fetch_add(&feed,1,__ATOMIC_RELAXED);}
void ref_diag_fetch(int v){__atomic_fetch_add(&fetch,1,__ATOMIC_RELAXED);__atomic_store_n(&last_vad,v,__ATOMIC_RELAXED);}
void ref_diag_wake(void){__atomic_fetch_add(&wake,1,__ATOMIC_RELAXED);}
void ref_diag_send(int ret){if(ret==ESP_OK)__atomic_fetch_add(&sent,1,__ATOMIC_RELAXED);else __atomic_fetch_add(&send_err,1,__ATOMIC_RELAXED);}
void ref_diag_transport(bool mqtt){__atomic_store_n(&mqtt_transport,mqtt,__ATOMIC_RELAXED);}
void ref_diag_print(void){
 uint64_t local[3],n;portENTER_CRITICAL(&mux);n=sample_count;sample_count=0;
 for(int c=0;c<3;c++){local[c]=sq[c];sq[c]=0;}portEXIT_CRITICAL(&mux);
 unsigned f=__atomic_exchange_n(&feed,0,__ATOMIC_RELAXED),x=__atomic_exchange_n(&fetch,0,__ATOMIC_RELAXED);
 unsigned w=__atomic_exchange_n(&wake,0,__ATOMIC_RELAXED),s=__atomic_exchange_n(&sent,0,__ATOMIC_RELAXED),e=__atomic_exchange_n(&send_err,0,__ATOMIC_RELAXED);
 bool mqtt=__atomic_load_n(&mqtt_transport,__ATOMIC_RELAXED);
 ESP_LOGI("REF_DIAG","5s feed=%u fetch=%u VAD=%d WakeNet=%u uplink=%u send_error=%u transport=%s UDP_send_error=%s%u MIC1_RMS=%.1f MIC2_RMS=%.1f REF_RMS=%.1f raw_frames=%llu",f,x,__atomic_load_n(&last_vad,__ATOMIC_RELAXED),w,s,e,mqtt?"MQTT_UDP":"WEBSOCKET/NOT_READY",mqtt?"":"NA/",mqtt?e:0,n?sqrt((double)local[0]/n):0,n?sqrt((double)local[1]/n):0,n?sqrt((double)local[2]/n):0,(unsigned long long)n);
}
esp_err_t board_audio_init(void){
 ESP_ERROR_CHECK(gpio_set_direction(8,GPIO_MODE_OUTPUT));ESP_ERROR_CHECK(gpio_set_level(8,1));
 i2c_master_bus_handle_t bus;
 i2c_master_bus_config_t b={.i2c_port=0,.sda_io_num=34,.scl_io_num=33,.clk_source=I2C_CLK_SRC_DEFAULT,.glitch_ignore_cnt=7,.flags.enable_internal_pullup=true};
 ESP_RETURN_ON_ERROR(i2c_new_master_bus(&b,&bus),TAG,"I2C");
 i2s_chan_config_t cc=I2S_CHANNEL_DEFAULT_CONFIG(0,I2S_ROLE_MASTER);cc.auto_clear=true;
 ESP_RETURN_ON_ERROR(i2s_new_channel(&cc,&tx,&rx),TAG,"I2S");
 i2s_std_config_t tc={.clk_cfg=I2S_STD_CLK_DEFAULT_CONFIG(24000),.slot_cfg=I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,I2S_SLOT_MODE_STEREO),.gpio_cfg={.mclk=28,.bclk=29,.ws=30,.dout=31,.din=I2S_GPIO_UNUSED}};
 i2s_tdm_config_t rc={.clk_cfg=I2S_TDM_CLK_DEFAULT_CONFIG(24000),.slot_cfg=I2S_TDM_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,I2S_SLOT_MODE_STEREO,I2S_TDM_SLOT0|I2S_TDM_SLOT1|I2S_TDM_SLOT2|I2S_TDM_SLOT3),.gpio_cfg={.mclk=28,.bclk=29,.ws=30,.dout=I2S_GPIO_UNUSED,.din=32}};
 tc.clk_cfg.mclk_multiple=I2S_MCLK_MULTIPLE_256;rc.clk_cfg.mclk_multiple=I2S_MCLK_MULTIPLE_256;rc.clk_cfg.bclk_div=8;rc.slot_cfg.total_slot=4;
 ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(tx,&tc),TAG,"TX");ESP_RETURN_ON_ERROR(i2s_channel_init_tdm_mode(rx,&rc),TAG,"RX");
 ESP_RETURN_ON_ERROR(i2s_channel_enable(tx),TAG,"TX enable");ESP_RETURN_ON_ERROR(i2s_channel_enable(rx),TAG,"RX enable");
 audio_codec_i2s_cfg_t dc={.port=0,.rx_handle=rx,.tx_handle=tx};const audio_codec_data_if_t *data=audio_codec_new_i2s_data(&dc);
 audio_codec_i2c_cfg_t ic={.port=0,.addr=ES8311_CODEC_DEFAULT_ADDR,.bus_handle=bus};
 const audio_codec_ctrl_if_t *dac_ctrl=audio_codec_new_i2c_ctrl(&ic);
 es8311_codec_cfg_t dac={.ctrl_if=dac_ctrl,.gpio_if=audio_codec_new_gpio(),.codec_mode=ESP_CODEC_DEV_WORK_MODE_DAC,.pa_pin=-1,.use_mclk=true,.hw_gain={.pa_voltage=5.0,.codec_dac_voltage=3.3}};
 esp_codec_dev_cfg_t dev={.dev_type=ESP_CODEC_DEV_TYPE_OUT,.codec_if=es8311_codec_new(&dac),.data_if=data};
 ESP_RETURN_ON_FALSE(dev.codec_if && data,ESP_FAIL,TAG,"ES8311/data init");play_dev_handle=esp_codec_dev_new(&dev);
 int address=0;for(int a=0x40;a<=0x43;a++)if(i2c_master_probe(bus,a,100)==ESP_OK){address=a;break;}
 ESP_RETURN_ON_FALSE(address,ESP_ERR_NOT_FOUND,TAG,"ES7210 not found");ic.addr=address<<1;adc_ctrl=audio_codec_new_i2c_ctrl(&ic);
 es7210_codec_cfg_t adc={.ctrl_if=adc_ctrl,.mic_selected=ES7210_SEL_MIC1|ES7210_SEL_MIC2|ES7210_SEL_MIC3};
 dev.dev_type=ESP_CODEC_DEV_TYPE_IN;dev.codec_if=es7210_codec_new(&adc);
 ESP_RETURN_ON_FALSE(dev.codec_if,ESP_FAIL,TAG,"ES7210 init");record_dev_handle=esp_codec_dev_new(&dev);
 ESP_RETURN_ON_FALSE(play_dev_handle && record_dev_handle,ESP_ERR_NO_MEM,TAG,"codec handles");
 ESP_LOGI(TAG,"ES7210=0x%02x ES8311 initialized; GPIO MCLK28 BCLK29 WS30 DOUT31 DIN32 SDA34 SCL33; S0=M1 S2=M2 S1=hardware_REF -> MMR",address);
 return ESP_OK;
}
esp_err_t bsp_extra_codec_dev_stop(void){
 esp_err_t a=esp_codec_dev_close(record_dev_handle),b=esp_codec_dev_close(play_dev_handle);return a==ESP_OK?b:a;
}
esp_err_t bsp_extra_codec_mute_set(bool mute){return esp_codec_dev_set_out_mute(play_dev_handle,mute);}
esp_err_t bsp_extra_codec_set_fs(uint32_t rate,uint32_t bits,i2s_slot_mode_t ch){
 ESP_RETURN_ON_ERROR(bsp_extra_codec_dev_stop(),TAG,"close");
 esp_codec_dev_sample_info_t info={.sample_rate=rate,.channel=ch,.bits_per_sample=bits};
 ESP_RETURN_ON_ERROR(esp_codec_dev_open(play_dev_handle,&info),TAG,"output open");
 ESP_RETURN_ON_ERROR(esp_codec_dev_set_out_vol(play_dev_handle,80),TAG,"volume");return esp_codec_dev_set_out_mute(play_dev_handle,false);
}
esp_err_t bsp_extra_codec_set_voice_fs(uint32_t rate,uint32_t bits,uint8_t channels,uint16_t slots,uint16_t mics){
 ESP_RETURN_ON_ERROR(bsp_extra_codec_set_fs(rate,bits,I2S_SLOT_MODE_STEREO),TAG,"playback format");
 esp_codec_dev_sample_info_t info={.sample_rate=rate,.channel=channels,.channel_mask=slots,.bits_per_sample=bits};
 ESP_RETURN_ON_ERROR(esp_codec_dev_open(record_dev_handle,&info),TAG,"record open");
 ESP_RETURN_ON_ERROR(esp_codec_dev_set_in_channel_gain(record_dev_handle,mics,24.0),TAG,"reference BSP gain");
 uint8_t reg;for(int i=0;i<=0x4c;i++){if((i>0x0d&&i<0x10)||(i>0x23&&i<0x40))continue;ESP_RETURN_ON_ERROR(adc_ctrl->read_reg(adc_ctrl,i,1,&reg,1),TAG,"readback");ESP_LOGI(TAG,"ES7210[%02x]=%02x",i,reg);}
 ESP_LOGI(TAG,"RX TDM actual data=%u slot=%u count=%u ws=%u shift=%u; route=[S0,S2,S1]=MMR",(unsigned)I2S0.rx_conf1.rx_bits_mod+1,(unsigned)I2S0.rx_conf1.rx_tdm_chan_bits+1,(unsigned)I2S0.rx_tdm_ctrl.rx_tdm_tot_chan_num+1,(unsigned)I2S0.rx_conf1.rx_tdm_ws_width+1,(unsigned)I2S0.rx_conf.rx_msb_shift);
 return ESP_OK;
}
esp_err_t bsp_extra_i2s_read(void *data,size_t len,size_t *got,uint32_t timeout){
 (void)timeout;*got=0;esp_err_t ret=esp_codec_dev_read(record_dev_handle,data,len);
 if(ret==ESP_OK){*got=len;uint64_t local[3]={0};const int16_t *pcm=data;unsigned slots[]={0,2,1};
 for(size_t n=0;n<len/8;n++)for(int c=0;c<3;c++){int64_t v=pcm[4*n+slots[c]];local[c]+=v*v;}
 portENTER_CRITICAL(&mux);for(int c=0;c<3;c++)sq[c]+=local[c];sample_count+=len/8;portEXIT_CRITICAL(&mux);}
 return ret;
}
esp_err_t bsp_extra_i2s_write(void *data,size_t len,size_t *got,uint32_t timeout){
 (void)timeout;*got=0;esp_err_t ret=esp_codec_dev_write(play_dev_handle,data,len);if(ret==ESP_OK)*got=len;return ret;
}
