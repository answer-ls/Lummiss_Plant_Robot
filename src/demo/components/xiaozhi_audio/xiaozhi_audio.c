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
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "video_streamer.h"
#include "wake_word.h"

static const char *TAG = "XIAOZHI_AUDIO";

/* UI 组件由 main 注册为同一固件的一部分；弱符号避免音频组件反向依赖 main。 */
extern void expression_manager_post_state(const char *state) __attribute__((weak));
extern void expression_manager_post_emotion(const char *emotion) __attribute__((weak));

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

static volatile int s_output_volume = XIAOZHI_CODEC_OUTPUT_VOLUME;
/* 深度按"下行会被突发注入多少包"定，不是按播放节奏定。
 *
 * 未开启 ESP_WS_CLIENT_SEPARATE_TX_LOCK 时，收发共用 client->lock：上传一帧
 * H.264 期间下行 Opus 收不进来，锁释放后才连续回调，形成网络突发。当前已经
 * 开启独立 TX 锁，但仍保留较深队列，用于吸收公网和 ESP-Hosted 本身的抖动。
 * 原值 6 = 360 ms，实测第一次回答 40 帧里丢了 12 帧（30%）。
 * 24 = 1.44 s 突发余量；存储全在 PSRAM（每包 ≤1.4 KB），不占内部内存。
 * 队列真被打满时仍然是丢最旧、保最新的策略（见 incoming_audio_callback）。 */
#define XIAOZHI_PLAYBACK_QUEUE_DEPTH  24
#define XIAOZHI_MAX_OPUS_PACKET       1400
#define XIAOZHI_PCM_BUFFER_BYTES      4096
#define XIAOZHI_PCM_POOL_SIZE         3
#define XIAOZHI_PCM_QUEUE_DEPTH       2
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
#define XIAOZHI_DECODE_STACK_BYTES    20480
#define XIAOZHI_OUTPUT_STACK_BYTES    6144
#define XIAOZHI_SERVICE_STACK_BYTES   7168
#define XIAOZHI_WAKE_PROCESS_STACK_BYTES 40960
#define XIAOZHI_REPORT_MS             5000

/* I2S 输出留在 CPU0，并提高到视频上传任务之上，优先及时补充 DMA；Opus 解码
 * 移到 CPU1，与阻塞的 I2S 写入彻底分开。解码优先级略高于视频编解码，保证
 * 每 60 ms 准备好一帧 PCM，但仍低于 USB/UVC 的实时任务。 */
#define XIAOZHI_CAPTURE_CORE           0
#define XIAOZHI_CAPTURE_PRIORITY       6
#define XIAOZHI_DECODE_CORE            1
#define XIAOZHI_DECODE_PRIORITY        9
#define XIAOZHI_OUTPUT_CORE            0
#define XIAOZHI_OUTPUT_PRIORITY       10
#define XIAOZHI_SERVICE_PRIORITY       5
#define XIAOZHI_WAKE_PROCESS_CORE      1
#define XIAOZHI_WAKE_PROCESS_PRIORITY  8

#define XIAOZHI_WAKE_AUDIO_QUEUE_TIMEOUT_MS  500
#define XIAOZHI_WAKE_AUDIO_DRAIN_TIMEOUT_MS 5000

#define XIAOZHI_EVENT_CHAT_CONNECTED  BIT0
#define XIAOZHI_EVENT_DISCONNECTED    BIT1
#define XIAOZHI_EVENT_CHANNEL_ACTIVE  BIT2
#define XIAOZHI_EVENT_UPLINK_ENABLED  BIT3
#define XIAOZHI_EVENT_SPEAKING        BIT4
#define XIAOZHI_EVENT_WAKE_ACTIVE     BIT5

typedef struct {
    uint16_t size;
    uint8_t data[XIAOZHI_MAX_OPUS_PACKET];
} xiaozhi_opus_packet_t;

typedef struct {
    uint8_t *data;
    uint16_t size;
} xiaozhi_pcm_frame_t;

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
    uint8_t *playback_pcm[XIAOZHI_PCM_POOL_SIZE];
} xiaozhi_audio_context_t;

static xiaozhi_audio_context_t s_audio;
static EventGroupHandle_t s_events;
static QueueHandle_t s_playback_queue;
static StaticQueue_t s_playback_queue_control;
static uint8_t *s_playback_queue_storage;
static QueueHandle_t s_pcm_free_queue;
static QueueHandle_t s_pcm_ready_queue;
static StaticQueue_t s_pcm_free_queue_control;
static StaticQueue_t s_pcm_ready_queue_control;
static uint8_t s_pcm_free_queue_storage[
    XIAOZHI_PCM_POOL_SIZE * sizeof(uint8_t *)];
static uint8_t s_pcm_ready_queue_storage[
    XIAOZHI_PCM_QUEUE_DEPTH * sizeof(xiaozhi_pcm_frame_t)];
static TaskHandle_t s_service_task;
static TaskHandle_t s_capture_task;
static TaskHandle_t s_decode_task;
static TaskHandle_t s_output_task;
static TaskHandle_t s_wake_process_task;
static bool s_started;
static bool s_wake_word_ready;

