#include "xiaozhi_audio.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio_codec_if.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_audio_enc.h"
#include "esp_audio_types.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_codec_dev_types.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_opus_dec.h"
#include "esp_opus_enc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "video_streamer.h"

static const char *TAG = "XIAOZHI_AUDIO";

/* 这些引脚来自厂商 guition-jc1060p470-y 小智示例。 */
#define XIAOZHI_I2C_PORT              I2C_NUM_1
#define XIAOZHI_I2C_SDA               GPIO_NUM_7
#define XIAOZHI_I2C_SCL               GPIO_NUM_8
#define XIAOZHI_I2S_PORT              I2S_NUM_0
#define XIAOZHI_I2S_MCLK              GPIO_NUM_13
#define XIAOZHI_I2S_BCLK              GPIO_NUM_12
#define XIAOZHI_I2S_WS                GPIO_NUM_10
#define XIAOZHI_I2S_DOUT              GPIO_NUM_9
#define XIAOZHI_I2S_DIN               GPIO_NUM_48
#define XIAOZHI_PA_ENABLE             GPIO_NUM_11

#define XIAOZHI_UPLINK_SAMPLE_RATE    16000
#define XIAOZHI_DOWNLINK_SAMPLE_RATE  24000
#define XIAOZHI_CODEC_SAMPLE_RATE     XIAOZHI_DOWNLINK_SAMPLE_RATE
#define XIAOZHI_FRAME_DURATION_MS     60
#define XIAOZHI_CODEC_INPUT_GAIN_DB   30.0f
#define XIAOZHI_CODEC_OUTPUT_VOLUME   60
#define XIAOZHI_PLAYBACK_QUEUE_DEPTH  6
#define XIAOZHI_MAX_OPUS_PACKET       1400
#define XIAOZHI_PCM_BUFFER_BYTES      4096
/* 栈需求由音频编解码组件自己给出：managed_components/espressif__esp_audio_codec/
 * README.md 的 Encoder/Decoder 两节结尾——"to support all encoders the running on
 * stack size should about 40k"、"To support all decoders, the task running the
 * decoder should have stack size of about 20K"。那里只统计堆，栈是另外算的。
 *
 * 原来只有一个 5120 的宏给 capture_task 和 playback_task 共用，于是上行门控一打
 * 开、第一次真正执行 esp_opus_enc_process()（SILK 编码 + VAD）就冲爆栈：实机 panic
 * 报 Stack protection fault，任务 "xiaozhi_mic"，现场
 * silk_encode_frame_FIX ← silk_Encode。此前不崩只是因为会话没建立时那段编码根本
 * 没执行过。
 *
 * 逐次试探的代价（5120 时越界 600 B，12288 时仍越界 1952 B）说明这条路径要 14 KB
 * 以上，所以直接按厂商给的数字定，不再一点点往上加。编码和解码需求差一倍，拆开。
 * 栈落在 PSRAM（创建时带 MALLOC_CAP_SPIRAM），合计约 60 KB，不占内部 RAM。 */
#define XIAOZHI_CAPTURE_STACK_BYTES   40960
#define XIAOZHI_PLAYBACK_STACK_BYTES  20480
#define XIAOZHI_SERVICE_STACK_BYTES   7168
#define XIAOZHI_REPORT_MS             5000

#define XIAOZHI_EVENT_CHAT_CONNECTED  BIT0
#define XIAOZHI_EVENT_DISCONNECTED    BIT1
#define XIAOZHI_EVENT_CHANNEL_ACTIVE  BIT2
#define XIAOZHI_EVENT_UPLINK_ENABLED  BIT3
#define XIAOZHI_EVENT_SPEAKING        BIT4

typedef struct {
    uint16_t size;
    uint8_t data[XIAOZHI_MAX_OPUS_PACKET];
} xiaozhi_opus_packet_t;

typedef struct {
    i2c_master_bus_handle_t i2c_bus;
    i2s_chan_handle_t tx_channel;
    i2s_chan_handle_t rx_channel;
    const audio_codec_data_if_t *data_if;
    const audio_codec_ctrl_if_t *ctrl_if;
    const audio_codec_gpio_if_t *gpio_if;
    const audio_codec_if_t *codec_if;
    esp_codec_dev_handle_t codec_device;
    void *opus_encoder;
    void *opus_decoder;
    int encoder_input_size;
    int encoder_output_size;
    uint8_t *capture_raw;
    uint8_t *capture_pcm;
    uint8_t *capture_opus;
    uint8_t *playback_pcm;
} xiaozhi_audio_context_t;

