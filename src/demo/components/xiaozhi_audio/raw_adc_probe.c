#include "raw_adc_probe.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <math.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include "soc/i2s_struct.h"
#include "soc/gpio_struct.h"
#include "soc/gpio_sig_map.h"
#include "soc/hp_sys_clkrst_struct.h"
#include "board_pins.h"

#define RAW_BYTES (24000 * 4 * 2 * 8)
#define BLOCK_BYTES 1920
static const char *TAG = "RAW_ADC";
static const char *names[] = {"MIC1_NEAR", "MIC2_NEAR", "PLAYBACK_1K"};
static uint32_t rx_overflows;

/* ISR 只计数，不打印；采集期间丢帧会使该组录音判为无效。 */
static bool IRAM_ATTR on_rx_overflow(i2s_chan_handle_t handle, i2s_event_data_t *event, void *context)
{
    (void)handle; (void)event; (void)context;
    __atomic_fetch_add(&rx_overflows, 1, __ATOMIC_RELAXED);
    return false;
}

static esp_err_t read_reg(const audio_codec_ctrl_if_t *ctrl, uint8_t reg, uint8_t *value)
{
    int ret = ctrl->read_reg(ctrl, reg, 1, value, 1);
    if (ret) ESP_LOGE(TAG, "I2C_READ reg=0x%02x error=%d", reg, ret);
    return ret ? ESP_FAIL : ESP_OK;
}

static esp_err_t dump_registers(const audio_codec_ctrl_if_t *ctrl, const char *phase)
{
    ESP_LOGI(TAG, "REG_BEGIN phase=%s source=I2C_READBACK", phase);
    for (unsigned reg = 0; reg <= 0x4c; reg++) {
        if ((reg > 0x0d && reg < 0x10) || (reg > 0x23 && reg < 0x40)) continue;
        uint8_t value;
        if (read_reg(ctrl, reg, &value) != ESP_OK) return ESP_FAIL;
        ESP_LOGI(TAG, "REG phase=%s addr=%02x value=%02x", phase, reg, value);
    }
    ESP_LOGI(TAG, "P4_READBACK rx_conf=%08" PRIx32 " rx_conf1=%08" PRIx32
             " rx_tdm=%08" PRIx32 " tx_conf=%08" PRIx32 " tx_conf1=%08" PRIx32 " tx_tdm=%08" PRIx32,
             I2S0.rx_conf.val, I2S0.rx_conf1.val, I2S0.rx_tdm_ctrl.val,
             I2S0.tx_conf.val, I2S0.tx_conf1.val, I2S0.tx_tdm_ctrl.val);
    ESP_LOGI(TAG, "P4_FORMAT data_bits=%u slot_bits=%u slots=%u ws_width=%u bit_shift=%u bit_order=%u",
             (unsigned)I2S0.rx_conf1.rx_bits_mod + 1, (unsigned)I2S0.rx_conf1.rx_tdm_chan_bits + 1,
             (unsigned)I2S0.rx_tdm_ctrl.rx_tdm_tot_chan_num + 1,
             (unsigned)I2S0.rx_conf1.rx_tdm_ws_width + 1,
             (unsigned)I2S0.rx_conf.rx_msb_shift, (unsigned)I2S0.rx_conf.rx_bit_order);
    ESP_LOGI(TAG, "P4_CLOCK_READBACK ctrl11=%08" PRIx32 " ctrl12=%08" PRIx32
             " ctrl13=%08" PRIx32 " ctrl14=%08" PRIx32,
             HP_SYS_CLKRST.peri_clk_ctrl11.val, HP_SYS_CLKRST.peri_clk_ctrl12.val,
             HP_SYS_CLKRST.peri_clk_ctrl13.val, HP_SYS_CLKRST.peri_clk_ctrl14.val);
    ESP_LOGI(TAG, "P4_GPIO_READBACK MCLK=%08" PRIx32 " BCLK=%08" PRIx32 " WS=%08" PRIx32
             " RX_BCLK_IN=%08" PRIx32 " RX_WS_IN=%08" PRIx32,
             GPIO.func_out_sel_cfg[BOARD_AUDIO_MCLK].val,
             GPIO.func_out_sel_cfg[BOARD_AUDIO_BCLK].val,
             GPIO.func_out_sel_cfg[BOARD_AUDIO_WS].val,
             GPIO.func_in_sel_cfg[I2S0_I_BCK_PAD_IN_IDX].val,
             GPIO.func_in_sel_cfg[I2S0_I_WS_PAD_IN_IDX].val);
    uint8_t format, tdm;
    if (read_reg(ctrl, 0x11, &format) != ESP_OK || read_reg(ctrl, 0x12, &tdm) != ESP_OK) return ESP_FAIL;
    const unsigned bits[] = {24, 20, 18, 16, 32, 0, 0, 0};
    ESP_LOGI(TAG, "ES7210_FORMAT data_bits=%u format_bits=%u lrck_invert=%u tdm_mode=%u (slot clocks require waveform)",
             bits[format >> 5], format & 3, (format >> 4) & 1, tdm & 3);
    return ESP_OK;
}