static uint32_t s_capture_frames;
static uint32_t s_capture_bytes;
static uint32_t s_capture_errors;
static uint32_t s_playback_frames;
static uint32_t s_playback_drops;
/* drop 的两条来源必须分开看：qovf 是"队列被突发打满、丢最旧包"（丢帧率随
 * 网络/视频发送抖动，加深队列能治），derr 是"Opus 解码或 codec 写入失败"
 * （加深队列没用，得看包本身或 I2S）。合在一起时报 30% 丢帧根本没法定位。 */
static uint32_t s_playback_queue_overflow;
static uint32_t s_playback_errors;
static char s_session_id[80];

/* 下行语音诊断只记录计数和耗时，不改变播放路径。这里使用临界区保护，
 * 因为 WebSocket 回调和扬声器任务可能运行在不同 CPU。 */
typedef struct {
    uint64_t rx_bytes;
    uint64_t rx_gap_us_total;
    uint64_t decode_us_total;
    uint64_t write_us_total;
    uint64_t wait_us_total;
    uint32_t rx_packets;
    uint32_t rx_gap_samples;
    uint32_t decode_calls;
    uint32_t decode_errors;
    uint32_t write_calls;
    uint32_t write_errors;
    uint32_t wait_samples;
    uint32_t starvation_count;
    uint32_t output_late_count;
    uint32_t queue_peak;
    uint32_t pcm_queue_peak;
    uint32_t rx_gap_us_max;
    uint32_t decode_us_max;
    uint32_t write_us_max;
    uint32_t wait_us_max;
    uint32_t output_gap_us_max;
    int64_t last_rx_us;
    int64_t last_output_us;
    bool output_started;
    bool decode_in_flight;
    bool output_in_flight;
} xiaozhi_playback_stats_t;

static xiaozhi_playback_stats_t s_playback_stats;
static portMUX_TYPE s_playback_stats_lock = portMUX_INITIALIZER_UNLOCKED;

static uint32_t elapsed_us_clamped(int64_t start_us, int64_t end_us)
{
    const int64_t elapsed = end_us - start_us;
    return elapsed <= 0 ? 0U :
           elapsed > UINT32_MAX ? UINT32_MAX : (uint32_t)elapsed;
}

static void update_max_u32(uint32_t *maximum, uint32_t value)
{
    if (value > *maximum) {
        *maximum = value;
    }
}

static void reset_speech_timing_stats(void)
{
    const uint32_t queue_depth = s_playback_queue == NULL
        ? 0U : uxQueueMessagesWaiting(s_playback_queue);
    portENTER_CRITICAL(&s_playback_stats_lock);
    s_playback_stats.last_rx_us = 0;
    s_playback_stats.last_output_us = 0;
    s_playback_stats.output_started = false;
    s_playback_stats.queue_peak = queue_depth;
    s_playback_stats.pcm_queue_peak = 0;
    s_playback_stats.rx_gap_us_max = 0;
    s_playback_stats.decode_us_max = 0;
    s_playback_stats.write_us_max = 0;
    s_playback_stats.wait_us_max = 0;
    s_playback_stats.output_gap_us_max = 0;
    portEXIT_CRITICAL(&s_playback_stats_lock);
}

static bool playback_pipeline_empty(void)
{
    bool work_in_flight;
    portENTER_CRITICAL(&s_playback_stats_lock);
    work_in_flight = s_playback_stats.decode_in_flight ||
                     s_playback_stats.output_in_flight;
    portEXIT_CRITICAL(&s_playback_stats_lock);

    return !work_in_flight &&
           (s_playback_queue == NULL ||
            uxQueueMessagesWaiting(s_playback_queue) == 0) &&
           (s_pcm_ready_queue == NULL ||
            uxQueueMessagesWaiting(s_pcm_ready_queue) == 0);
}

/* 官方示例的自动对话模式：回答播放完成后继续监听，不要求再次说唤醒词。 */
static void resume_listening_after_playback(void)
{
    if (!s_wake_word_ready ||
        (xEventGroupGetBits(s_events) & XIAOZHI_EVENT_CHANNEL_ACTIVE) == 0 ||
        (xEventGroupGetBits(s_events) & XIAOZHI_EVENT_SPEAKING) != 0 ||
        !playback_pipeline_empty() ||
        (xEventGroupGetBits(s_events) & XIAOZHI_EVENT_UPLINK_ENABLED) != 0) {
        return;
    }

    char listen_start[192];
    snprintf(listen_start, sizeof(listen_start),
             "{\"session_id\":\"%s\",\"type\":\"listen\","
             "\"state\":\"start\",\"mode\":\"auto\"}",
             s_session_id);
    if (video_streamer_agent_send_text(listen_start) == ESP_OK) {
        xEventGroupSetBits(s_events, XIAOZHI_EVENT_UPLINK_ENABLED);
        ESP_LOGI(TAG, "回答播放完成，恢复连续对话监听");
    } else {
        ESP_LOGW(TAG, "恢复连续对话监听失败，重新等待唤醒词");
        wake_word_start();
        xEventGroupSetBits(s_events, XIAOZHI_EVENT_WAKE_ACTIVE);
    }
}