static xiaozhi_audio_context_t s_audio;
static EventGroupHandle_t s_events;
static QueueHandle_t s_playback_queue;
static StaticQueue_t s_playback_queue_control;
static uint8_t *s_playback_queue_storage;
static TaskHandle_t s_service_task;
static TaskHandle_t s_capture_task;
static TaskHandle_t s_playback_task;
static bool s_started;

static uint32_t s_capture_frames;
static uint32_t s_capture_bytes;
static uint32_t s_capture_errors;
static uint32_t s_playback_frames;
static uint32_t s_playback_drops;
static char s_session_id[80];

static void stop_worker_tasks(void)
{
    /* 初始化中途失败时，必须先停止已启动的音频任务，再释放 codec/I2S。
     * 否则工作任务可能继续访问已经销毁的句柄。 */
    if (s_capture_task != NULL) {
        vTaskDeleteWithCaps(s_capture_task);
        s_capture_task = NULL;
    }
    if (s_playback_task != NULL) {
        vTaskDeleteWithCaps(s_playback_task);
        s_playback_task = NULL;
    }
}

static void audio_hw_cleanup(void)
{
    heap_caps_free(s_audio.capture_pcm);
    heap_caps_free(s_audio.capture_raw);
    heap_caps_free(s_audio.capture_opus);
    heap_caps_free(s_audio.playback_pcm);
    s_audio.capture_pcm = NULL;
    s_audio.capture_raw = NULL;
    s_audio.capture_opus = NULL;
    s_audio.playback_pcm = NULL;

    if (s_audio.opus_encoder != NULL) {
        esp_opus_enc_close(s_audio.opus_encoder);
        s_audio.opus_encoder = NULL;
    }
    if (s_audio.opus_decoder != NULL) {
        esp_opus_dec_close(s_audio.opus_decoder);
        s_audio.opus_decoder = NULL;
    }
    if (s_audio.codec_device != NULL) {
        esp_codec_dev_close(s_audio.codec_device);
        esp_codec_dev_delete(s_audio.codec_device);
        s_audio.codec_device = NULL;
    }
    if (s_audio.codec_if != NULL) {
        audio_codec_delete_codec_if(s_audio.codec_if);
        s_audio.codec_if = NULL;
    }
    if (s_audio.ctrl_if != NULL) {
        audio_codec_delete_ctrl_if(s_audio.ctrl_if);
        s_audio.ctrl_if = NULL;
    }
    if (s_audio.gpio_if != NULL) {
        audio_codec_delete_gpio_if(s_audio.gpio_if);
        s_audio.gpio_if = NULL;
    }
    if (s_audio.data_if != NULL) {
        audio_codec_delete_data_if(s_audio.data_if);
        s_audio.data_if = NULL;
    }
    if (s_audio.tx_channel != NULL) {
        i2s_channel_disable(s_audio.tx_channel);
        i2s_del_channel(s_audio.tx_channel);
        s_audio.tx_channel = NULL;
    }
    if (s_audio.rx_channel != NULL) {
        i2s_channel_disable(s_audio.rx_channel);
        i2s_del_channel(s_audio.rx_channel);
        s_audio.rx_channel = NULL;
    }
    if (s_audio.i2c_bus != NULL) {
        i2c_del_master_bus(s_audio.i2c_bus);
        s_audio.i2c_bus = NULL;
    }
}