static esp_err_t read_block(i2s_chan_handle_t rx, uint8_t *dest, size_t bytes)
{
    size_t got = 0;
    esp_err_t ret = i2s_channel_read(rx, dest, bytes, &got, 1000);
    if (ret != ESP_OK || got != bytes) {
        ESP_LOGE(TAG, "RX_FAIL ret=%s bytes=%u requested=%u", esp_err_to_name(ret), (unsigned)got, (unsigned)bytes);
        return ret == ESP_OK ? ESP_ERR_INVALID_SIZE : ret;
    }
    return ESP_OK;
}


/* frame[n] 占8字节、小端有符号16bit：buffer[4*n+0..3]依次为SLOT0..3。
 * 物理MIC1/MIC2/REF对应关系待实验确认，不在本函数预设。
 * 每次RX后立即拆到各槽独立缓冲；导出前不再从raw重建这些缓冲。 */
static void split_slots(const int16_t *raw, int16_t *planar, size_t frame_offset, size_t frames)
{
    for (size_t n = 0; n < frames; ++n) {
        for (unsigned slot = 0; slot < 4; ++slot) {
            planar[slot * (RAW_BYTES / 8) + frame_offset + n] = raw[4 * n + slot];
        }
    }
}

static void export_data(const char *phase, const char *kind, const uint8_t *data, int64_t start, int64_t end)
{
    uint32_t hash = 2166136261u;
    printf("\nRM_BEGIN %s %s 24000 4 %u %lld %lld\n", phase, kind, RAW_BYTES, (long long)start, (long long)end);
    for (size_t offset = 0; offset < RAW_BYTES; offset += 240) {
        uint8_t encoded[324]; size_t len = 0;
        mbedtls_base64_encode(encoded, sizeof(encoded), &len, data + offset, 240);
        for (unsigned i = 0; i < 240; ++i) hash = (hash ^ data[offset + i]) * 16777619u;
        printf("RM_DATA %s %s %u %.*s\n", phase, kind, (unsigned)offset, (int)len, encoded);
        vTaskDelay(1);
    }
    printf("RM_END %s %s %08" PRIx32 "\n", phase, kind, hash);
}