static void restore_uplink_after_playback(void)
{
    resume_listening_after_playback();
}

/* 与开发板示例一致：唤醒时先发送 AFE 保存的前置音频，再发送 detect/start。
 * 使用当前 Opus 编码器即可；此时实时上行尚未开启，不会与采集任务并发编码。 */
static void send_wake_preroll(void)
{
    const size_t available = wake_word_copy_preroll(NULL, 0);
    const size_t frame_samples =
        (size_t)s_audio.encoder_input_size / sizeof(int16_t);
    if (available < frame_samples || frame_samples == 0) {
        ESP_LOGW(TAG, "唤醒前音频不足，跳过前置音频发送");
        return;
    }

    int16_t *pcm = heap_caps_malloc(available * sizeof(int16_t),
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (pcm == NULL) {
        ESP_LOGW(TAG, "唤醒前音频复制缓冲分配失败，继续建立对话");
        return;
    }
    const size_t samples = wake_word_copy_preroll(pcm, available);
    esp_opus_enc_reset(s_audio.opus_encoder);

    size_t sent_packets = 0;
    for (size_t offset = 0; offset + frame_samples <= samples;
         offset += frame_samples) {
        esp_audio_enc_in_frame_t input = {
            .buffer = (uint8_t *)(pcm + offset),
            .len = (uint32_t)s_audio.encoder_input_size,
        };
        esp_audio_enc_out_frame_t output = {
            .buffer = s_audio.capture_opus,
            .len = (uint32_t)s_audio.encoder_output_size,
        };
        esp_audio_err_t error = esp_opus_enc_process(
            s_audio.opus_encoder, &input, &output);
        if (error != ESP_AUDIO_ERR_OK || output.encoded_bytes == 0 ||
            video_streamer_agent_send_audio_wait(
                s_audio.capture_opus, output.encoded_bytes,
                XIAOZHI_WAKE_AUDIO_QUEUE_TIMEOUT_MS) != ESP_OK) {
            ESP_LOGW(TAG, "唤醒前音频发送中断，已发送 %u 包",
                     (unsigned)sent_packets);
            break;
        }
        sent_packets++;
    }
    heap_caps_free(pcm);

    esp_err_t drain_error = video_streamer_agent_wait_audio_drain(
        XIAOZHI_WAKE_AUDIO_DRAIN_TIMEOUT_MS);
    if (drain_error != ESP_OK) {
        ESP_LOGW(TAG, "等待唤醒前音频发完超时: %s",
                 esp_err_to_name(drain_error));
    }
    ESP_LOGI(TAG, "唤醒前音频已处理：%u 包，约 %u ms",
             (unsigned)sent_packets,
             (unsigned)(sent_packets * XIAOZHI_FRAME_DURATION_MS));
}

static void process_detected_wake_word(const char *wake_word)
{
    if (s_events == NULL) {
        return;
    }
    if ((xEventGroupGetBits(s_events) & XIAOZHI_EVENT_CHANNEL_ACTIVE) == 0) {
        xEventGroupClearBits(s_events, XIAOZHI_EVENT_WAKE_ACTIVE);
        ESP_LOGW(TAG, "收到唤醒词但语音通道未激活，等待连接后重新检测: %s",
                 wake_word);
        return;
    }
    xEventGroupClearBits(s_events, XIAOZHI_EVENT_WAKE_ACTIVE);
    ESP_LOGI(TAG, "唤醒词已触发: %s", wake_word);
    if (expression_manager_post_state != NULL) {
        expression_manager_post_state("listen");
    }

    send_wake_preroll();

    char listen_detect[256];
    char listen_start[192];
    snprintf(listen_detect, sizeof(listen_detect),
             "{\"session_id\":\"%s\",\"type\":\"listen\","
             "\"state\":\"detect\",\"text\":\"%s\"}",
             s_session_id, wake_word);
    snprintf(listen_start, sizeof(listen_start),
             "{\"session_id\":\"%s\",\"type\":\"listen\","
             "\"state\":\"start\",\"mode\":\"auto\"}",
             s_session_id);

    if (video_streamer_agent_send_text(listen_detect) == ESP_OK &&
        video_streamer_agent_send_text(listen_start) == ESP_OK) {
        xEventGroupSetBits(s_events, XIAOZHI_EVENT_UPLINK_ENABLED);
        ESP_LOGI(TAG, "麦克风上行已开启");
    } else {
        ESP_LOGE(TAG, "发送唤醒事件失败，恢复等待唤醒词");
        if ((xEventGroupGetBits(s_events) &
             XIAOZHI_EVENT_CHANNEL_ACTIVE) != 0) {
            wake_word_start();
            xEventGroupSetBits(s_events, XIAOZHI_EVENT_WAKE_ACTIVE);
        }
    }
}

/* Opus/SILK 编码需要大栈。检测回调运行在只有 4 KB 栈的 AFE 任务中，
 * 因此回调只能通知本任务，不能直接编码唤醒前音频。 */
static void wake_process_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        process_detected_wake_word(wake_word_get_last());
    }
}