static esp_err_t audio_hw_init(void)
{
    esp_err_t ret = ESP_OK;
    i2c_master_bus_config_t i2c_config = {
        .i2c_port = XIAOZHI_I2C_PORT,
        .sda_io_num = XIAOZHI_I2C_SDA,
        .scl_io_num = XIAOZHI_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_GOTO_ON_ERROR(i2c_new_master_bus(&i2c_config, &s_audio.i2c_bus),
                      fail, TAG, "创建 ES8311 I2C 总线失败");

    i2s_chan_config_t channel_config =
        I2S_CHANNEL_DEFAULT_CONFIG(XIAOZHI_I2S_PORT, I2S_ROLE_MASTER);
    channel_config.dma_desc_num = 6;
    channel_config.dma_frame_num = 240;
    channel_config.auto_clear_after_cb = true;
    ESP_GOTO_ON_ERROR(i2s_new_channel(&channel_config,
                                      &s_audio.tx_channel,
                                      &s_audio.rx_channel),
                      fail, TAG, "创建 I2S 双工通道失败");

    /* ES8311 使用标准 Philips I2S，物理总线保持双声道时隙，codec_dev
     * 在上层只暴露一个麦克风/扬声器声道。 */
    i2s_std_config_t i2s_config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(XIAOZHI_CODEC_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = XIAOZHI_I2S_MCLK,
            .bclk = XIAOZHI_I2S_BCLK,
            .ws = XIAOZHI_I2S_WS,
            .dout = XIAOZHI_I2S_DOUT,
            .din = XIAOZHI_I2S_DIN,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    i2s_config.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    i2s_config.slot_cfg.slot_mask = I2S_STD_SLOT_BOTH;
    ESP_GOTO_ON_ERROR(i2s_channel_init_std_mode(s_audio.tx_channel, &i2s_config),
                      fail, TAG, "初始化 I2S 发送通道失败");
    ESP_GOTO_ON_ERROR(i2s_channel_init_std_mode(s_audio.rx_channel, &i2s_config),
                      fail, TAG, "初始化 I2S 接收通道失败");
    ESP_GOTO_ON_ERROR(i2s_channel_enable(s_audio.tx_channel),
                      fail, TAG, "启用 I2S 发送通道失败");
    ESP_GOTO_ON_ERROR(i2s_channel_enable(s_audio.rx_channel),
                      fail, TAG, "启用 I2S 接收通道失败");

    audio_codec_i2s_cfg_t data_config = {
        .port = XIAOZHI_I2S_PORT,
        .rx_handle = s_audio.rx_channel,
        .tx_handle = s_audio.tx_channel,
    };
    s_audio.data_if = audio_codec_new_i2s_data(&data_config);
    ESP_GOTO_ON_FALSE(s_audio.data_if != NULL, ESP_ERR_NO_MEM,
                      fail, TAG, "创建 codec I2S 数据接口失败");

    audio_codec_i2c_cfg_t control_config = {
        .port = XIAOZHI_I2C_PORT,
        .addr = ES8311_CODEC_DEFAULT_ADDR,
        .bus_handle = s_audio.i2c_bus,
    };
    s_audio.ctrl_if = audio_codec_new_i2c_ctrl(&control_config);
    s_audio.gpio_if = audio_codec_new_gpio();
    ESP_GOTO_ON_FALSE(s_audio.ctrl_if != NULL && s_audio.gpio_if != NULL,
                      ESP_ERR_NO_MEM, fail, TAG, "创建 ES8311 控制接口失败");

    es8311_codec_cfg_t codec_config = {
        .ctrl_if = s_audio.ctrl_if,
        .gpio_if = s_audio.gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH,
        .pa_pin = XIAOZHI_PA_ENABLE,
        .pa_reverted = false,
        .master_mode = false,
        .use_mclk = true,
        .digital_mic = false,
        .invert_mclk = false,
        .invert_sclk = false,
        .hw_gain = {
            .pa_voltage = 5.0f,
            .codec_dac_voltage = 3.3f,
        },
    };
    s_audio.codec_if = es8311_codec_new(&codec_config);
    ESP_GOTO_ON_FALSE(s_audio.codec_if != NULL, ESP_ERR_NOT_FOUND,
                      fail, TAG, "未检测到板载 ES8311");

    esp_codec_dev_cfg_t device_config = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN_OUT,
        .codec_if = s_audio.codec_if,
        .data_if = s_audio.data_if,
    };
    s_audio.codec_device = esp_codec_dev_new(&device_config);
    ESP_GOTO_ON_FALSE(s_audio.codec_device != NULL, ESP_ERR_NO_MEM,
                      fail, TAG, "创建 ES8311 设备失败");

    esp_codec_dev_sample_info_t sample_info = {
        .bits_per_sample = 16,
        .channel = 1,
        .channel_mask = 0,
        .sample_rate = XIAOZHI_CODEC_SAMPLE_RATE,
        .mclk_multiple = 0,
    };
    ESP_GOTO_ON_FALSE(esp_codec_dev_open(s_audio.codec_device, &sample_info) ==
                          ESP_CODEC_DEV_OK,
                      ESP_FAIL, fail, TAG, "打开 ES8311 失败");
    ESP_GOTO_ON_FALSE(esp_codec_dev_set_in_gain(
                          s_audio.codec_device,
                          XIAOZHI_CODEC_INPUT_GAIN_DB) == ESP_CODEC_DEV_OK,
                      ESP_FAIL, fail, TAG, "设置麦克风增益失败");
    ESP_GOTO_ON_FALSE(esp_codec_dev_set_out_vol(
                          s_audio.codec_device,
                          XIAOZHI_CODEC_OUTPUT_VOLUME) == ESP_CODEC_DEV_OK,
                      ESP_FAIL, fail, TAG, "设置扬声器音量失败");

    ESP_LOGI(TAG,
             "板载音频初始化完成：ES8311，I2S%d，24kHz/16bit/mono，MIC DIN=GPIO%d",
             XIAOZHI_I2S_PORT, XIAOZHI_I2S_DIN);
    return ESP_OK;

fail:
    audio_hw_cleanup();
    return ret;
}

static esp_err_t audio_codec_init(void)
{
    esp_opus_enc_config_t encoder_config = {
        .sample_rate = XIAOZHI_UPLINK_SAMPLE_RATE,
        .channel = ESP_AUDIO_MONO,
        .bits_per_sample = ESP_AUDIO_BIT16,
        .bitrate = ESP_OPUS_BITRATE_AUTO,
        .frame_duration = ESP_OPUS_ENC_FRAME_DURATION_60_MS,
        .application_mode = ESP_OPUS_ENC_APPLICATION_VOIP,
        .complexity = 0,
        .enable_fec = false,
        .enable_dtx = true,
        .enable_vbr = true,
    };
    esp_audio_err_t audio_error = esp_opus_enc_open(
        &encoder_config, sizeof(encoder_config), &s_audio.opus_encoder);
    if (audio_error != ESP_AUDIO_ERR_OK || s_audio.opus_encoder == NULL) {
        ESP_LOGE(TAG, "创建 Opus 编码器失败：%d", audio_error);
        return ESP_FAIL;
    }
    audio_error = esp_opus_enc_get_frame_size(
        s_audio.opus_encoder,
        &s_audio.encoder_input_size,
        &s_audio.encoder_output_size);
    if (audio_error != ESP_AUDIO_ERR_OK || s_audio.encoder_input_size <= 0 ||
        s_audio.encoder_output_size <= 0 ||
        s_audio.encoder_output_size > XIAOZHI_MAX_OPUS_PACKET) {
        ESP_LOGE(TAG, "Opus 帧大小非法：in=%d out=%d error=%d",
                 s_audio.encoder_input_size,
                 s_audio.encoder_output_size,
                 audio_error);
        return ESP_FAIL;
    }

    esp_opus_dec_cfg_t decoder_config = {
        .sample_rate = XIAOZHI_DOWNLINK_SAMPLE_RATE,
        .channel = ESP_AUDIO_MONO,
        .frame_duration = ESP_OPUS_DEC_FRAME_DURATION_60_MS,
        .self_delimited = false,
    };
    audio_error = esp_opus_dec_open(
        &decoder_config, sizeof(decoder_config), &s_audio.opus_decoder);
    if (audio_error != ESP_AUDIO_ERR_OK || s_audio.opus_decoder == NULL) {
        ESP_LOGE(TAG, "创建 Opus 解码器失败：%d", audio_error);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Opus 初始化完成：60ms，PCM=%d bytes，最大码流=%d bytes",
             s_audio.encoder_input_size, s_audio.encoder_output_size);
    return ESP_OK;
}

static esp_err_t audio_work_buffers_init(void)
{
    /* I2S DMA 直接读写的 PCM 必须放内部 DMA 内存；Opus 包也保持在内部 RAM，
     * 避免编解码库访问不支持 DMA 的地址。四个缓冲只在启动时申请一次。 */
    const size_t capture_raw_size =
        (size_t)s_audio.encoder_input_size *
        XIAOZHI_CODEC_SAMPLE_RATE / XIAOZHI_UPLINK_SAMPLE_RATE;
    s_audio.capture_raw = heap_caps_malloc(
        capture_raw_size,
        MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    s_audio.capture_pcm = heap_caps_malloc(
        (size_t)s_audio.encoder_input_size,
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    s_audio.capture_opus = heap_caps_malloc(
        (size_t)s_audio.encoder_output_size,
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    s_audio.playback_pcm = heap_caps_malloc(
        XIAOZHI_PCM_BUFFER_BYTES,
        MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    if (s_audio.capture_raw == NULL || s_audio.capture_pcm == NULL ||
        s_audio.capture_opus == NULL ||
        s_audio.playback_pcm == NULL) {
        ESP_LOGE(TAG, "小智音频工作缓冲分配失败");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static void resample_mic_24k_to_16k(const int16_t *input,
                                    int16_t *output,
                                    size_t output_samples)
{
    /* 24 kHz → 16 kHz 是固定 3:2 比例。每两个输出样本消耗三个输入
     * 样本；奇数位置取相邻两点平均，首版语音上行不引入额外 DSP 库。 */
    const size_t pairs = output_samples / 2U;
    for (size_t pair = 0; pair < pairs; pair++) {
        const size_t in_pos = pair * 3U;
        const size_t out_pos = pair * 2U;
        output[out_pos] = input[in_pos];
        output[out_pos + 1U] = (int16_t)(
            ((int32_t)input[in_pos + 1U] + input[in_pos + 2U]) / 2);
    }
}

static void report_audio_stats(uint32_t peak)
{
    static TickType_t last_report_tick;
    const TickType_t now = xTaskGetTickCount();
    if ((now - last_report_tick) < pdMS_TO_TICKS(XIAOZHI_REPORT_MS)) {
        return;
    }
    last_report_tick = now;
    ESP_LOGI(TAG,
             "MIC packets=%" PRIu32 " bytes=%" PRIu32 " peak=%" PRIu32
             " read_err=%" PRIu32 " | SPK frames=%" PRIu32 " drop=%" PRIu32,
             s_capture_frames,
             s_capture_bytes,
             peak,
             s_capture_errors,
             s_playback_frames,
             s_playback_drops);
}

static void capture_task(void *arg)
{
    (void)arg;
    uint8_t *raw = s_audio.capture_raw;
    uint8_t *pcm = s_audio.capture_pcm;
    uint8_t *opus = s_audio.capture_opus;
    const int raw_size = s_audio.encoder_input_size *
                         XIAOZHI_CODEC_SAMPLE_RATE /
                         XIAOZHI_UPLINK_SAMPLE_RATE;

    for (;;) {
        int codec_error = esp_codec_dev_read(
            s_audio.codec_device, raw, raw_size);
        if (codec_error != ESP_CODEC_DEV_OK) {
            s_capture_errors++;
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        uint32_t peak = 0;
        const int16_t *samples = (const int16_t *)raw;
        const size_t sample_count = (size_t)raw_size /
                                    sizeof(int16_t);
        for (size_t i = 0; i < sample_count; i++) {
            int32_t value = samples[i];
            uint32_t magnitude = (uint32_t)(value < 0 ? -value : value);
            if (magnitude > peak) {
                peak = magnitude;
            }
        }

        resample_mic_24k_to_16k(
            (const int16_t *)raw,
            (int16_t *)pcm,
            (size_t)s_audio.encoder_input_size / sizeof(int16_t));

        EventBits_t bits = xEventGroupGetBits(s_events);
        if ((bits & (XIAOZHI_EVENT_CHANNEL_ACTIVE |
                     XIAOZHI_EVENT_UPLINK_ENABLED)) ==
            (XIAOZHI_EVENT_CHANNEL_ACTIVE | XIAOZHI_EVENT_UPLINK_ENABLED)) {
            esp_audio_enc_in_frame_t input_frame = {
                .buffer = pcm,
                .len = (uint32_t)s_audio.encoder_input_size,
            };
            esp_audio_enc_out_frame_t output_frame = {
                .buffer = opus,
                .len = (uint32_t)s_audio.encoder_output_size,
            };
            esp_audio_err_t audio_error = esp_opus_enc_process(
                s_audio.opus_encoder, &input_frame, &output_frame);
            if (audio_error == ESP_AUDIO_ERR_OK && output_frame.encoded_bytes > 0) {
                esp_err_t send_error = video_streamer_agent_send_audio(
                    opus, output_frame.encoded_bytes);
                if (send_error == ESP_OK) {
                    s_capture_frames++;
                    s_capture_bytes += output_frame.encoded_bytes;
                } else if (send_error != ESP_ERR_INVALID_STATE) {
                    s_capture_errors++;
                }
            } else if (audio_error != ESP_AUDIO_ERR_OK) {
                s_capture_errors++;
            }
        }
        report_audio_stats(peak);
    }
}

static void playback_task(void *arg)
{
    (void)arg;
    uint8_t *pcm = s_audio.playback_pcm;

    xiaozhi_opus_packet_t packet;
    for (;;) {
        if (xQueueReceive(s_playback_queue, &packet, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if ((xEventGroupGetBits(s_events) & XIAOZHI_EVENT_CHANNEL_ACTIVE) == 0) {
            continue;
        }

        esp_audio_dec_in_raw_t input = {
            .buffer = packet.data,
            .len = packet.size,
        };
        esp_audio_dec_out_frame_t output = {
            .buffer = pcm,
            .len = XIAOZHI_PCM_BUFFER_BYTES,
        };
        esp_audio_dec_info_t info = {0};
        esp_audio_err_t audio_error = esp_opus_dec_decode(
            s_audio.opus_decoder, &input, &output, &info);
        if (audio_error != ESP_AUDIO_ERR_OK || output.decoded_size == 0) {
            s_playback_drops++;
            continue;
        }
        if (esp_codec_dev_write(s_audio.codec_device,
                                pcm,
                                (int)output.decoded_size) != ESP_CODEC_DEV_OK) {
            s_playback_drops++;
            continue;
        }
        s_playback_frames++;
    }
}

static void incoming_audio_callback(const uint8_t *data, size_t len, void *ctx)
{
    (void)ctx;
    if (data == NULL || len == 0 || len > XIAOZHI_MAX_OPUS_PACKET ||
        s_playback_queue == NULL) {
        s_playback_drops++;
        return;
    }

    xiaozhi_opus_packet_t packet = {
        .size = (uint16_t)len,
    };
    memcpy(packet.data, data, len);
    if (xQueueSend(s_playback_queue, &packet, 0) != pdTRUE) {
        /* 扬声器来不及播放时丢掉最旧包，优先保持实时性。 */
        xiaozhi_opus_packet_t oldest;
        if (xQueueReceive(s_playback_queue, &oldest, 0) == pdTRUE) {
            s_playback_drops++;
        }
        if (xQueueSend(s_playback_queue, &packet, 0) != pdTRUE) {
            s_playback_drops++;
        }
    }
}

static void server_connection_callback(bool connected, void *ctx)
{
    (void)ctx;
    if (connected) {
        xEventGroupClearBits(s_events, XIAOZHI_EVENT_DISCONNECTED);
        xEventGroupSetBits(s_events, XIAOZHI_EVENT_CHAT_CONNECTED);
        ESP_LOGI(TAG, "小智复用 Agent WebSocket，等待服务端 Hello");
    } else {
        xEventGroupClearBits(s_events,
                             XIAOZHI_EVENT_CHAT_CONNECTED |
                             XIAOZHI_EVENT_CHANNEL_ACTIVE |
                             XIAOZHI_EVENT_UPLINK_ENABLED |
                             XIAOZHI_EVENT_SPEAKING);
        xEventGroupSetBits(s_events, XIAOZHI_EVENT_DISCONNECTED);
        s_session_id[0] = '\0';
        if (s_playback_queue != NULL) {
            xQueueReset(s_playback_queue);
        }
        ESP_LOGW(TAG, "小智 Agent WebSocket 已断开");
    }
}

static int json_int_value(const char *json, const char *key, int fallback)
{
    char pattern[48];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *position = strstr(json, pattern);
    if (position == NULL) {
        return fallback;
    }
    position = strchr(position + strlen(pattern), ':');
    if (position == NULL) {
        return fallback;
    }
    position++;
    while (*position == ' ' || *position == '\t') {
        position++;
    }
    return atoi(position);
}

static bool json_string_value(const char *json,
                              const char *key,
                              char *output,
                              size_t output_size)
{
    char pattern[48];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *position = strstr(json, pattern);
    if (position == NULL || output == NULL || output_size == 0) {
        return false;
    }
    position = strchr(position + strlen(pattern), ':');
    if (position == NULL) {
        return false;
    }
    position++;
    while (*position == ' ' || *position == '\t') {
        position++;
    }
    if (*position++ != '\"') {
        return false;
    }
    const char *end = strchr(position, '\"');
    if (end == NULL || end == position ||
        (size_t)(end - position) >= output_size) {
        return false;
    }
    memcpy(output, position, (size_t)(end - position));
    output[end - position] = '\0';
    return true;
}

static void server_text_callback(const char *text, size_t len, void *ctx)
{
    (void)ctx;
    if (text == NULL || len == 0) {
        return;
    }
    if (strstr(text, "认证失败") != NULL) {
        ESP_LOGE(TAG, "小智鉴权失败，需要重新执行 OTA 获取动态 Token");
        return;
    }
    char message_type[16] = {0};
    if (!json_string_value(text, "type", message_type, sizeof(message_type))) {
        return;
    }
    if (strcmp(message_type, "hello") == 0) {
        /* 服务端 Hello 已确认当前下行 24 kHz/mono/60 ms。 */
        const int sample_rate = json_int_value(text, "sample_rate", 0);
        const int channels = json_int_value(text, "channels", 0);
        const int frame_duration = json_int_value(text, "frame_duration", 0);
        if (sample_rate != XIAOZHI_DOWNLINK_SAMPLE_RATE || channels != 1 ||
            frame_duration != XIAOZHI_FRAME_DURATION_MS) {
            ESP_LOGE(TAG,
                     "不支持服务端音频参数：rate=%d channel=%d frame=%dms",
                     sample_rate, channels, frame_duration);
            return;
        }
        if (!json_string_value(text, "session_id",
                               s_session_id, sizeof(s_session_id))) {
            ESP_LOGE(TAG, "服务端 Hello 缺少有效 session_id");
            return;
        }
        static const char listen_start[] =
            "{\"type\":\"listen\",\"state\":\"start\",\"mode\":\"auto\"}";
        if (video_streamer_agent_send_text(listen_start) == ESP_OK) {
            xEventGroupSetBits(s_events,
                               XIAOZHI_EVENT_CHANNEL_ACTIVE |
                               XIAOZHI_EVENT_UPLINK_ENABLED);
            ESP_LOGI(TAG,
                     "服务端 Hello 已确认，session_id=%s，麦克风开始 16 kHz Opus 上行",
                     s_session_id);
        }
        return;
    }
    if (strcmp(message_type, "tts") == 0) {
        char state[24] = {0};
        if (!json_string_value(text, "state", state, sizeof(state))) {
            return;
        }
        if (strcmp(state, "start") == 0) {
            xEventGroupClearBits(s_events, XIAOZHI_EVENT_UPLINK_ENABLED);
            xEventGroupSetBits(s_events, XIAOZHI_EVENT_SPEAKING);
            ESP_LOGI(TAG, "小智开始回答");
        } else if (strcmp(state, "stop") == 0) {
            xEventGroupClearBits(s_events, XIAOZHI_EVENT_SPEAKING);
            xEventGroupSetBits(s_events, XIAOZHI_EVENT_UPLINK_ENABLED);
            ESP_LOGI(TAG, "小智回答结束，恢复麦克风上行");
        } else if (strcmp(state, "sentence_start") == 0) {
            ESP_LOGI(TAG, "小智回答文本：%.*s", (int)len, text);
        }
    } else if (strcmp(message_type, "stt") == 0) {
        ESP_LOGI(TAG, "语音识别结果：%.*s", (int)len, text);
    }
}

static void abort_audio_service(void)
{
    /* 异步初始化失败时撤销所有对外回调，避免网络任务继续访问半初始化状态。 */
    video_streamer_set_agent_callbacks(NULL);
    stop_worker_tasks();
    if (s_playback_queue != NULL) {
        vQueueDelete(s_playback_queue);
        s_playback_queue = NULL;
    }
    heap_caps_free(s_playback_queue_storage);
    s_playback_queue_storage = NULL;
    audio_hw_cleanup();
    if (s_events != NULL) {
        vEventGroupDelete(s_events);
        s_events = NULL;
    }
    s_started = false;
    s_service_task = NULL;
    vTaskDeleteWithCaps(NULL);
}

static void service_task(void *arg)
{
    (void)arg;
    if (audio_hw_init() != ESP_OK || audio_codec_init() != ESP_OK ||
        audio_work_buffers_init() != ESP_OK) {
        ESP_LOGE(TAG, "板载音频初始化失败，小智服务停止");
        abort_audio_service();
        return;
    }

    s_playback_queue_storage = heap_caps_calloc(
        XIAOZHI_PLAYBACK_QUEUE_DEPTH,
        sizeof(xiaozhi_opus_packet_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_playback_queue_storage == NULL) {
        ESP_LOGE(TAG, "小智播放队列 PSRAM 分配失败");
        abort_audio_service();
        return;
    }
    s_playback_queue = xQueueCreateStatic(
        XIAOZHI_PLAYBACK_QUEUE_DEPTH,
        sizeof(xiaozhi_opus_packet_t),
        s_playback_queue_storage,
        &s_playback_queue_control);
    if (s_playback_queue == NULL) {
        ESP_LOGE(TAG, "创建小智播放队列失败");
        abort_audio_service();
        return;
    }

    BaseType_t task_result = xTaskCreatePinnedToCoreWithCaps(
        capture_task, "xiaozhi_mic", XIAOZHI_CAPTURE_STACK_BYTES, NULL, 6,
        &s_capture_task, 0, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (task_result == pdPASS) {
        task_result = xTaskCreatePinnedToCoreWithCaps(
            playback_task, "xiaozhi_spk", XIAOZHI_PLAYBACK_STACK_BYTES, NULL, 6,
            &s_playback_task, 0, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (task_result != pdPASS) {
        ESP_LOGE(TAG, "创建小智音频工作任务失败");
        abort_audio_service();
        return;
    }

    ESP_LOGI(TAG, "小智已注册到共享 Agent WebSocket，等待 OTA 鉴权连接");

    /* WebSocket 的断线和重连由 video_streamer 管理；音频任务只根据回调
     * 更新状态，不再创建第二条连接。 */
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

esp_err_t xiaozhi_audio_start(void)
{
    if (s_started) {
        return ESP_OK;
    }
    s_events = xEventGroupCreate();
    if (s_events == NULL) {
        return ESP_ERR_NO_MEM;
    }

    /* 先注册网络回调再创建音频服务任务，避免 WSS 建连很快时漏掉 Hello。 */
    const video_streamer_agent_callbacks_t callbacks = {
        .connection_changed = server_connection_callback,
        .text_received = server_text_callback,
        .audio_received = incoming_audio_callback,
        .ctx = NULL,
    };
    video_streamer_set_agent_callbacks(&callbacks);

    s_started = true;
    BaseType_t result = xTaskCreatePinnedToCoreWithCaps(
        service_task,
        "xiaozhi_service",
        XIAOZHI_SERVICE_STACK_BYTES,
        NULL,
        5,
        &s_service_task,
        0,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (result != pdPASS) {
        s_started = false;
        video_streamer_set_agent_callbacks(NULL);
        vEventGroupDelete(s_events);
        s_events = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

bool xiaozhi_audio_is_connected(void)
{
    return s_events != NULL &&
           (xEventGroupGetBits(s_events) & XIAOZHI_EVENT_CHAT_CONNECTED) != 0;
}

bool xiaozhi_audio_is_listening(void)
{
    return s_events != NULL &&
           (xEventGroupGetBits(s_events) & XIAOZHI_EVENT_UPLINK_ENABLED) != 0;
}

bool xiaozhi_audio_is_speaking(void)
{
    return s_events != NULL &&
           (xEventGroupGetBits(s_events) & XIAOZHI_EVENT_SPEAKING) != 0;
}
