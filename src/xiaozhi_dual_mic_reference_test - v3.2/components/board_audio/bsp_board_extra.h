#pragma once
#include "esp_err.h"
#include "driver/i2s_std.h"
#define CODEC_VOICE_INPUT_CHANNELS 4
#define BSP_EXTRA_ES7210_TDM_ALL_SLOTS_MASK 15
#define BSP_EXTRA_ES7210_PHYSICAL_CONNECTED_MIC_MASK 7
#ifdef __cplusplus
extern "C" {
#endif
esp_err_t board_audio_init(void);
esp_err_t bsp_extra_codec_set_voice_fs(uint32_t rate,uint32_t bits,uint8_t channels,uint16_t slot_mask,uint16_t mic_mask);
esp_err_t bsp_extra_codec_set_fs(uint32_t rate,uint32_t bits,i2s_slot_mode_t channels);
esp_err_t bsp_extra_codec_dev_stop(void);
esp_err_t bsp_extra_codec_mute_set(bool mute);
esp_err_t bsp_extra_i2s_read(void *data,size_t len,size_t *got,uint32_t timeout);
esp_err_t bsp_extra_i2s_write(void *data,size_t len,size_t *got,uint32_t timeout);
void ref_diag_feed(void);
void ref_diag_fetch(int vad);
void ref_diag_wake(void);
void ref_diag_send(int result);
void ref_diag_transport(bool mqtt);
void ref_diag_print(void);
#ifdef __cplusplus
}
#endif