static void wake_word_detected_cb(const char *wake_word, void *user_data)
{
    (void)user_data;
    ESP_LOGI(TAG, "唤醒词事件已提交: %s", wake_word);
    if (s_wake_process_task != NULL) {
        xTaskNotifyGive(s_wake_process_task);
    }
}

static void stop_worker_tasks(void)
{
    /* 初始化中途失败时，必须先停止已启动的音频任务，再释放 codec/I2S。
     * 否则工作任务可能继续访问已经销毁的句柄。 */
    if (s_capture_task != NULL) {
        vTaskDeleteWithCaps(s_capture_task);
        s_capture_task = NULL;
    }
    if (s_decode_task != NULL) {
        vTaskDeleteWithCaps(s_decode_task);
        s_decode_task = NULL;
    }
    if (s_wake_process_task != NULL) {
        vTaskDeleteWithCaps(s_wake_process_task);
        s_wake_process_task = NULL;
    }
    if (s_output_task != NULL) {
        vTaskDeleteWithCaps(s_output_task);
        s_output_task = NULL;
    }
}

static void audio_hw_cleanup(void)
{
    heap_caps_free(s_audio.capture_pcm);
    heap_caps_free(s_audio.capture_raw);
    heap_caps_free(s_audio.capture_opus);
    s_audio.capture_pcm = NULL;
    s_audio.capture_raw = NULL;
    s_audio.capture_opus = NULL;
    for (size_t i = 0; i < XIAOZHI_PCM_POOL_SIZE; i++) {
        heap_caps_free(s_audio.playback_pcm[i]);
        s_audio.playback_pcm[i] = NULL;
    }

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
    s_output_volume = XIAOZHI_CODEC_OUTPUT_VOLUME;

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
    /* 采集缓冲继续使用内部内存。播放 PCM 放 PSRAM：IDF 的 i2s_channel_write()
     * 会 memcpy 到驱动自己的 DMA 描述符，不要求调用方缓冲本身具备 DMA 能力。
     * 这样新增双缓冲不会挤占日志中最低只剩约 1 KB 的 DMA 堆。 */
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
    for (size_t i = 0; i < XIAOZHI_PCM_POOL_SIZE; i++) {
        s_audio.playback_pcm[i] = heap_caps_malloc(
            XIAOZHI_PCM_BUFFER_BYTES,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (s_audio.capture_raw == NULL || s_audio.capture_pcm == NULL ||
        s_audio.capture_opus == NULL) {
        ESP_LOGE(TAG, "小智音频工作缓冲分配失败");
        return ESP_ERR_NO_MEM;
    }
    for (size_t i = 0; i < XIAOZHI_PCM_POOL_SIZE; i++) {
        if (s_audio.playback_pcm[i] == NULL) {
            ESP_LOGE(TAG, "小智 PCM 缓冲池分配失败：%u/%u",
                     (unsigned)i, XIAOZHI_PCM_POOL_SIZE);
            return ESP_ERR_NO_MEM;
        }
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
    static xiaozhi_playback_stats_t previous;
    const TickType_t now = xTaskGetTickCount();
    if ((now - last_report_tick) < pdMS_TO_TICKS(XIAOZHI_REPORT_MS)) {
        return;
    }
    last_report_tick = now;

    xiaozhi_playback_stats_t current;
    portENTER_CRITICAL(&s_playback_stats_lock);
    current = s_playback_stats;
    /* 峰值按 5 秒窗口重新开始，便于把卡顿时刻与 VIDEO 日志对齐。 */
    s_playback_stats.queue_peak = 0;
    s_playback_stats.pcm_queue_peak = 0;
    s_playback_stats.rx_gap_us_max = 0;
    s_playback_stats.decode_us_max = 0;
    s_playback_stats.write_us_max = 0;
    s_playback_stats.wait_us_max = 0;
    s_playback_stats.output_gap_us_max = 0;
    portEXIT_CRITICAL(&s_playback_stats_lock);

    const uint32_t rx_gap_samples =
        current.rx_gap_samples - previous.rx_gap_samples;
    const uint32_t decode_calls = current.decode_calls - previous.decode_calls;
    const uint32_t write_calls = current.write_calls - previous.write_calls;
    const uint32_t wait_samples = current.wait_samples - previous.wait_samples;
    const uint32_t rx_gap_avg_us = rx_gap_samples == 0 ? 0U : (uint32_t)(
        (current.rx_gap_us_total - previous.rx_gap_us_total) / rx_gap_samples);
    const uint32_t decode_avg_us = decode_calls == 0 ? 0U : (uint32_t)(
        (current.decode_us_total - previous.decode_us_total) / decode_calls);
    const uint32_t write_avg_us = write_calls == 0 ? 0U : (uint32_t)(
        (current.write_us_total - previous.write_us_total) / write_calls);
    const uint32_t wait_avg_us = wait_samples == 0 ? 0U : (uint32_t)(
        (current.wait_us_total - previous.wait_us_total) / wait_samples);

    ESP_LOGI(TAG,
             "MIC packets=%" PRIu32 " bytes=%" PRIu32 " peak=%" PRIu32
             " read_err=%" PRIu32 " | SPK frames=%" PRIu32 " drop=%" PRIu32
             " (qovf=%" PRIu32 " derr=%" PRIu32
             " queue=%u/%u)",
             s_capture_frames,
             s_capture_bytes,
             peak,
             s_capture_errors,
             s_playback_frames,
             s_playback_drops,
             s_playback_queue_overflow,
             s_playback_errors,
             s_playback_queue == NULL
                 ? 0U
                 : (unsigned)uxQueueMessagesWaiting(s_playback_queue),
             XIAOZHI_PLAYBACK_QUEUE_DEPTH);
    ESP_LOGI(TAG,
             "AUDIO_DIAG 5s: rx=%" PRIu32 " bytes=%" PRIu64
             " gap_avg/max=%" PRIu32 "/%" PRIu32 "us"
             " | opus_q=%u/%" PRIu32 " pcm_q=%u/%" PRIu32
             " | dec_avg/max=%" PRIu32 "/%" PRIu32 "us err=%" PRIu32
             " | i2s_avg/max=%" PRIu32 "/%" PRIu32 "us err=%" PRIu32
             " | wait_avg/max=%" PRIu32 "/%" PRIu32
             "us starve=%" PRIu32 " out_gap_max=%" PRIu32
             "us late=%" PRIu32,
             current.rx_packets - previous.rx_packets,
             current.rx_bytes - previous.rx_bytes,
             rx_gap_avg_us, current.rx_gap_us_max,
             s_playback_queue == NULL
                 ? 0U
                 : (unsigned)uxQueueMessagesWaiting(s_playback_queue),
             current.queue_peak,
             s_pcm_ready_queue == NULL
                 ? 0U
                 : (unsigned)uxQueueMessagesWaiting(s_pcm_ready_queue),
             current.pcm_queue_peak,
             decode_avg_us, current.decode_us_max,
             current.decode_errors - previous.decode_errors,
             write_avg_us, current.write_us_max,
             current.write_errors - previous.write_errors,
             wait_avg_us, current.wait_us_max,
             current.starvation_count - previous.starvation_count,
             current.output_gap_us_max,
             current.output_late_count - previous.output_late_count);
    previous = current;
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
        } else if ((bits & XIAOZHI_EVENT_CHANNEL_ACTIVE) &&
                   (bits & XIAOZHI_EVENT_WAKE_ACTIVE)) {
            wake_word_feed((const int16_t *)pcm,
                           (size_t)s_audio.encoder_input_size /
                           sizeof(int16_t));
        }
        report_audio_stats(peak);
    }
}

static void decode_task(void *arg)
{
    (void)arg;
    xiaozhi_opus_packet_t packet;
    for (;;) {
        if (xQueueReceive(s_playback_queue, &packet, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if ((xEventGroupGetBits(s_events) & XIAOZHI_EVENT_CHANNEL_ACTIVE) == 0) {
            continue;
        }

        /* 缓冲池有三个块：一个供解码器写入，最多两个在 PCM 就绪队列中。
         * 只有扬声器写完后才归还，保证解码器不会覆盖正在播放的数据。 */
        portENTER_CRITICAL(&s_playback_stats_lock);
        s_playback_stats.decode_in_flight = true;
        portEXIT_CRITICAL(&s_playback_stats_lock);

        uint8_t *pcm = NULL;
        if (xQueueReceive(s_pcm_free_queue, &pcm, portMAX_DELAY) != pdTRUE ||
            pcm == NULL) {
            portENTER_CRITICAL(&s_playback_stats_lock);
            s_playback_stats.decode_in_flight = false;
            portEXIT_CRITICAL(&s_playback_stats_lock);
            s_playback_drops++;
            s_playback_errors++;
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
        const int64_t decode_start_us = esp_timer_get_time();
        esp_audio_err_t audio_error = esp_opus_dec_decode(
            s_audio.opus_decoder, &input, &output, &info);
        const uint32_t decode_us = elapsed_us_clamped(
            decode_start_us, esp_timer_get_time());
        portENTER_CRITICAL(&s_playback_stats_lock);
        s_playback_stats.decode_calls++;
        s_playback_stats.decode_us_total += decode_us;
        update_max_u32(&s_playback_stats.decode_us_max, decode_us);
        if (audio_error != ESP_AUDIO_ERR_OK || output.decoded_size == 0) {
            s_playback_stats.decode_errors++;
            s_playback_stats.decode_in_flight = false;
            portEXIT_CRITICAL(&s_playback_stats_lock);
            xQueueSend(s_pcm_free_queue, &pcm, portMAX_DELAY);
            s_playback_drops++;
            s_playback_errors++;
            continue;
        }
        portEXIT_CRITICAL(&s_playback_stats_lock);

        xiaozhi_pcm_frame_t frame = {
            .data = pcm,
            .size = (uint16_t)output.decoded_size,
        };
        if (xQueueSend(s_pcm_ready_queue, &frame, portMAX_DELAY) != pdTRUE) {
            xQueueSend(s_pcm_free_queue, &pcm, portMAX_DELAY);
            s_playback_drops++;
            s_playback_errors++;
        }

        const uint32_t pcm_depth = uxQueueMessagesWaiting(s_pcm_ready_queue);
        portENTER_CRITICAL(&s_playback_stats_lock);
        update_max_u32(&s_playback_stats.pcm_queue_peak, pcm_depth);
        s_playback_stats.decode_in_flight = false;
        portEXIT_CRITICAL(&s_playback_stats_lock);
        restore_uplink_after_playback();
    }
}

static void output_task(void *arg)
{
    (void)arg;
    xiaozhi_pcm_frame_t frame;

    for (;;) {
        const int64_t wait_start_us = esp_timer_get_time();
        if (xQueueReceive(s_pcm_ready_queue, &frame, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        const uint32_t wait_us = elapsed_us_clamped(
            wait_start_us, esp_timer_get_time());

        portENTER_CRITICAL(&s_playback_stats_lock);
        s_playback_stats.output_in_flight = true;
        if (s_playback_stats.output_started) {
            s_playback_stats.wait_samples++;
            s_playback_stats.wait_us_total += wait_us;
            update_max_u32(&s_playback_stats.wait_us_max, wait_us);
            if (wait_us > (XIAOZHI_FRAME_DURATION_MS + 15U) * 1000U) {
                s_playback_stats.starvation_count++;
            }
        }
        portEXIT_CRITICAL(&s_playback_stats_lock);

        if ((xEventGroupGetBits(s_events) & XIAOZHI_EVENT_CHANNEL_ACTIVE) == 0) {
            xQueueSend(s_pcm_free_queue, &frame.data, portMAX_DELAY);
            portENTER_CRITICAL(&s_playback_stats_lock);
            s_playback_stats.output_in_flight = false;
            portEXIT_CRITICAL(&s_playback_stats_lock);
            continue;
        }

        const int64_t write_start_us = esp_timer_get_time();
        const int codec_result = esp_codec_dev_write(
            s_audio.codec_device, frame.data, frame.size);
        const int64_t write_end_us = esp_timer_get_time();
        const uint32_t write_us = elapsed_us_clamped(
            write_start_us, write_end_us);

        portENTER_CRITICAL(&s_playback_stats_lock);
        s_playback_stats.write_calls++;
        s_playback_stats.write_us_total += write_us;
        update_max_u32(&s_playback_stats.write_us_max, write_us);
        if (codec_result != ESP_CODEC_DEV_OK) {
            s_playback_stats.write_errors++;
            s_playback_stats.output_in_flight = false;
            portEXIT_CRITICAL(&s_playback_stats_lock);
            xQueueSend(s_pcm_free_queue, &frame.data, portMAX_DELAY);
            s_playback_drops++;
            s_playback_errors++;
            continue;
        }
        if (s_playback_stats.last_output_us != 0) {
            const uint32_t output_gap_us = elapsed_us_clamped(
                s_playback_stats.last_output_us, write_start_us);
            update_max_u32(&s_playback_stats.output_gap_us_max, output_gap_us);
            if (output_gap_us > (XIAOZHI_FRAME_DURATION_MS + 15U) * 1000U) {
                s_playback_stats.output_late_count++;
            }
        }
        s_playback_stats.last_output_us = write_start_us;
        s_playback_stats.output_started = true;
        s_playback_stats.output_in_flight = false;
        portEXIT_CRITICAL(&s_playback_stats_lock);
        s_playback_frames++;
        xQueueSend(s_pcm_free_queue, &frame.data, portMAX_DELAY);

        /* tts/stop 可能先于 I2S 播完最后几包到达。等队列真正排空后再恢复
         * 麦克风，避免把扬声器尾音重新上传给服务端。 */
        restore_uplink_after_playback();
    }
}

static void incoming_audio_callback(const uint8_t *data, size_t len, void *ctx)
{
    (void)ctx;
    if (data == NULL || len == 0 || len > XIAOZHI_MAX_OPUS_PACKET ||
        s_playback_queue == NULL) {
        s_playback_drops++;
        s_playback_errors++;
        return;
    }

    const int64_t now_us = esp_timer_get_time();
    portENTER_CRITICAL(&s_playback_stats_lock);
    s_playback_stats.rx_packets++;
    s_playback_stats.rx_bytes += len;
    if (s_playback_stats.last_rx_us != 0) {
        const uint32_t gap_us = elapsed_us_clamped(
            s_playback_stats.last_rx_us, now_us);
        s_playback_stats.rx_gap_samples++;
        s_playback_stats.rx_gap_us_total += gap_us;
        update_max_u32(&s_playback_stats.rx_gap_us_max, gap_us);
    }
    s_playback_stats.last_rx_us = now_us;
    portEXIT_CRITICAL(&s_playback_stats_lock);

    xiaozhi_opus_packet_t packet = {
        .size = (uint16_t)len,
    };
    memcpy(packet.data, data, len);
    if (xQueueSend(s_playback_queue, &packet, 0) != pdTRUE) {
        /* 扬声器来不及播放时丢掉最旧包，优先保持实时性。 */
        s_playback_queue_overflow++;
        xiaozhi_opus_packet_t oldest;
        if (xQueueReceive(s_playback_queue, &oldest, 0) == pdTRUE) {
            s_playback_drops++;
        }
        if (xQueueSend(s_playback_queue, &packet, 0) != pdTRUE) {
            s_playback_drops++;
        }
    }

    const uint32_t queue_depth = uxQueueMessagesWaiting(s_playback_queue);
    portENTER_CRITICAL(&s_playback_stats_lock);
    update_max_u32(&s_playback_stats.queue_peak, queue_depth);
    portEXIT_CRITICAL(&s_playback_stats_lock);
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
                             XIAOZHI_EVENT_SPEAKING |
                             XIAOZHI_EVENT_WAKE_ACTIVE);
        xEventGroupSetBits(s_events, XIAOZHI_EVENT_DISCONNECTED);
        wake_word_stop();
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

        xEventGroupSetBits(s_events, XIAOZHI_EVENT_CHANNEL_ACTIVE);

        if (s_wake_word_ready) {
            /* 不直接开启上行，先启动唤醒词检测。等用户说出唤醒词后再 send listen。 */
            wake_word_stop();
            wake_word_start();
            xEventGroupSetBits(s_events, XIAOZHI_EVENT_WAKE_ACTIVE);
            ESP_LOGI(TAG,
                     "服务端 Hello 已确认，session_id=%s，等待唤醒词（你好小智）",
                     s_session_id);
        } else {
            /* 与示例保持一致：模型不可用时保持静默，禁止把环境声音直接上传。 */
            xEventGroupClearBits(s_events, XIAOZHI_EVENT_UPLINK_ENABLED |
                                           XIAOZHI_EVENT_WAKE_ACTIVE);
            ESP_LOGE(TAG,
                     "服务端 Hello 已确认，但唤醒词不可用，麦克风上行保持关闭");
        }
        return;
    }
    if (strcmp(message_type, "tts") == 0) {
        char state[24] = {0};
        if (!json_string_value(text, "state", state, sizeof(state))) {
            return;
        }
        if (strcmp(state, "start") == 0) {
            reset_speech_timing_stats();
            xEventGroupClearBits(s_events, XIAOZHI_EVENT_UPLINK_ENABLED |
                                           XIAOZHI_EVENT_WAKE_ACTIVE);
            xEventGroupSetBits(s_events, XIAOZHI_EVENT_SPEAKING);
            wake_word_stop();
            ESP_LOGI(TAG, "小智开始回答");
        } else if (strcmp(state, "stop") == 0) {
            xEventGroupClearBits(s_events, XIAOZHI_EVENT_SPEAKING);
            if (expression_manager_post_state != NULL) {
                expression_manager_post_state("tts_stop");
            }
            if (s_wake_word_ready && playback_pipeline_empty()) {
                resume_listening_after_playback();
            } else if (s_wake_word_ready) {
                ESP_LOGI(TAG, "服务端回答结束，等待扬声器播放完剩余音频后恢复连续对话");
            }
        } else if (strcmp(state, "sentence_start") == 0) {
            ESP_LOGI(TAG, "小智回答文本：%.*s", (int)len, text);
        }
    } else if (strcmp(message_type, "listen") == 0) {
        char state[24] = {0};
        if (json_string_value(text, "state", state, sizeof(state)) &&
            strcmp(state, "stop") == 0) {
            if (expression_manager_post_state != NULL) {
                expression_manager_post_state("thinking");
            }
            xEventGroupClearBits(s_events, XIAOZHI_EVENT_UPLINK_ENABLED);
            if ((xEventGroupGetBits(s_events) &
                 XIAOZHI_EVENT_SPEAKING) == 0 && s_wake_word_ready) {
                wake_word_stop();
                wake_word_start();
                xEventGroupSetBits(s_events, XIAOZHI_EVENT_WAKE_ACTIVE);
                ESP_LOGI(TAG, "服务端结束连续监听，等待唤醒词");
            }
        }
    } else if (strcmp(message_type, "llm") == 0) {
        char emotion[20] = {0};
        if (json_string_value(text, "emotion", emotion, sizeof(emotion)) &&
            expression_manager_post_emotion != NULL) {
            expression_manager_post_emotion(emotion);
            ESP_LOGI(TAG, "收到情绪事件：%s", emotion);
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
    if (s_pcm_ready_queue != NULL) {
        vQueueDelete(s_pcm_ready_queue);
        s_pcm_ready_queue = NULL;
    }
    if (s_pcm_free_queue != NULL) {
        vQueueDelete(s_pcm_free_queue);
        s_pcm_free_queue = NULL;
    }
    heap_caps_free(s_playback_queue_storage);
    s_playback_queue_storage = NULL;
    wake_word_deinit();
    s_wake_word_ready = false;
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

    s_pcm_free_queue = xQueueCreateStatic(
        XIAOZHI_PCM_POOL_SIZE,
        sizeof(uint8_t *),
        s_pcm_free_queue_storage,
        &s_pcm_free_queue_control);
    s_pcm_ready_queue = xQueueCreateStatic(
        XIAOZHI_PCM_QUEUE_DEPTH,
        sizeof(xiaozhi_pcm_frame_t),
        s_pcm_ready_queue_storage,
        &s_pcm_ready_queue_control);
    if (s_pcm_free_queue == NULL || s_pcm_ready_queue == NULL) {
        ESP_LOGE(TAG, "创建小智 PCM 双缓冲队列失败");
        abort_audio_service();
        return;
    }
    for (size_t i = 0; i < XIAOZHI_PCM_POOL_SIZE; i++) {
        if (xQueueSend(s_pcm_free_queue,
                       &s_audio.playback_pcm[i], 0) != pdTRUE) {
            ESP_LOGE(TAG, "初始化小智 PCM 缓冲池失败");
            abort_audio_service();
            return;
        }
    }

    BaseType_t task_result = xTaskCreatePinnedToCoreWithCaps(
        output_task, "xiaozhi_spk", XIAOZHI_OUTPUT_STACK_BYTES, NULL,
        XIAOZHI_OUTPUT_PRIORITY, &s_output_task, XIAOZHI_OUTPUT_CORE,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (task_result == pdPASS) {
        task_result = xTaskCreatePinnedToCoreWithCaps(
            wake_process_task, "wake_process",
            XIAOZHI_WAKE_PROCESS_STACK_BYTES, NULL,
            XIAOZHI_WAKE_PROCESS_PRIORITY, &s_wake_process_task,
            XIAOZHI_WAKE_PROCESS_CORE,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (task_result == pdPASS) {
        task_result = xTaskCreatePinnedToCoreWithCaps(
            decode_task, "xiaozhi_dec", XIAOZHI_DECODE_STACK_BYTES, NULL,
            XIAOZHI_DECODE_PRIORITY, &s_decode_task, XIAOZHI_DECODE_CORE,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (task_result == pdPASS) {
        task_result = xTaskCreatePinnedToCoreWithCaps(
            capture_task, "xiaozhi_mic", XIAOZHI_CAPTURE_STACK_BYTES, NULL,
            XIAOZHI_CAPTURE_PRIORITY, &s_capture_task, XIAOZHI_CAPTURE_CORE,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (task_result != pdPASS) {
        ESP_LOGE(TAG, "创建小智音频工作任务失败");
        abort_audio_service();
        return;
    }

    ESP_LOGI(TAG,
             "小智音频流水线已就绪：MIC=CPU%d/P%d，DEC=CPU%d/P%d，"
             "SPK=CPU%d/P%d，WAKE=CPU%d/P%d，PCM=%u帧",
             XIAOZHI_CAPTURE_CORE, XIAOZHI_CAPTURE_PRIORITY,
             XIAOZHI_DECODE_CORE, XIAOZHI_DECODE_PRIORITY,
             XIAOZHI_OUTPUT_CORE, XIAOZHI_OUTPUT_PRIORITY,
             XIAOZHI_WAKE_PROCESS_CORE, XIAOZHI_WAKE_PROCESS_PRIORITY,
             XIAOZHI_PCM_QUEUE_DEPTH);

    /* 初始化已完成；收发由 MIC、SPK 和 WebSocket 任务负责，无需保留空转任务。 */
    s_service_task = NULL;
    vTaskDeleteWithCaps(NULL);
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

    /* 在主任务上下文中初始化唤醒词引擎——包含 Flash mmap，需要较大栈。
     * service_task 只有 7KB 栈，不够支撑 spi_flash_mmap 的缓存操作。      */
    s_wake_word_ready = false;
    if (wake_word_init(1) == ESP_OK) {
        wake_word_set_callback(wake_word_detected_cb, NULL);
        s_wake_word_ready = true;
        ESP_LOGI(TAG, "唤醒词引擎已就绪，等待 WebSocket 连接后启用");
    } else {
        ESP_LOGE(TAG, "唤醒词引擎初始化失败，语音上行将保持关闭");
    }

    s_started = true;
    BaseType_t result = xTaskCreatePinnedToCoreWithCaps(
        service_task,
        "xiaozhi_service",
        XIAOZHI_SERVICE_STACK_BYTES,
        NULL,
        XIAOZHI_SERVICE_PRIORITY,
        &s_service_task,
        XIAOZHI_CAPTURE_CORE,
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

bool xiaozhi_audio_is_wake_detected(void)
{
    return wake_word_is_detected();
}

const char *xiaozhi_audio_get_wake_word(void)
{
    return wake_word_get_last();
}

esp_err_t xiaozhi_audio_set_volume(int volume)
{
    if (volume < 0 || volume > 100) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_audio.codec_device == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (esp_codec_dev_set_out_vol(s_audio.codec_device, volume) !=
        ESP_CODEC_DEV_OK) {
        return ESP_FAIL;
    }
    s_output_volume = volume;
    ESP_LOGI(TAG, "扬声器音量已设置为 %d", volume);
    return ESP_OK;
}

int xiaozhi_audio_get_volume(void)
{
    return s_output_volume;
}