esp_err_t raw_adc_probe_run(const audio_codec_ctrl_if_t *ctrl, i2s_chan_handle_t rx, i2s_chan_handle_t tx)
{
    if (!ctrl || !rx || !tx) return ESP_ERR_INVALID_ARG;
    i2s_event_callbacks_t callbacks = {.on_recv_q_ovf = on_rx_overflow};
    esp_err_t setup = i2s_channel_disable(rx);
    if (setup != ESP_OK) return setup;
    setup = i2s_channel_register_event_callback(rx, &callbacks, NULL);
    esp_err_t enabled = i2s_channel_enable(rx);
    if (setup != ESP_OK) return setup;
    if (enabled != ESP_OK) return enabled;
    if (dump_registers(ctrl, "INIT") != ESP_OK) return ESP_FAIL;
    if (I2S0.rx_conf1.rx_bits_mod != 15 || I2S0.rx_conf1.rx_tdm_chan_bits != 15 ||
        I2S0.rx_tdm_ctrl.rx_tdm_tot_chan_num != 3 || (I2S0.rx_tdm_ctrl.val & 0xffff) != 15) {
        ESP_LOGE(TAG, "RX_FORMAT_UNEXPECTED，停止采样");
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t *capture = heap_caps_malloc(RAW_BYTES * 6, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!capture) return ESP_ERR_NO_MEM;
    uint8_t scratch[BLOCK_BYTES];
    int16_t tone[240];
    /* 24kHz下每24点一个周期，低幅度1kHz正弦；DAC音量由调用方固定为10。 */
    for (unsigned i = 0; i < 240; ++i) tone[i] = (int16_t)(4096 * sinf(2 * 3.14159265358979323846f * (i % 24) / 24));
    int64_t start[3], end[3];
    esp_err_t ret = ESP_OK;
    ESP_LOGI(TAG, "MAPPING_ONLY BASE unchanged: no ADC register writes, no MMR/resample/AFE/Opus");
    for (unsigned phase = 0; phase < 3; ++phase) {
        if ((ret = dump_registers(ctrl, names[phase])) != ESP_OK) break;
        ESP_LOGI(TAG, "PREPARE %s 10秒后录8秒：%s", names[phase], phase == 0 ?
                 "只靠近物理MIC1说话/轻敲，远离MIC2" : phase == 1 ?
                 "只靠近物理MIC2说话/轻敲，远离MIC1" : "保持安静，自动播放1kHz，请勿说话");
        int64_t until = esp_timer_get_time() + 10000000;
        while (esp_timer_get_time() < until && ret == ESP_OK) ret = read_block(rx, scratch, sizeof(scratch));
        if (ret != ESP_OK) break;
        ESP_LOGI(TAG, "RECORD_BEGIN %s duration=8s", names[phase]);
        uint32_t lost_before = __atomic_load_n(&rx_overflows, __ATOMIC_RELAXED);
        uint8_t *raw = capture + phase * RAW_BYTES * 2;
        int16_t *planar = (int16_t *)(raw + RAW_BYTES);
        start[phase] = esp_timer_get_time();
        for (size_t offset = 0; offset < RAW_BYTES; offset += BLOCK_BYTES) {
            if (phase == 2) {
                size_t written = 0;
                ret = i2s_channel_write(tx, tone, sizeof(tone), &written, 1000);
                if (ret != ESP_OK || written != sizeof(tone)) { ret = ESP_FAIL; break; }
            }
            ret = read_block(rx, raw + offset, BLOCK_BYTES);
            if (ret != ESP_OK) break;
            split_slots((const int16_t *)(raw + offset), planar, offset / 8, BLOCK_BYTES / 8);
        }
        end[phase] = esp_timer_get_time();
        uint32_t lost = __atomic_load_n(&rx_overflows, __ATOMIC_RELAXED) - lost_before;
        if (phase == 2) {
            /* 输出静音并等待已排队音频消费完毕，不重配I2S时钟或ADC。 */
            memset(tone, 0, sizeof(tone));
            for (unsigned i = 0; i < 20; ++i) {
                size_t written = 0;
                esp_err_t stop = i2s_channel_write(tx, tone, sizeof(tone), &written, 1000);
                if (stop != ESP_OK || written != sizeof(tone)) { ret = ESP_FAIL; break; }
            }
        }
        /* 溢出数在采样结束时读取，不计入后续停音阶段。 */
        ESP_LOGI(TAG, "RECORD_END %s elapsed_us=%lld overflow=%" PRIu32 " ret=%s", names[phase],
                 (long long)(end[phase] - start[phase]), lost, esp_err_to_name(ret));
        if (lost) ret = ESP_ERR_INVALID_STATE;
        if (ret != ESP_OK) break;
        unsigned mismatch = 0;
        for (unsigned n = 0; n < 1000; ++n) {
            for (unsigned slot = 0; slot < 4; ++slot) {
                int16_t sample;
                memcpy(&sample, raw + n * 8 + slot * 2, 2);
                mismatch += sample != planar[slot * (RAW_BYTES / 8) + n];
            }
        }
        ESP_LOGI(TAG, "LAYOUT_CHECK phase=%s consecutive_frames=1000 mismatch=%u", names[phase], mismatch);
        if (mismatch) { ret = ESP_FAIL; break; }
        for (unsigned slot = 0; slot < 4; ++slot) {
            uint64_t sum = 0; int32_t peak = 0;
            for (size_t n = 0; n < RAW_BYTES / 8; ++n) {
                int32_t v = planar[slot * (RAW_BYTES / 8) + n];
                sum += (int64_t)v * v;
                int32_t magnitude = v < 0 ? -v : v;
                if (magnitude > peak) peak = magnitude;
            }
            ESP_LOGI(TAG, "SLOT_STATS phase=%s slot%u_rms=%.3f slot%u_peak=%ld", names[phase], slot,
                     sqrt((double)sum / (RAW_BYTES / 8)), slot, (long)peak);
        }
    }
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "EXPORT_BEGIN 原始与固件拆槽数据独立导出，约20分钟");
        for (unsigned phase = 0; phase < 3; ++phase) {
            export_data(names[phase], "RAW", capture + phase * RAW_BYTES * 2, start[phase], end[phase]);
            export_data(names[phase], "FW", capture + (phase * 2 + 1) * RAW_BYTES, start[phase], end[phase]);
        }
        printf("RM_DONE\n");
    }
    free(capture);
    return ret;
}
