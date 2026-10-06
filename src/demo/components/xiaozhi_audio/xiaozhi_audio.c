#include "mem_contig.h"
#include "xiaozhi_audio.h"

#include <inttypes.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio_codec_if.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/i2s_tdm.h"
#include "esp_audio_enc.h"
#include "esp_audio_types.h"
#include "esp_ae_rate_cvt.h"
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
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "video_streamer.h"

#if defined(CONFIG_CLOUD_PROTOCOL_V3)
/* V3：音频控制面在 MQTT（hello/会话参数），传输面在 UDP。
 * 旧协议下这两个头不参与编译。 */
#include "cloud_mqtt.h"
#include "cloud_udp.h"
#include "ambient_led.h"
#endif
#include "wake_word.h"
#include "audio_probe.h"
#include "opus_probe.h"
#include "lifecycle_probe.h"
#include "capture_diag.h"
#include "raw_adc_probe.h"
#include "board_pins.h"

static const char *TAG = "XIAOZHI_AUDIO";

/* UI 组件由 main 注册为同一固件的一部分；弱符号避免音频组件反向依赖 main。 */
extern void expression_manager_post_state(const char *state) __attribute__((weak));
extern void expression_manager_post_emotion(const char *emotion) __attribute__((weak));

/* I2C/I2S 板级引脚由 board_pins.h 按所选板型提供。 */
#define XIAOZHI_I2C_PORT              I2C_NUM_1
#define XIAOZHI_I2S_PORT              I2S_NUM_0

#define XIAOZHI_UPLINK_SAMPLE_RATE    16000
#define XIAOZHI_DOWNLINK_SAMPLE_RATE  24000
#define XIAOZHI_CODEC_SAMPLE_RATE     XIAOZHI_DOWNLINK_SAMPLE_RATE
#define XIAOZHI_FRAME_DURATION_MS     60
/* 采集节奏独立于 Opus 60ms 帧。双麦 AFE 每次需 64ms，60ms 读取会
 * 周期性积累到 120ms 才 feed；改成 10ms 读取后，feed 间隔为 60/70ms。 */
#define XIAOZHI_CAPTURE_SAMPLES_16K   160U
#define XIAOZHI_CAPTURE_INPUT_SAMPLES \
    (XIAOZHI_CAPTURE_SAMPLES_16K * XIAOZHI_CODEC_SAMPLE_RATE / XIAOZHI_UPLINK_SAMPLE_RATE)
#define XIAOZHI_SINGLE_MIC_TEST 0
/* 双麦对齐独立参考工程的三路 24dB；单麦对照保留原有 30dB 设置。 */
#if BOARD_AUDIO_HAS_ES7210 && !XIAOZHI_SINGLE_MIC_TEST
#define XIAOZHI_CODEC_INPUT_GAIN_DB   24.0f
#else
#define XIAOZHI_CODEC_INPUT_GAIN_DB   30.0f
#endif
/* 保留四槽物理帧；单麦测试向 AFE 输入 MIC2 和播放参考。 */
#if BOARD_AUDIO_HAS_ES7210
#define XIAOZHI_CAPTURE_CHANNELS 4U
#if XIAOZHI_SINGLE_MIC_TEST
#define XIAOZHI_AFE_CHANNELS     2U
#else
#define XIAOZHI_AFE_CHANNELS     3U
#endif
#else
#define XIAOZHI_CAPTURE_CHANNELS 1U
#define XIAOZHI_AFE_CHANNELS     1U
#endif
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
 * 原来由 capture_task 执行 Opus 时，5120 字节栈曾触发保护异常。
 * 现在 Opus 独立在 encoder_task，先保留采集任务原有栈预算，实机验证后再缩减。
 *
 * 逐次试探的代价（5120 时越界 600 B，12288 时仍越界 1952 B）说明这条路径要 14 KB
 * 以上，所以直接按厂商给的数字定，不再一点点往上加。编码和解码需求差一倍，拆开。
 * 工作任务栈落在 PSRAM（创建时带 MALLOC_CAP_SPIRAM），不占内部 RAM。 */
#define XIAOZHI_CAPTURE_STACK_BYTES   40960
#define XIAOZHI_ENCODER_STACK_BYTES   40960
#define XIAOZHI_DECODE_STACK_BYTES    20480
#define XIAOZHI_OUTPUT_STACK_BYTES    6144
#define XIAOZHI_SERVICE_STACK_BYTES   7168
#define XIAOZHI_WAKE_PROCESS_STACK_BYTES 40960
/* 控制/预录编码仍使用 PSRAM 大栈；模型映射与清理由独立 INTERNAL 小栈负责。 */
#define XIAOZHI_WAKE_PROCESS_STACK_CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define XIAOZHI_REPORT_MS             5000

/* I2S 输出留在 CPU0，并提高到视频上传任务之上，优先及时补充 DMA；Opus 解码
 * 移到 CPU1，与阻塞的 I2S 写入彻底分开。解码优先级略高于视频编解码，保证
 * 每 60 ms 准备好一帧 PCM，但仍低于 USB/UVC 的实时任务。 */
#define XIAOZHI_CAPTURE_CORE           0
#define XIAOZHI_CAPTURE_PRIORITY       6
#define XIAOZHI_ENCODER_PRIORITY       2
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
#define XIAOZHI_EVENT_RTC_ACK         BIT6

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
    const audio_codec_ctrl_if_t *input_ctrl_if;
    const audio_codec_gpio_if_t *gpio_if;
    const audio_codec_if_t *codec_if;
    const audio_codec_if_t *input_codec_if;
    esp_codec_dev_handle_t codec_device;
    esp_codec_dev_handle_t input_codec_device;
    void *opus_encoder;
    void *opus_decoder;
    int encoder_input_size;
    int encoder_output_size;
    uint8_t *capture_raw;
    uint8_t *capture_pcm;
    uint8_t *uplink_pcm;
    uint8_t *capture_opus;
    /* 每个 AFE 输入通道各自保留重采样相位，避免交错多通道相互串扰。 */
    esp_ae_rate_cvt_handle_t input_resamplers[XIAOZHI_AFE_CHANNELS];
    int16_t *resample_input;
    int16_t *resample_output;
    uint32_t resample_max_output_samples;
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
static TaskHandle_t s_encoder_task;
static TaskHandle_t s_decode_task;
static TaskHandle_t s_output_task;
static TaskHandle_t s_wake_process_task;
static bool s_started;
static bool s_hardware_test_started;
static TaskHandle_t s_hardware_test_mic_task;
static TaskHandle_t s_hardware_test_spk_task;
static int16_t *s_hardware_test_mic_buffer;
static int16_t *s_hardware_test_tone_buffer;
static bool s_wake_word_ready;
/* 只在 AFE 访问期间持有；RTC 销毁模型前须取得此锁。 */
static SemaphoreHandle_t s_capture_guard;
/* 预录与实时上行共用 Opus 编码器及输出缓冲。 */
static SemaphoreHandle_t s_encoder_guard;
static SemaphoreHandle_t s_rtc_request_guard;
static bool s_rtc_requested;
static bool s_rtc_pending;
static bool s_rtc_suspended;
static esp_err_t s_rtc_result;
static uint32_t s_afe_epoch;
static uint32_t s_uplink_pcm_frames, s_uplink_opus_packets, s_uplink_drop;

/* 采集任务持 capture_guard 累计，控制任务短暂取快照后解锁打印。 */
typedef struct {
    uint32_t clipped, input_samples, peak[3];
    /* 原始 24kHz 槽位统计，不改变送入 AFE 的 PCM。 */
    uint32_t raw_frames, nonzero_pairs, equal_pairs, double_pairs;
    int64_t sum[3], cross;
    uint64_t square[3];
} mic_diag_t;
static mic_diag_t s_mic_diag;

/* 旧开发板由 ES8311 同时采集/播放；新 PCB 的麦克风接独立 ES7210，
 * 因此所有采集入口统一经该 helper 取得正确的输入设备。 */
static esp_codec_dev_handle_t audio_input_device(void)
{
    return s_audio.input_codec_device != NULL
               ? s_audio.input_codec_device
               : s_audio.codec_device;
}

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

/* 仅诊断：短临界区保护跨任务计数，日志在临界区外输出；us为本次启动的绝对单调时间。 */
static portMUX_TYPE s_trace_lock = portMUX_INITIALIZER_UNLOCKED;
static char s_trace_session[80];
static unsigned s_trace_wake, s_trace_round, s_trace_live, s_trace_pre, s_trace_stt;
static void voice_trace(const char *event, const char *session, int ret)
{
    char sid[80];
    const int64_t us = esp_timer_get_time();
    bool print = true;
    portENTER_CRITICAL(&s_trace_lock);
    if (!strcmp(event, "WAKE_DETECTED")) {
        s_trace_wake++;
        s_trace_session[0] = 0;
        s_trace_round = s_trace_live = s_trace_pre = s_trace_stt = 0;
    }
    if (!strcmp(event, "SESSION_BOUND") && session)
        snprintf(s_trace_session, sizeof(s_trace_session), "%s", session);
    if (!strcmp(event, "LISTEN_START_BEGIN")) {
        s_trace_round++;
        s_trace_live = s_trace_stt = 0;
    }
    if (!strcmp(event, "PREROLL_PACKET_TX")) { if (ret == ESP_OK) s_trace_pre++; print = false; }
    if (!strcmp(event, "LIVE_OPUS_TX")) {
        if (ret == ESP_OK) { s_trace_live++; print = s_trace_live == 1; }
        else print = false;
        event = "FIRST_LIVE_OPUS_TX";
    }
    if (!strcmp(event, "STT_RX")) {
        /* 迟到/缺失session的STT单独标注，不污染当前轮首条统计。 */
        if (session && session[0] && !strcmp(session,s_trace_session)) {
            event = s_trace_stt++ == 0 ? "FIRST_STT_RX" : "STT_RX";
        } else event = "STT_RX_UNMATCHED";
    }
    snprintf(sid,sizeof(sid),"%s",session ? session : s_trace_session);
    const unsigned wake=s_trace_wake, round=s_trace_round, live=s_trace_live, pre=s_trace_pre;
    portEXIT_CRITICAL(&s_trace_lock);
    if (print) ESP_LOGI("VOICE_TRACE", "us=%lld session_id=%s wake=%u round=%u event=%s ret=%d preroll_packets=%u live_packets=%u",
                       (long long)us, sid[0]?sid:"PENDING",wake,round,event,ret,pre,live);
}


#if defined(CONFIG_CLOUD_PROTOCOL_V3)
/* 单个共享 AFE，WakeNet 与增强 PCM 输出分别门控。
 * 会话只由 wake_process 任务切换，VAD 不启动/结束监听、不筛掉静音帧。 */
typedef enum {
    VOICE_STATE_WAKE_IDLE = 0,
    VOICE_STATE_CONNECTING,
    VOICE_STATE_LISTENING,
    VOICE_STATE_SPEAKING,
    VOICE_STATE_CONTINUOUS_LISTENING,
} voice_state_t;

/* STT 只有一句话结束后才到达。10 秒看门狗可能在用户已经开口、但服务端尚未
 * 返回最终识别结果时结束会话；30 秒仍短于服务端约 120 秒的空闲期限。 */
#define VOICE_STATE_IDLE_TIMEOUT_MS 30000

static voice_state_t s_voice_state = VOICE_STATE_WAKE_IDLE;
/* 控制任务拥有会话时间戳，无符号毫秒差支持计数器回绕。 */
static uint32_t s_voice_state_entered_ms;
static uint32_t s_last_stt_ms;   /* 最近一次 STT 到达时间（连续监听看门狗用） */
static bool s_tts_finished;
static int64_t s_tts_stop_us;
typedef enum { VOICE_WAKE, VOICE_TTS_START, VOICE_TTS_STOP, VOICE_END, VOICE_STT } voice_event_type_t;
typedef struct {
    voice_event_type_t type;
    char session[80];
} voice_event_t;
static QueueHandle_t s_voice_events;
static bool s_voice_event_overflow;
static void wake_word_detected_cb(const char *word, void *ctx);

/* 网络/AFE 回调只投递事件；状态及会话统一由现有 wake_process 任务管理。 */
static void post_voice_event(voice_event_type_t type, const char *session)
{
    voice_event_t event = { .type = type };
    if (session) snprintf(event.session, sizeof(event.session), "%s", session);
    if (s_voice_events && xQueueSend(s_voice_events, &event, 0) != pdTRUE) {
        __atomic_store_n(&s_voice_event_overflow, true, __ATOMIC_RELEASE);
    }
}

static const char *voice_state_name(voice_state_t state)
{
    switch (state) {
    case VOICE_STATE_WAKE_IDLE:             return "WAKE_IDLE";
    case VOICE_STATE_CONNECTING:            return "CONNECTING";
    case VOICE_STATE_LISTENING:             return "LISTENING";
    case VOICE_STATE_SPEAKING:              return "SPEAKING";
    case VOICE_STATE_CONTINUOUS_LISTENING:  return "CONTINUOUS_LISTENING";
    default:                                return "UNKNOWN";
    }
}

static void set_voice_state(voice_state_t next)
{
    const voice_state_t old = s_voice_state;
    const bool listening = next == VOICE_STATE_LISTENING || next == VOICE_STATE_CONTINUOUS_LISTENING;
    if (listening && (!(xEventGroupGetBits(s_events) & XIAOZHI_EVENT_CHANNEL_ACTIVE) || s_rtc_suspended)) {
        ESP_LOGE(TAG, "语音通道未就绪，拒绝开启监听");
        return;
    }
    const bool waking = next == VOICE_STATE_WAKE_IDLE && s_wake_word_ready && !s_rtc_suspended;
    /* 先等编码/发送结束，再切换上行门控，避免旧会话音频跨轮发送。 */
    xSemaphoreTake(s_encoder_guard, portMAX_DELAY);
    xSemaphoreTake(s_capture_guard, portMAX_DELAY);
    xEventGroupClearBits(s_events, XIAOZHI_EVENT_UPLINK_ENABLED | XIAOZHI_EVENT_WAKE_ACTIVE);
    wake_word_enable_voice_processing(listening);
    if (waking) {
        wake_word_start();
        xEventGroupSetBits(s_events, XIAOZHI_EVENT_WAKE_ACTIVE);
    } else {
        wake_word_stop();
    }
    if (listening) {
        xEventGroupSetBits(s_events, XIAOZHI_EVENT_UPLINK_ENABLED);
        voice_trace("MIC_UPLINK_ENABLE", NULL, ESP_OK);
    }
    if (next == VOICE_STATE_SPEAKING && old != next) voice_trace("SPEAKING_ENTER", NULL, ESP_OK);
    const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    const uint32_t idle_ms = now_ms - s_voice_state_entered_ms;
    s_voice_state = next;
    if (old != next) {
        lifecycle_mark(voice_state_name(next));
        capture_diag_state(next == VOICE_STATE_CONTINUOUS_LISTENING);
    }
    s_voice_state_entered_ms = now_ms;
    const EventBits_t bits = xEventGroupGetBits(s_events);
    xSemaphoreGive(s_capture_guard);
    xSemaphoreGive(s_encoder_guard);
    ESP_LOGI("VOICE_STATE", "%s -> %s wake_enabled=%d voice_processing_enabled=%d mic_uplink=%d vad_state=%s session_ready=%d idle_ms=%u",
             voice_state_name(old), voice_state_name(next),
             (bits & XIAOZHI_EVENT_WAKE_ACTIVE) ? 1 : 0,
             listening,
             (bits & XIAOZHI_EVENT_UPLINK_ENABLED) ? 1 : 0,
             wake_word_voice_detected() ? "SPEECH" : "SILENCE",
             (bits & XIAOZHI_EVENT_CHANNEL_ACTIVE) != 0,
             (unsigned)idle_ms);

    /* 语音状态只更新灯效控制层，RMT 刷新仍由 ambient_led 专用任务完成。 */
    switch (next) {
    case VOICE_STATE_LISTENING:
    case VOICE_STATE_CONTINUOUS_LISTENING:
        ambient_led_set_state(AMBIENT_LED_STATE_LISTENING);
        break;
    case VOICE_STATE_SPEAKING:
        ambient_led_set_state(AMBIENT_LED_STATE_SPEAKING);
        break;
    case VOICE_STATE_WAKE_IDLE:
    default:
        ambient_led_set_state(AMBIENT_LED_STATE_WAKE_IDLE);
        break;
    }
}

/* 定义在 wake 处理区（process_detected_wake_word 之前） */
static void enter_wake_idle(void);
#endif /* CONFIG_CLOUD_PROTOCOL_V3 */

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

/* 下行控制文本（listen / abort 等 JSON）的发送通道。
 * V3 走 MQTT 的 publish_topic；旧协议走 Agent WebSocket 的 text 帧。
 * 不做封装的话，V3 下这些调用全部落在已停用的 WebSocket 上，
 * 表现为「唤醒词识别到了但服务端毫无反应」。 */
static esp_err_t send_agent_text(const char *text)
{
    const bool start = strstr(text, "\"state\":\"start\"") != NULL;
    const bool detect = strstr(text, "\"state\":\"detect\"") != NULL;
    if (start) voice_trace("LISTEN_START_BEGIN", NULL, ESP_OK);
#if defined(CONFIG_CLOUD_PROTOCOL_V3)
    esp_err_t ret = cloud_mqtt_publish_text(text);
#else
    esp_err_t ret = video_streamer_agent_send_text(text);
#endif
    /* TX成功只代表本地发布成功，不代表服务端接收确认。 */
    if (start || detect) voice_trace(start ? "LISTEN_START_TX" : "LISTEN_DETECT_TX", NULL, ret);
    return ret;
}

/* 官方示例的自动对话模式：回答播放完成后继续监听，不要求再次说唤醒词。 */
static void resume_listening_after_playback(void)
{
#if defined(CONFIG_CLOUD_PROTOCOL_V3)
    if (s_voice_state != VOICE_STATE_SPEAKING || !s_tts_finished || s_rtc_suspended) return;
    int64_t last_audio_us;
    portENTER_CRITICAL(&s_playback_stats_lock);
    last_audio_us = s_playback_stats.last_output_us > s_playback_stats.last_rx_us
                        ? s_playback_stats.last_output_us : s_playback_stats.last_rx_us;
    portEXIT_CRITICAL(&s_playback_stats_lock);
    /* I2S 六个 240-sample 描述符最多缓存 60ms；保留 120ms 排空余量，
     * 也避免 MQTT stop 比最后的 UDP 音频包先到时过早开启麦克风。 */
    const int64_t now_us = esp_timer_get_time();
    if (now_us - last_audio_us < 120000 || now_us - s_tts_stop_us < 120000) return;
#else
    if (xEventGroupGetBits(s_events) & (XIAOZHI_EVENT_SPEAKING | XIAOZHI_EVENT_UPLINK_ENABLED)) return;
#endif
    if (!s_wake_word_ready || !playback_pipeline_empty() ||
        !(xEventGroupGetBits(s_events) & XIAOZHI_EVENT_CHANNEL_ACTIVE)) return;

    lifecycle_mark("POST_TTS");
    char listen_start[192];
    snprintf(listen_start, sizeof(listen_start),
             "{\"session_id\":\"%s\",\"type\":\"listen\","
             "\"state\":\"start\",\"mode\":\"auto\"}",
             s_session_id);
    if (send_agent_text(listen_start) == ESP_OK) {
        /* MQTT QoS0 仅说明本地发布调用成功，不能据此推断服务端已开始 ASR。 */
        ESP_LOGI(TAG, "回答播放完成，listen/start 已本地发布（QoS0 无服务端确认），恢复连续对话监听");
#if defined(CONFIG_CLOUD_PROTOCOL_V3)
        set_voice_state(VOICE_STATE_CONTINUOUS_LISTENING);
        /* 每次开机仅抓首次续听中实际发送成功的 Opus 包。 */
        opus_probe_start();
#else
        xEventGroupSetBits(s_events, XIAOZHI_EVENT_UPLINK_ENABLED);
#endif
    } else {
        ESP_LOGW(TAG, "恢复连续对话监听失败，重新等待唤醒词");
#if defined(CONFIG_CLOUD_PROTOCOL_V3)
        enter_wake_idle();
#else
        wake_word_start();
        xEventGroupSetBits(s_events, XIAOZHI_EVENT_WAKE_ACTIVE);
#endif
    }
}

static void restore_uplink_after_playback(void)
{
#if !defined(CONFIG_CLOUD_PROTOCOL_V3)
    resume_listening_after_playback();
#endif
    /* V3 由语音控制任务统一确认 TTS stop 和实际播放排空。 */
}

/* 与开发板示例一致：唤醒时先发送 AFE 保存的前置音频，再发送 detect/start。
 * 与实时编码任务共用编码器，由 encoder_guard 保证预录顺序。 */
static void send_wake_preroll(void)
{
    /* 预录和实时上行共用 Opus 编码器，整段预录必须串行发送。 */
    xSemaphoreTake(s_encoder_guard, portMAX_DELAY);
    voice_trace("PREROLL_SEND_BEGIN", NULL, ESP_OK);
    const size_t available = wake_word_copy_preroll(NULL, 0);
    const size_t frame_samples =
        (size_t)s_audio.encoder_input_size / sizeof(int16_t);
    if (available < frame_samples || frame_samples == 0) {
        voice_trace("PREROLL_SEND_END", NULL, ESP_ERR_INVALID_SIZE);
        ESP_LOGW(TAG, "唤醒前音频不足，跳过前置音频发送");
        xSemaphoreGive(s_encoder_guard);
        return;
    }

    int16_t *pcm = heap_caps_malloc(available * sizeof(int16_t),
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (pcm == NULL) {
        voice_trace("PREROLL_SEND_END", NULL, ESP_ERR_NO_MEM);
        ESP_LOGW(TAG, "唤醒前音频复制缓冲分配失败，继续建立对话");
        xSemaphoreGive(s_encoder_guard);
        return;
    }
    const size_t samples = wake_word_copy_preroll(pcm, available);
    esp_opus_enc_reset(s_audio.opus_encoder);

    size_t sent_packets = 0;
    esp_err_t preroll_result = ESP_OK;
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
#if defined(CONFIG_CLOUD_PROTOCOL_V3)
        /* V3：前置音频与实时上行走同一条 UDP 通道。
         * 旧的 send_audio_wait 落在已停用的 WebSocket 上，永远失败 ——
         * 实测表现为"唤醒前音频发送中断，已发送 0 包"。 */
        esp_err_t send_ok = ESP_OK;
        if (error == ESP_AUDIO_ERR_OK && output.encoded_bytes > 0) {
            send_ok = cloud_udp_send_opus(s_audio.capture_opus,
                                          output.encoded_bytes);
        }
#else
        const esp_err_t send_ok = video_streamer_agent_send_audio_wait(
            s_audio.capture_opus, output.encoded_bytes,
            XIAOZHI_WAKE_AUDIO_QUEUE_TIMEOUT_MS);
#endif
        if (error != ESP_AUDIO_ERR_OK || output.encoded_bytes == 0 ||
            send_ok != ESP_OK) {
            preroll_result = send_ok != ESP_OK ? send_ok : ESP_FAIL;
            ESP_LOGW(TAG, "唤醒前音频发送中断，已发送 %u 包",
                     (unsigned)sent_packets);
            break;
        }
        voice_trace("PREROLL_PACKET_TX", NULL, ESP_OK);
        sent_packets++;
    }
    heap_caps_free(pcm);
    voice_trace("PREROLL_SEND_END", NULL, preroll_result);

#if !defined(CONFIG_CLOUD_PROTOCOL_V3)
    /* 旧版 WebSocket 上行带发送队列，detect/start 前必须等队列排空。 */
    esp_err_t drain_error = video_streamer_agent_wait_audio_drain(
        XIAOZHI_WAKE_AUDIO_DRAIN_TIMEOUT_MS);
    if (drain_error != ESP_OK) {
        ESP_LOGW(TAG, "等待唤醒前音频发完超时: %s",
                 esp_err_to_name(drain_error));
    }
#else
    /* V3 的 UDP sendto 已同步完成；不能再等待已停用的 WebSocket 队列。 */
#endif
    ESP_LOGI(TAG, "唤醒前音频已处理：%u 包，约 %u ms",
             (unsigned)sent_packets,
             (unsigned)(sent_packets * XIAOZHI_FRAME_DURATION_MS));
    xSemaphoreGive(s_encoder_guard);
}

#if defined(CONFIG_CLOUD_PROTOCOL_V3)
/* 前向声明：两个回调的执行体在本文件后部，V3 会话打开时要注册它们 */
static void incoming_audio_callback(const uint8_t *data, size_t len, void *ctx);
static void server_text_callback(const char *text, size_t len, void *ctx);

/* 退回 WAKE_IDLE：停上行、停 UDP 通道（旧会话作废）、重启唤醒词喂音。幂等。 */
static void enter_wake_idle(void)
{
    xSemaphoreTake(s_encoder_guard, portMAX_DELAY);
    xSemaphoreTake(s_capture_guard, portMAX_DELAY);
    xEventGroupClearBits(s_events, XIAOZHI_EVENT_UPLINK_ENABLED |
                                  XIAOZHI_EVENT_SPEAKING | XIAOZHI_EVENT_CHANNEL_ACTIVE);
    wake_word_enable_voice_processing(false);
    xSemaphoreGive(s_capture_guard);
    cloud_udp_stop();
    xSemaphoreGive(s_encoder_guard);
    s_session_id[0] = '\0';
    s_tts_finished = false;
    s_last_stt_ms = 0;
    if (s_playback_queue) xQueueReset(s_playback_queue);
    /* PCM 由播放任务归还原池，不能 reset 指针队列导致缓冲丢失。 */
    set_voice_state(VOICE_STATE_WAKE_IDLE);
}

/* 唤醒时重新协商会话：重新发布 hello v3 并等待新的 Server Hello
 * （新 session_id + 新 UDP key），随后把 UDP 通道切换到新会话。
 * 在 wake_process_task 里阻塞等待是安全的（该任务只处理唤醒事件）。
 * 返回 true = 新会话与 UDP 通道就绪。 */
static bool v3_open_session_for_wake(void)
{
    cloud_udp_stop();
    xEventGroupClearBits(s_events, XIAOZHI_EVENT_CHANNEL_ACTIVE);
    if (cloud_mqtt_reopen_session() != ESP_OK) {
        ESP_LOGE(TAG, "重新发布 hello 失败");
        return false;
    }
    /* 最多等 3 s：hello 经 MQTT 往返，实测几百毫秒。 */
    for (int i = 0; i < 150; i++) {
        if (__atomic_load_n(&s_rtc_requested, __ATOMIC_ACQUIRE)) return false;
        if (cloud_mqtt_is_session_ready()) {
            cloud_mqtt_session_t session;
            if (!cloud_mqtt_get_session(&session)) {
                return false;
            }
            cloud_udp_set_audio_callback(incoming_audio_callback, NULL);
            cloud_mqtt_set_text_callback(server_text_callback, NULL);
            if (cloud_udp_start(&session) != ESP_OK) {
                ESP_LOGE(TAG, "UDP 音频通道启动失败");
                return false;
            }
            snprintf(s_session_id, sizeof(s_session_id), "%s",
                     session.session_id);
            xEventGroupSetBits(s_events, XIAOZHI_EVENT_CHANNEL_ACTIVE);
            voice_trace("SESSION_BOUND", s_session_id, ESP_OK);
            ESP_LOGI(TAG, "新会话已就绪：%s", s_session_id);
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    ESP_LOGE(TAG, "等待 Server Hello 超时（3 s）");
    return false;
}
#endif /* CONFIG_CLOUD_PROTOCOL_V3 */

static void process_detected_wake_word(const char *wake_word)
{
    if (s_events == NULL) {
        return;
    }
#if defined(CONFIG_CLOUD_PROTOCOL_V3)
    /* V3：一次唤醒 = 重新 hello = 全新会话。服务端空闲超时（实测 ~120 s）
     * 会结束旧会话，旧 UDP key 发过去无人处理 —— 固件必须重新协商，
     * 否则第二次唤醒后服务端再也不回话。 */
    if (s_voice_state != VOICE_STATE_WAKE_IDLE || s_rtc_suspended || !s_wake_word_ready) return;
    set_voice_state(VOICE_STATE_CONNECTING);
    for (int i = 0; !playback_pipeline_empty(); ++i) {
        if (i >= 200 || __atomic_load_n(&s_rtc_requested, __ATOMIC_ACQUIRE)) {
            enter_wake_idle();
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    ESP_LOGI(TAG, "唤醒词已触发: %s", wake_word);
    if (expression_manager_post_state != NULL) {
        expression_manager_post_state("listen");
    }
    if (!v3_open_session_for_wake()) {
        ESP_LOGE(TAG, "重建 AI 会话失败，回到唤醒待机");
        enter_wake_idle();
        return;
    }
    /* 此时实时上行仍关闭，独占 Opus 编码器发送已冻结的唤醒前置音频。 */
    send_wake_preroll();
    {
        /* 局部块：避免与下方旧协议回滚路径的同名变量冲突 */
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
        if (send_agent_text(listen_detect) == ESP_OK &&
            send_agent_text(listen_start) == ESP_OK) {
            set_voice_state(VOICE_STATE_LISTENING);
            ESP_LOGI(TAG, "麦克风上行已开启");
        } else {
            ESP_LOGE(TAG, "发送唤醒事件失败，恢复等待唤醒词");
            enter_wake_idle();
        }
    }
    return;
#endif
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

    if (send_agent_text(listen_detect) == ESP_OK &&
        send_agent_text(listen_start) == ESP_OK) {
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
#if defined(CONFIG_CLOUD_PROTOCOL_V3)
static void handle_voice_event(const voice_event_t *incoming)
{
    if (s_rtc_suspended) return;
    const voice_event_t event = *incoming;
    if (event.type == VOICE_WAKE) {
        process_detected_wake_word(wake_word_get_last());
    } else if (s_session_id[0] &&
               (!event.session[0] || strcmp(event.session, s_session_id) == 0)) {
        switch (event.type) {
        case VOICE_TTS_START:
            s_tts_finished = false;
            xEventGroupSetBits(s_events, XIAOZHI_EVENT_SPEAKING);
            set_voice_state(VOICE_STATE_SPEAKING);
            reset_speech_timing_stats();
            break;
        case VOICE_TTS_STOP:
            if (s_voice_state == VOICE_STATE_SPEAKING) {
                s_tts_finished = true;
                s_tts_stop_us = esp_timer_get_time();
                xEventGroupClearBits(s_events, XIAOZHI_EVENT_SPEAKING);
            }
            break;
        case VOICE_END: enter_wake_idle(); break;
        case VOICE_STT: s_last_stt_ms = (uint32_t)(esp_timer_get_time() / 1000); break;
        default: break;
        }
    }
}
#endif

#if defined(CONFIG_CLOUD_PROTOCOL_V3)
/* 复用语音控制任务，不增加诊断任务；打印时绝不占用采集锁。 */
static void report_voice_status(void)
{
    static uint32_t contig_idle_epoch = UINT32_MAX;
    static TickType_t last_tick;
    static wake_word_stats_t previous;
    static uint32_t epoch, last_tx, last_drop, last_read_error;
    const TickType_t now = xTaskGetTickCount();
    if (now - last_tick < pdMS_TO_TICKS(XIAOZHI_REPORT_MS)) return;
    const uint32_t window_ms = (now - last_tick) * portTICK_PERIOD_MS;
    last_tick = now;
    if (!s_rtc_suspended && s_wake_word_ready &&
        s_voice_state == VOICE_STATE_WAKE_IDLE && contig_idle_epoch != s_afe_epoch) {
        mem_contig_log("WAKE_IDLE_STABLE");
        contig_idle_epoch = s_afe_epoch;
    }
    wake_word_stats_t afe = {0};
    xSemaphoreTake(s_capture_guard, portMAX_DELAY);
    const mic_diag_t mic = s_mic_diag;
    memset(&s_mic_diag, 0, sizeof(s_mic_diag));
    wake_word_get_stats(&afe);
    const uint32_t tx = s_capture_frames, drop = s_uplink_drop, errors = s_capture_errors;
    const EventBits_t bits = xEventGroupGetBits(s_events);
    if (epoch != s_afe_epoch) {
        memset(&previous, 0, sizeof(previous));
        epoch = s_afe_epoch;
    }
    xSemaphoreGive(s_capture_guard);

    /* 浮点运算和串口输出都放在控制任务，避免阻塞实时采集。 */
    if (mic.raw_frames) {
        double mean[3], variance[3];
        for (size_t ch = 0; ch < 3; ++ch) {
            mean[ch] = (double)mic.sum[ch] / mic.raw_frames;
            variance[ch] = (double)mic.square[ch] / mic.raw_frames - mean[ch] * mean[ch];
            if (variance[ch] < 0) variance[ch] = 0;
        }
        const double denominator = sqrt(variance[0] * variance[1]);
        const double correlation = denominator > 0 ?
            ((double)mic.cross / mic.raw_frames - mean[0] * mean[1]) / denominator : 0;
        ESP_LOGI("MIC_INPUT", "24k n=%lu AC_RMS(M1/M2/R)=%.0f/%.0f/%.0f DC=%.0f/%.0f/%.0f corr=%.3f nonzero=%lu equal=%lu double=%lu",
                 (unsigned long)mic.raw_frames, sqrt(variance[0]), sqrt(variance[1]), sqrt(variance[2]),
                 mean[0], mean[1], mean[2], correlation, (unsigned long)mic.nonzero_pairs,
                 (unsigned long)mic.equal_pairs, (unsigned long)mic.double_pairs);
    }

    const char *status = s_rtc_suspended ? "RTC暂停唤醒" :
        !s_wake_word_ready ? "唤醒引擎未就绪" :
        s_voice_state == VOICE_STATE_WAKE_IDLE ? "待机-等待你好小智" :
        s_voice_state == VOICE_STATE_CONNECTING ? "建立会话" :
        s_voice_state == VOICE_STATE_SPEAKING ? "播报中" : "监听中-请直接说话";
    const bool balanced = s_rtc_suspended ||
        mic.input_samples + previous.accumulator_remaining_samples ==
        afe.feed_samples - previous.feed_samples + afe.accumulator_remaining_samples;
    ESP_LOGI("VOICE_STATUS", "%s wake=%d uplink=%d | %lums feed/fetch=%lu/%lu empty=%lu hits=%lu VAD=%s S/N=%lu/%lu tx=%lu mic=%lu/%lu",
             status, !!(bits & XIAOZHI_EVENT_WAKE_ACTIVE), !!(bits & XIAOZHI_EVENT_UPLINK_ENABLED),
             (unsigned long)window_ms, (unsigned long)(afe.feed_count - previous.feed_count),
             (unsigned long)(afe.fetch_count - previous.fetch_count),
             (unsigned long)(afe.ringbuffer_underflow - previous.ringbuffer_underflow),
             (unsigned long)(afe.wake_detect_count - previous.wake_detect_count),
             afe.voice_detected ? "SPEECH" : "SILENCE",
             (unsigned long)(afe.speech_frames - previous.speech_frames),
             (unsigned long)(afe.silence_frames - previous.silence_frames),
             (unsigned long)(tx - last_tx), (unsigned long)mic.peak[0], (unsigned long)mic.peak[1]);
    if (!balanced || afe.feed_fail != previous.feed_fail || afe.output_drop != previous.output_drop ||
        drop != last_drop || errors != last_read_error || mic.clipped) {
        ESP_LOGW("VOICE_STATUS", "音频异常 balance=%d feed_fail=%lu out_drop=%lu send_drop=%lu io_or_codec_err=%lu clipped=%lu mic_peak=%lu/%lu ref=%lu",
                 balanced, (unsigned long)(afe.feed_fail - previous.feed_fail),
                 (unsigned long)(afe.output_drop - previous.output_drop), (unsigned long)(drop - last_drop),
                 (unsigned long)(errors - last_read_error), (unsigned long)mic.clipped,
                 (unsigned long)mic.peak[0], (unsigned long)mic.peak[1], (unsigned long)mic.peak[2]);
    }
    ESP_LOGI("VAD_DIAG", "us=%lld session_id=%s window_ms=%lu vad_speech_frames=%lu vad_silence_frames=%lu vad_speech_begin_count=%lu vad_speech_end_count=%lu valid_fetch=%lu",
             (long long)esp_timer_get_time(), s_session_id[0]?s_session_id:"NONE", (unsigned long)window_ms,
             (unsigned long)(afe.speech_frames-previous.speech_frames), (unsigned long)(afe.silence_frames-previous.silence_frames),
             (unsigned long)(afe.speech_begin_count-previous.speech_begin_count), (unsigned long)(afe.speech_end_count-previous.speech_end_count),
             (unsigned long)(afe.fetch_count-previous.fetch_count));
    previous = afe;
    last_tx = tx;
    last_drop = drop;
    last_read_error = errors;
}
#endif

static void wake_process_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "WAKE_PROCESS stack_caps=SPIRAM|8BIT stack_bytes=%u core=%d priority=%d capture=unchanged resampler=unchanged",
             (unsigned)XIAOZHI_WAKE_PROCESS_STACK_BYTES,
             XIAOZHI_WAKE_PROCESS_CORE, XIAOZHI_WAKE_PROCESS_PRIORITY);
    for (;;) {
#if defined(CONFIG_CLOUD_PROTOCOL_V3)
        /* 控制任务同步等待生命周期 worker；持 capture_guard 时 worker 不反向取锁。 */
        if (__atomic_exchange_n(&s_rtc_pending, false, __ATOMIC_ACQ_REL)) {
            const bool suspend = __atomic_load_n(&s_rtc_requested, __ATOMIC_ACQUIRE);
            s_rtc_result = ESP_OK;
            if (suspend && !s_rtc_suspended) {
                xSemaphoreTake(s_capture_guard, portMAX_DELAY);
                s_rtc_suspended = true;
                xSemaphoreGive(s_capture_guard);
                if (s_session_id[0]) cloud_mqtt_send_goodbye();
                enter_wake_idle();
                xSemaphoreTake(s_capture_guard, portMAX_DELAY);
                s_rtc_result = wake_word_deinit();
                if (s_rtc_result == ESP_OK) s_wake_word_ready = false;
                xSemaphoreGive(s_capture_guard);
                if (s_rtc_result == ESP_OK) {
                    ESP_LOGI(TAG, "RTC_AUDIO suspended AFE=released wake=0 processor=0 uplink=0");
                } else {
                    /* worker 创建失败时还未销毁 AFE；撤销暂停，不给 RTC 假成功。 */
                    xSemaphoreTake(s_capture_guard, portMAX_DELAY);
                    s_rtc_suspended = false;
                    xSemaphoreGive(s_capture_guard);
                    set_voice_state(VOICE_STATE_WAKE_IDLE);
                    ESP_LOGE(TAG, "RTC_AUDIO cleanup rejected: %s", esp_err_to_name(s_rtc_result));
                }
            } else if (!suspend && s_rtc_suspended) {
                xSemaphoreTake(s_capture_guard, portMAX_DELAY);
                s_rtc_result = wake_word_init(XIAOZHI_AFE_CHANNELS);
                if (s_rtc_result == ESP_OK) {
                    wake_word_set_callback(wake_word_detected_cb, NULL);
                    s_wake_word_ready = true;
                    s_rtc_suspended = false;
                    s_afe_epoch++;
                    memset(&s_mic_diag, 0, sizeof(s_mic_diag));
                }
                xSemaphoreGive(s_capture_guard);
                if (s_rtc_result == ESP_OK) set_voice_state(VOICE_STATE_WAKE_IDLE);
                ESP_LOGI(TAG, "RTC_AUDIO restore=%s", esp_err_to_name(s_rtc_result));
            }
            /* 同一阶段跨轮比较空闲量和最大块，不把一次分配成功当作无泄漏证据。 */
            ESP_LOGI(TAG, "RTC_AUDIO_MEM rtc_suspend=%d result=%s DMA=%u/%u INTERNAL=%u/%u PSRAM=%u stack_high_water=%u",
                     suspend, esp_err_to_name(s_rtc_result),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                     (unsigned)uxTaskGetStackHighWaterMark(NULL));
            xEventGroupSetBits(s_events, XIAOZHI_EVENT_RTC_ACK);
        }
        if (__atomic_exchange_n(&s_voice_event_overflow, false, __ATOMIC_ACQ_REL)) {
            ESP_LOGE(TAG, "语音控制队列已满，结束会话，避免遗漏 stop 导致持续上传");
            xQueueReset(s_voice_events);
            enter_wake_idle();
        }
        voice_event_t event;
        if (xQueueReceive(s_voice_events, &event, pdMS_TO_TICKS(50)) == pdTRUE && !s_rtc_suspended) {
            handle_voice_event(&event);
        }
        report_voice_status();
        if (!s_rtc_suspended) {
            if ((xEventGroupGetBits(s_events) & XIAOZHI_EVENT_CHANNEL_ACTIVE) &&
                !cloud_mqtt_is_session_ready()) {
                ESP_LOGW(TAG, "MQTT 会话失效，关闭语音通道并恢复唤醒");
                enter_wake_idle();
            }
            resume_listening_after_playback();
            if (s_voice_state == VOICE_STATE_LISTENING || s_voice_state == VOICE_STATE_CONTINUOUS_LISTENING) {
                const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
                /* 按有效 STT/进入监听时间续期，VAD 单帧静音不结束会话。 */
                if (now - s_voice_state_entered_ms >= VOICE_STATE_IDLE_TIMEOUT_MS &&
                    (!s_last_stt_ms || now - s_last_stt_ms >= VOICE_STATE_IDLE_TIMEOUT_MS)) {
                    ESP_LOGW(TAG, "监听 30s 无新的 STT，结束会话并恢复唤醒");
                    cloud_mqtt_send_goodbye();
                    enter_wake_idle();
                }
            }
        }
#else
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        process_detected_wake_word(wake_word_get_last());
#endif
    }
}

static void wake_word_detected_cb(const char *wake_word, void *user_data)
{
    (void)user_data;
    voice_trace("WAKE_DETECTED", NULL, ESP_OK);
    ESP_LOGI(TAG, "唤醒词事件已提交: %s", wake_word);
    if (s_wake_process_task != NULL) {
#if defined(CONFIG_CLOUD_PROTOCOL_V3)
        post_voice_event(VOICE_WAKE, NULL);
#else
        xTaskNotifyGive(s_wake_process_task);
#endif
    }
}

esp_err_t xiaozhi_audio_set_rtc_suspended(bool suspend)
{
#if defined(CONFIG_CLOUD_PROTOCOL_V3)
    if (!s_started) return ESP_OK;
    if (!s_wake_process_task) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_rtc_request_guard, portMAX_DELAY);
    xEventGroupClearBits(s_events, XIAOZHI_EVENT_RTC_ACK);
    __atomic_store_n(&s_rtc_requested, suspend, __ATOMIC_RELEASE);
    __atomic_store_n(&s_rtc_pending, true, __ATOMIC_RELEASE);
    /* 未收到释放确认前，RTC 不得继续申请编码器/Peer 资源。 */
    xEventGroupWaitBits(s_events, XIAOZHI_EVENT_RTC_ACK, pdTRUE, pdTRUE, portMAX_DELAY);
    const esp_err_t result = s_rtc_result;
    xSemaphoreGive(s_rtc_request_guard);
    return result;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

static void stop_worker_tasks(void)
{
    /* 初始化中途失败时，必须先停止已启动的音频任务，再释放 codec/I2S。
     * 否则工作任务可能继续访问已经销毁的句柄。 */
    if (s_capture_task != NULL) {
        vTaskDeleteWithCaps(s_capture_task);
        s_capture_task = NULL;
    }
    if (s_encoder_task != NULL) {
        vTaskDeleteWithCaps(s_encoder_task);
        s_encoder_task = NULL;
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
    for (size_t i = 0; i < XIAOZHI_AFE_CHANNELS; ++i) {
        if (s_audio.input_resamplers[i] != NULL) {
            esp_ae_rate_cvt_close(s_audio.input_resamplers[i]);
            s_audio.input_resamplers[i] = NULL;
        }
    }
    heap_caps_free(s_audio.resample_input);
    heap_caps_free(s_audio.resample_output);
    s_audio.resample_input = NULL;
    s_audio.resample_output = NULL;
    s_audio.resample_max_output_samples = 0;
    heap_caps_free(s_audio.capture_pcm);
    heap_caps_free(s_audio.uplink_pcm);
    heap_caps_free(s_audio.capture_raw);
    heap_caps_free(s_audio.capture_opus);
    s_audio.capture_pcm = NULL;
    s_audio.uplink_pcm = NULL;
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
    if (s_audio.input_codec_device != NULL) {
        esp_codec_dev_close(s_audio.input_codec_device);
        esp_codec_dev_delete(s_audio.input_codec_device);
        s_audio.input_codec_device = NULL;
    }
    if (s_audio.codec_device != NULL) {
        esp_codec_dev_close(s_audio.codec_device);
        esp_codec_dev_delete(s_audio.codec_device);
        s_audio.codec_device = NULL;
    }
    if (s_audio.input_codec_if != NULL) {
        audio_codec_delete_codec_if(s_audio.input_codec_if);
        s_audio.input_codec_if = NULL;
    }
    if (s_audio.codec_if != NULL) {
        audio_codec_delete_codec_if(s_audio.codec_if);
        s_audio.codec_if = NULL;
    }
    if (s_audio.input_ctrl_if != NULL) {
        audio_codec_delete_ctrl_if(s_audio.input_ctrl_if);
        s_audio.input_ctrl_if = NULL;
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
        .sda_io_num = BOARD_AUDIO_I2C_SDA,
        .scl_io_num = BOARD_AUDIO_I2C_SCL,
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

    /* 参考小智官方 BoxAudioCodec：ES8311 TX 使用标准 Philips I2S；新 PCB
     * 的 ES7210 RX 使用四时隙 TDM。两者共享 MCLK/BCLK/WS，但数据方向分别
     * 走 GPIO31(ES8311 DSDIN) 和 GPIO32(ES7210 SDOUT1/TDMOUT)。 */
    i2s_std_config_t tx_i2s_config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(XIAOZHI_CODEC_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = BOARD_AUDIO_MCLK,
            .bclk = BOARD_AUDIO_BCLK,
            .ws = BOARD_AUDIO_WS,
            .dout = BOARD_AUDIO_DOUT,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    tx_i2s_config.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    tx_i2s_config.slot_cfg.slot_mask = I2S_STD_SLOT_BOTH;
    ESP_GOTO_ON_ERROR(i2s_channel_init_std_mode(s_audio.tx_channel,
                                                &tx_i2s_config),
                      fail, TAG, "初始化 I2S 发送通道失败");
#if BOARD_AUDIO_HAS_ES7210
    i2s_tdm_config_t rx_i2s_config = {
        .clk_cfg = I2S_TDM_CLK_DEFAULT_CONFIG(XIAOZHI_CODEC_SAMPLE_RATE),
        .slot_cfg = I2S_TDM_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO,
            I2S_TDM_SLOT0 | I2S_TDM_SLOT1 |
            I2S_TDM_SLOT2 | I2S_TDM_SLOT3),
        .gpio_cfg = {
            .mclk = BOARD_AUDIO_MCLK,
            .bclk = BOARD_AUDIO_BCLK,
            .ws = BOARD_AUDIO_WS,
            .dout = I2S_GPIO_UNUSED,
            .din = BOARD_AUDIO_DIN,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    rx_i2s_config.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    rx_i2s_config.clk_cfg.bclk_div = 8;
    rx_i2s_config.slot_cfg.total_slot = 4;
    ESP_GOTO_ON_ERROR(i2s_channel_init_tdm_mode(s_audio.rx_channel,
                                                &rx_i2s_config),
                      fail, TAG, "初始化 ES7210 TDM 接收通道失败");
#else
    i2s_std_config_t rx_i2s_config = tx_i2s_config;
    rx_i2s_config.gpio_cfg.dout = I2S_GPIO_UNUSED;
    rx_i2s_config.gpio_cfg.din = BOARD_AUDIO_DIN;
    ESP_GOTO_ON_ERROR(i2s_channel_init_std_mode(s_audio.rx_channel,
                                                &rx_i2s_config),
                      fail, TAG, "初始化 I2S 接收通道失败");
#endif
    /* 只订阅溢出事件，不改变DMA队列、时钟或采集参数。 */
    i2s_event_callbacks_t diag_callbacks = {.on_recv_q_ovf = capture_diag_overflow};
    ESP_GOTO_ON_ERROR(i2s_channel_register_event_callback(s_audio.rx_channel, &diag_callbacks, NULL),
                      fail, TAG, "注册 RX 溢出统计失败");
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

#if BOARD_AUDIO_HAS_ES7210
    /* ES7210 的 AD0/AD1 允许形成 0x40~0x43 四个 7-bit 地址。首板先
     * 实际 probe，避免仅凭装配图猜地址电阻；codec 组件参数使用 8-bit
     * 地址表示法，所以探测值需要左移一位。 */
    uint8_t es7210_addr_7bit = 0;
    for (uint8_t candidate = 0x40; candidate <= 0x43; ++candidate) {
        if (i2c_master_probe(s_audio.i2c_bus, candidate, 50) == ESP_OK) {
            es7210_addr_7bit = candidate;
            break;
        }
    }
    ESP_GOTO_ON_FALSE(es7210_addr_7bit != 0, ESP_ERR_NOT_FOUND,
                      fail, TAG, "I2C 0x40~0x43 未检测到 ES7210");
    control_config.addr = (uint8_t)(es7210_addr_7bit << 1);
    ESP_LOGI(TAG, "检测到 ES7210：I2C 7-bit 地址=0x%02X", es7210_addr_7bit);
    s_audio.input_ctrl_if = audio_codec_new_i2c_ctrl(&control_config);
    ESP_GOTO_ON_FALSE(s_audio.input_ctrl_if != NULL, ESP_ERR_NO_MEM,
                      fail, TAG, "创建 ES7210 控制接口失败");
#endif

    es8311_codec_cfg_t codec_config = {
        .ctrl_if = s_audio.ctrl_if,
        .gpio_if = s_audio.gpio_if,
#if BOARD_AUDIO_HAS_ES7210
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
#else
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH,
#endif
#if BOARD_HAS_PA_GPIO
        .pa_pin = BOARD_AUDIO_PA_EN,
#else
        .pa_pin = -1,
#endif
        .pa_reverted = false,
        .master_mode = false,
        .use_mclk = true,
        .digital_mic = false,
        .invert_mclk = false,
        .invert_sclk = false,
        /* 不使用 PA 供电电压补偿。esp_codec_dev 会把 0V 当作默认
         * 5.0V/3.3V，因此这里显式设置相同参考电压，使 hw_gain=0dB。 */
        .hw_gain = {
            .pa_voltage = 3.3f,
            .codec_dac_voltage = 3.3f,
            .pa_gain = 0.0f,
        },
    };
    s_audio.codec_if = es8311_codec_new(&codec_config);
    ESP_GOTO_ON_FALSE(s_audio.codec_if != NULL, ESP_ERR_NOT_FOUND,
                      fail, TAG, "未检测到板载 ES8311");

#if BOARD_AUDIO_HAS_ES7210
    /* 当前 PCB 有两只实体麦克风，分别接 ES7210 MIC1、MIC2；MIC3 接
     * ES8311 OUT_P/N，作为 AEC 播放参考，MIC4 未使用。打开 MIC1/2/3
     * 后 ES7210 会进入四时隙 TDM，物理时隙顺序仍为
     * MIC1/MIC3/MIC2/MIC4。采集后重排为 MIC1/MIC2/MIC3，送入 MMR AFE。 */
    es7210_codec_cfg_t input_codec_config = {
        .ctrl_if = s_audio.input_ctrl_if,
        .master_mode = false,
        .mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2 |
                        ES7210_SEL_MIC3,
        .mclk_src = ES7210_MCLK_FROM_PAD,
        .mclk_div = 256,
    };
    s_audio.input_codec_if = es7210_codec_new(&input_codec_config);
    ESP_GOTO_ON_FALSE(s_audio.input_codec_if != NULL, ESP_ERR_NOT_FOUND,
                      fail, TAG, "未检测到板载 ES7210");
#endif

    esp_codec_dev_cfg_t device_config = {
#if BOARD_AUDIO_HAS_ES7210
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
#else
        .dev_type = ESP_CODEC_DEV_TYPE_IN_OUT,
#endif
        .codec_if = s_audio.codec_if,
        .data_if = s_audio.data_if,
    };
    s_audio.codec_device = esp_codec_dev_new(&device_config);
    ESP_GOTO_ON_FALSE(s_audio.codec_device != NULL, ESP_ERR_NO_MEM,
                      fail, TAG, "创建 ES8311 设备失败");

#if BOARD_AUDIO_HAS_ES7210
    device_config.dev_type = ESP_CODEC_DEV_TYPE_IN;
    device_config.codec_if = s_audio.input_codec_if;
    s_audio.input_codec_device = esp_codec_dev_new(&device_config);
    ESP_GOTO_ON_FALSE(s_audio.input_codec_device != NULL, ESP_ERR_NO_MEM,
                      fail, TAG, "创建 ES7210 输入设备失败");
#endif

    esp_codec_dev_sample_info_t output_sample_info = {
        .bits_per_sample = 16,
        .channel = 1,
        .channel_mask = 0,
        .sample_rate = XIAOZHI_CODEC_SAMPLE_RATE,
        .mclk_multiple = 0,
    };
    ESP_GOTO_ON_FALSE(esp_codec_dev_open(s_audio.codec_device,
                                         &output_sample_info) ==
                          ESP_CODEC_DEV_OK,
                      ESP_FAIL, fail, TAG, "打开 ES8311 失败");
#if BOARD_AUDIO_HAS_ES7210
    /* ES7210 TDM 顺序为 SLOT0=MIC1、SLOT1=MIC3(AEC参考)、SLOT2=MIC2、
     * SLOT3=MIC4。读取完整四槽，避免把参考误当第二只麦克风。 */
    esp_codec_dev_sample_info_t input_sample_info = output_sample_info;
    input_sample_info.channel = 4;
    input_sample_info.channel_mask = 0x0f;
    ESP_GOTO_ON_FALSE(esp_codec_dev_open(s_audio.input_codec_device,
                                         &input_sample_info) == ESP_CODEC_DEV_OK,
                      ESP_FAIL, fail, TAG, "打开 ES7210 失败");
#endif
    ESP_GOTO_ON_FALSE(esp_codec_dev_set_in_gain(
                          audio_input_device(),
                          XIAOZHI_CODEC_INPUT_GAIN_DB) == ESP_CODEC_DEV_OK,
                      ESP_FAIL, fail, TAG, "设置麦克风增益失败");
#if BOARD_AUDIO_HAS_ES7210
    /* 单麦对照保留原参考增益；双麦参考方案让 MIC1/MIC2/REF 均为 24dB。 */
#if XIAOZHI_SINGLE_MIC_TEST
    ESP_GOTO_ON_FALSE(esp_codec_dev_set_in_channel_gain(
                          audio_input_device(), ESP_CODEC_DEV_MAKE_CHANNEL_MASK(2),
                          0.0f) == ESP_CODEC_DEV_OK,
                      ESP_FAIL, fail, TAG, "设置 AEC 参考增益失败");
    ESP_LOGW(TAG, "MIC_ROUTE SINGLE_MIC_TEST=1 MIC2=SLOT2 REF=SLOT1 -> AFE=MR; gain=30dB; MIC1 excluded from AFE");
#else
    ESP_LOGI(TAG, "MIC_ROUTE slots=MIC1,REF,MIC2,unused -> AFE=MMR; gain=24/24/24dB; uplink=AFE mono");
#endif
#endif
    ESP_GOTO_ON_FALSE(esp_codec_dev_set_out_vol(
                          s_audio.codec_device,
                          XIAOZHI_CODEC_OUTPUT_VOLUME) == ESP_CODEC_DEV_OK,
                      ESP_FAIL, fail, TAG, "设置扬声器音量失败");
    s_output_volume = XIAOZHI_CODEC_OUTPUT_VOLUME;

    ESP_LOGI(TAG,
#if BOARD_AUDIO_HAS_ES7210
             "板载音频初始化完成：ES8311 STD DAC + ES7210 4-slot TDM ADC"
             "（双麦：MIC1=SLOT0、MIC2=SLOT2；AEC参考=SLOT1），I2S%d，"
#else
             "板载音频初始化完成：ES8311，I2S%d，"
#endif
             "24kHz/16bit，P4 DIN=GPIO%d DOUT=GPIO%d",
             XIAOZHI_I2S_PORT, BOARD_AUDIO_DIN, BOARD_AUDIO_DOUT);
    return ESP_OK;

fail:
    audio_hw_cleanup();
    return ret;
}

/* 独占 RX 的测试只初始化硬件，不创建 AFE、重采样和 Opus 任务。 */
static void raw_adc_test_task(void *arg)
{
    (void)arg;
    esp_err_t ret = audio_hw_init();
    /* 仅设置DAC播放音量，不写ES7210输入寄存器。 */
    if (ret == ESP_OK && (esp_codec_dev_set_out_vol(s_audio.codec_device, 10) != ESP_CODEC_DEV_OK ||
        esp_codec_dev_set_out_mute(s_audio.codec_device, false) != ESP_CODEC_DEV_OK)) ret = ESP_FAIL;
    if (ret == ESP_OK) ret = raw_adc_probe_run(s_audio.input_ctrl_if, s_audio.rx_channel, s_audio.tx_channel);
    ESP_LOGI("RAW_ADC", "RESULT=%s", esp_err_to_name(ret));
    vTaskDelete(NULL);
}

esp_err_t xiaozhi_audio_start_raw_test(void)
{
    return xTaskCreate(raw_adc_test_task, "raw_adc", 8192, NULL, 5, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
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
    /* esp_codec_dev_read() 经 I2S 驱动从其 DMA ring memcpy 到用户缓冲，
     * capture_raw 不由 DMA 控制器直接访问，放 PSRAM 保留内部连续块。 */
    const size_t capture_raw_size =
        XIAOZHI_CAPTURE_INPUT_SAMPLES * sizeof(int16_t) * XIAOZHI_CAPTURE_CHANNELS;
    s_audio.capture_raw = heap_caps_malloc(
        capture_raw_size,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    /* 重采样、Opus 编码及 UDP 封包仅由 CPU 访问，不属于 I2S DMA ring。 */
    const size_t afe_input_bytes = XIAOZHI_CAPTURE_SAMPLES_16K * sizeof(int16_t) * XIAOZHI_AFE_CHANNELS;
    const size_t pcm_bytes = afe_input_bytes > (size_t)s_audio.encoder_input_size ?
                            afe_input_bytes : (size_t)s_audio.encoder_input_size;
    s_audio.capture_pcm = heap_caps_malloc(
        pcm_bytes,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_audio.uplink_pcm = heap_caps_malloc(
        (size_t)s_audio.encoder_input_size,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_audio.capture_opus = heap_caps_malloc(
        (size_t)s_audio.encoder_output_size,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_audio.resample_input = heap_caps_malloc(
        XIAOZHI_CAPTURE_INPUT_SAMPLES * XIAOZHI_AFE_CHANNELS * sizeof(int16_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    esp_ae_rate_cvt_cfg_t resample_config = {
        .src_rate = XIAOZHI_CODEC_SAMPLE_RATE,
        .dest_rate = XIAOZHI_UPLINK_SAMPLE_RATE,
        .channel = ESP_AUDIO_MONO,
        .bits_per_sample = ESP_AUDIO_BIT16,
        .complexity = 2,
        .perf_type = ESP_AE_RATE_CVT_PERF_TYPE_SPEED,
    };
    esp_ae_err_t resample_err = ESP_AE_ERR_OK;
    for (size_t ch = 0; ch < XIAOZHI_AFE_CHANNELS; ++ch) {
        resample_err = esp_ae_rate_cvt_open(&resample_config, &s_audio.input_resamplers[ch]);
        uint32_t channel_max = 0;
        if (resample_err != ESP_AE_ERR_OK || s_audio.input_resamplers[ch] == NULL ||
            esp_ae_rate_cvt_get_max_out_sample_num(
                s_audio.input_resamplers[ch], XIAOZHI_CAPTURE_INPUT_SAMPLES, &channel_max) != ESP_AE_ERR_OK ||
            channel_max == 0 ||
            (s_audio.resample_max_output_samples != 0 && channel_max != s_audio.resample_max_output_samples)) {
            ESP_LOGE(TAG, "esp_ae_rate_cvt 初始化失败：channel=%u err=%d max=%u",
                     (unsigned)ch, (int)resample_err, (unsigned)channel_max);
            return ESP_FAIL;
        }
        s_audio.resample_max_output_samples = channel_max;
    }
    s_audio.resample_output = heap_caps_malloc(
        s_audio.resample_max_output_samples * XIAOZHI_AFE_CHANNELS * sizeof(int16_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_LOGI(TAG, "AUDIO_USER_PSRAM raw=%u pcm=%u opus=%u bytes",
             (unsigned)capture_raw_size, (unsigned)pcm_bytes,
             (unsigned)s_audio.encoder_output_size);
    for (size_t i = 0; i < XIAOZHI_PCM_POOL_SIZE; i++) {
        s_audio.playback_pcm[i] = heap_caps_malloc(
            XIAOZHI_PCM_BUFFER_BYTES,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (s_audio.capture_raw == NULL || s_audio.capture_pcm == NULL ||
        s_audio.uplink_pcm == NULL ||
        s_audio.capture_opus == NULL || s_audio.resample_input == NULL ||
        s_audio.resample_output == NULL) {
        ESP_LOGE(TAG, "小智音频工作缓冲分配失败");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "输入重采样器已就绪：%uHz->%uHz channels=%u max_out=%u samples/channel",
             XIAOZHI_CODEC_SAMPLE_RATE, XIAOZHI_UPLINK_SAMPLE_RATE,
             (unsigned)XIAOZHI_AFE_CHANNELS, (unsigned)s_audio.resample_max_output_samples);
    for (size_t i = 0; i < XIAOZHI_PCM_POOL_SIZE; i++) {
        if (s_audio.playback_pcm[i] == NULL) {
            ESP_LOGE(TAG, "小智 PCM 缓冲池分配失败：%u/%u",
                     (unsigned)i, XIAOZHI_PCM_POOL_SIZE);
            return ESP_ERR_NO_MEM;
        }
    }
    return ESP_OK;
}

static esp_err_t resample_mic_24k_to_16k(const int16_t *input,
                                         int16_t *output,
                                         size_t *output_samples)
{
    const size_t input_frames = XIAOZHI_CAPTURE_INPUT_SAMPLES;
    size_t channel_output_samples = 0;
    for (size_t ch = 0; ch < XIAOZHI_AFE_CHANNELS; ++ch) {
        /* 板级物理槽保持实测映射：单麦 MIC2=SLOT2、REF=SLOT1；双麦 MMR=SLOT0/2/1。 */
        const size_t slot = XIAOZHI_SINGLE_MIC_TEST ? (ch == 0 ? 2 : 1) :
                            (ch == 0 ? 0 : (ch == 1 ? 2 : 1));
        int16_t *mono_input = s_audio.resample_input + ch * input_frames;
        int16_t *mono_output = s_audio.resample_output + ch * s_audio.resample_max_output_samples;
        for (size_t frame = 0; frame < input_frames; ++frame) {
            mono_input[frame] = input[frame * XIAOZHI_CAPTURE_CHANNELS + slot];
        }
        uint32_t produced = s_audio.resample_max_output_samples;
        esp_ae_err_t err = esp_ae_rate_cvt_process(
            s_audio.input_resamplers[ch], (esp_ae_sample_t)mono_input,
            input_frames, (esp_ae_sample_t)mono_output, &produced);
        if (err != ESP_AE_ERR_OK || produced == 0 || produced > s_audio.resample_max_output_samples ||
            (channel_output_samples != 0 && produced != channel_output_samples)) {
            ESP_LOGE(TAG, "esp_ae_rate_cvt 处理失败：channel=%u err=%d samples=%u",
                     (unsigned)ch, (int)err, (unsigned)produced);
            /* 任一声道失败后清空全部相位，避免下次处理时通道间时间错位。 */
            for (size_t reset_ch = 0; reset_ch < XIAOZHI_AFE_CHANNELS; ++reset_ch) {
                (void)esp_ae_rate_cvt_reset(s_audio.input_resamplers[reset_ch]);
            }
            return ESP_FAIL;
        }
        channel_output_samples = produced;
    }
    for (size_t frame = 0; frame < channel_output_samples; ++frame) {
        for (size_t ch = 0; ch < XIAOZHI_AFE_CHANNELS; ++ch) {
            output[frame * XIAOZHI_AFE_CHANNELS + ch] =
                s_audio.resample_output[ch * s_audio.resample_max_output_samples + frame];
        }
    }
    *output_samples = channel_output_samples;
    return ESP_OK;
}

static void capture_task(void *arg)
{
    (void)arg;
    uint8_t *raw = s_audio.capture_raw;
    uint8_t *pcm = s_audio.capture_pcm;
    const int raw_size = XIAOZHI_CAPTURE_SAMPLES_16K * sizeof(int16_t) *
                         XIAOZHI_CODEC_SAMPLE_RATE /
                         XIAOZHI_UPLINK_SAMPLE_RATE * XIAOZHI_CAPTURE_CHANNELS;

    for (;;) {
        capture_diag_begin(esp_timer_get_time());
        int codec_error = esp_codec_dev_read(
            audio_input_device(), raw, raw_size);
        capture_diag_read_end(esp_timer_get_time(), codec_error, raw, raw_size);
        if (codec_error != ESP_CODEC_DEV_OK) {
            s_capture_errors++;
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        lifecycle_raw(raw, (size_t)raw_size);
        int64_t diag_ts = esp_timer_get_time();
        xSemaphoreTake(s_capture_guard, portMAX_DELAY);
        capture_diag_metric(CD_GUARD, esp_timer_get_time() - diag_ts);
        if (s_rtc_suspended || !s_wake_word_ready) {
            xSemaphoreGive(s_capture_guard);
            continue;
        }

        uint32_t clipped_samples = 0;
        uint32_t channel_peak[3] = {0};
        const int16_t *samples = (const int16_t *)raw;
        const size_t sample_count = (size_t)raw_size /
                                    sizeof(int16_t);
        for (size_t i = 0; i < sample_count; i++) {
            int32_t value = samples[i];
            uint32_t magnitude = (uint32_t)(value < 0 ? -value : value);
            size_t slot = i % XIAOZHI_CAPTURE_CHANNELS;
            if (slot == 3) continue;
            size_t ch = slot == 1 ? 2 : (slot == 2 ? 1 : 0);
            s_mic_diag.sum[ch] += value;
            s_mic_diag.square[ch] += (uint64_t)((int64_t)value * value);
            if (slot == 0) {
                const int32_t mic2 = samples[i + 2];
                s_mic_diag.raw_frames++;
                s_mic_diag.cross += (int64_t)value * mic2;
                /* 排除双零样本，避免静音让相等/精确两倍统计出现假阳性。 */
                if (value != 0 || mic2 != 0) {
                    s_mic_diag.nonzero_pairs++;
                    s_mic_diag.equal_pairs += value == mic2;
                    s_mic_diag.double_pairs += mic2 == value * 2;
                }
            }
            if (magnitude > channel_peak[ch]) channel_peak[ch] = magnitude;
            /* 参考单独报告，两只麦克风的统计不混入参考和未接通道。 */
            if (slot == 1) continue;
            if (magnitude >= 32000U) {
                clipped_samples++;
            }
        }

        diag_ts = esp_timer_get_time();
        size_t output_samples = 0;
        if (resample_mic_24k_to_16k(
            (const int16_t *)raw,
            (int16_t *)pcm,
            &output_samples) != ESP_OK || output_samples != XIAOZHI_CAPTURE_SAMPLES_16K) {
            capture_diag_metric(CD_RESAMPLE, esp_timer_get_time() - diag_ts);
            xSemaphoreGive(s_capture_guard);
            continue;
        }
        capture_diag_metric(CD_RESAMPLE, esp_timer_get_time() - diag_ts);
        audio_probe_input((const int16_t *)raw, (size_t)raw_size,
                          (const int16_t *)pcm, output_samples);

#if !defined(CONFIG_CLOUD_PROTOCOL_V3)
        EventBits_t bits = xEventGroupGetBits(s_events);
        const bool uplink = (bits & (XIAOZHI_EVENT_CHANNEL_ACTIVE |
                     XIAOZHI_EVENT_UPLINK_ENABLED)) ==
            (XIAOZHI_EVENT_CHANNEL_ACTIVE | XIAOZHI_EVENT_UPLINK_ENABLED);
        wake_word_set_uplink(uplink);
#endif
        /* 持续 feed 让 AEC 状态跟随播放参考；只有监听时缓存并上传 AFE 输出。 */
        diag_ts = esp_timer_get_time();
        wake_word_feed((const int16_t *)pcm,
                       XIAOZHI_CAPTURE_SAMPLES_16K * XIAOZHI_AFE_CHANNELS);
        capture_diag_metric(CD_FEED, esp_timer_get_time() - diag_ts);
        /* 实时采集只更新数字，不在持锁/供数路径打印串口日志。 */
        s_mic_diag.clipped += clipped_samples;
        s_mic_diag.input_samples += XIAOZHI_CAPTURE_SAMPLES_16K;
        for (size_t ch = 0; ch < 3; ++ch) {
            if (channel_peak[ch] > s_mic_diag.peak[ch]) s_mic_diag.peak[ch] = channel_peak[ch];
        }
        xSemaphoreGive(s_capture_guard);
    }
}

/* AFE 自带 PCM 输出缓冲；独立任务消费它，避免 Opus/UDP 阻塞下一次 I2S 读取。 */
static void encoder_task(void *arg)
{
    (void)arg;
    uint8_t *pcm = s_audio.uplink_pcm;
    uint8_t *opus = s_audio.capture_opus;
    const EventBits_t uplink_bits =
        XIAOZHI_EVENT_CHANNEL_ACTIVE | XIAOZHI_EVENT_UPLINK_ENABLED;
    for (;;) {
        if ((xEventGroupGetBits(s_events) & uplink_bits) != uplink_bits) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        xSemaphoreTake(s_capture_guard, portMAX_DELAY);
        const size_t samples = (!s_rtc_suspended && s_wake_word_ready) ?
            wake_word_read_pcm((int16_t *)pcm,
                (size_t)s_audio.encoder_input_size / sizeof(int16_t)) : 0;
        xSemaphoreGive(s_capture_guard);
        if (samples == 0) {
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }

        xSemaphoreTake(s_encoder_guard, portMAX_DELAY);
        /* 状态可能在取出 PCM 后切换；旧轮音频不能送入新轮会话。 */
        if ((xEventGroupGetBits(s_events) & uplink_bits) != uplink_bits ||
            s_rtc_suspended) {
            xSemaphoreGive(s_encoder_guard);
            continue;
        }
        s_uplink_pcm_frames++;
        esp_audio_enc_in_frame_t input_frame = {
            .buffer = pcm,
            .len = (uint32_t)s_audio.encoder_input_size,
        };
        esp_audio_enc_out_frame_t output_frame = {
            .buffer = opus,
            .len = (uint32_t)s_audio.encoder_output_size,
        };
        audio_probe_uplink((const int16_t *)pcm, (size_t)s_audio.encoder_input_size);
        int64_t diag_ts = esp_timer_get_time();
        esp_audio_err_t audio_error = esp_opus_enc_process(
            s_audio.opus_encoder, &input_frame, &output_frame);
        capture_diag_metric(CD_OPUS, esp_timer_get_time() - diag_ts);
        if (audio_error == ESP_AUDIO_ERR_OK && output_frame.encoded_bytes > 0) {
            s_uplink_opus_packets++;
#if defined(CONFIG_CLOUD_PROTOCOL_V3)
            bool diag_sink = capture_diag_sink(output_frame.encoded_bytes);
            esp_err_t send_error = ESP_OK;
            if (!diag_sink) {
                diag_ts = esp_timer_get_time();
                send_error = cloud_udp_send_opus(opus, output_frame.encoded_bytes);
                capture_diag_metric(CD_UDP, esp_timer_get_time() - diag_ts);
                capture_diag_udp_result(send_error, output_frame.encoded_bytes);
            }
#else
            bool diag_sink = false;
            esp_err_t send_error = video_streamer_agent_send_audio(
                opus, output_frame.encoded_bytes);
#endif
            if (!diag_sink) voice_trace("LIVE_OPUS_TX", NULL, send_error);
            if (send_error == ESP_OK) {
                if (!diag_sink) opus_probe_packet(opus, output_frame.encoded_bytes);
                s_capture_frames++;
                s_capture_bytes += output_frame.encoded_bytes;
            } else {
                s_uplink_drop++;
                s_capture_errors++;
            }
        } else {
            s_uplink_drop++;
            s_capture_errors++;
        }
        xSemaphoreGive(s_encoder_guard);
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

    if (!(xEventGroupGetBits(s_events) & XIAOZHI_EVENT_CHANNEL_ACTIVE)) return;

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
#if defined(CONFIG_CLOUD_PROTOCOL_V3)
    (void)connected;
    return;
#endif
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

#if !defined(CONFIG_CLOUD_PROTOCOL_V3)
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

#endif

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
#if defined(CONFIG_CLOUD_PROTOCOL_V3)
    /* 回调不直接更改 AFE/会话。队列消费者再核对 session，避免迟到消息复活旧会话。 */
    if (__atomic_load_n(&s_rtc_requested, __ATOMIC_ACQUIRE)) return;
    char event_session[80] = {0};
    json_string_value(text, "session_id", event_session, sizeof(event_session));
    if (strcmp(message_type, "hello") == 0) return;
    if (strcmp(message_type, "goodbye") == 0 || strcmp(message_type, "abort") == 0) {
        post_voice_event(VOICE_END, event_session);
        return;
    }
    if (strcmp(message_type, "tts") == 0 || strcmp(message_type, "listen") == 0) {
        char state[24] = {0};
        if (!json_string_value(text, "state", state, sizeof(state))) return;
        if (strcmp(message_type, "tts") == 0) {
            if (strcmp(state, "start") == 0) {
                voice_trace("TTS_START_RX", event_session, ESP_OK);
                post_voice_event(VOICE_TTS_START, event_session);
            }
            else if (strcmp(state, "stop") == 0) {
                post_voice_event(VOICE_TTS_STOP, event_session);
                if (expression_manager_post_state) expression_manager_post_state("tts_stop");
            } else if (strcmp(state, "sentence_start") == 0) ESP_LOGI(TAG, "小智回答文本：%.*s", (int)len, text);
        } else if (strcmp(state, "stop") == 0) post_voice_event(VOICE_END, event_session);
        return;
    }
    if (strcmp(message_type, "stt") == 0) {
        voice_trace("STT_RX", event_session, ESP_OK);
        post_voice_event(VOICE_STT, event_session);
        ESP_LOGI(TAG, "语音识别结果：%.*s", (int)len, text);
        return;
    }
#else
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
#if defined(CONFIG_CLOUD_PROTOCOL_V3)
            set_voice_state(VOICE_STATE_SPEAKING);
#endif
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
#if defined(CONFIG_CLOUD_PROTOCOL_V3)
                /* 服务端主动结束连续监听（会话即将/已经结束）→ 回唤醒待机 */
                set_voice_state(VOICE_STATE_WAKE_IDLE);
#endif
                ESP_LOGI(TAG, "服务端结束连续监听，等待唤醒词");
            }
        }
    }
#endif
    if (strcmp(message_type, "llm") == 0) {
        /* 小智官方下行格式：{"session_id":"xxx","type":"llm",
         *                   "emotion":"happy","text":"😀"}
         * emotion 决定屏幕表情；text 是与该情绪配套的文本（常常就是一个
         * 表情符号），只用于日志，不参与业务判断。
         *
         * 两处约定：
         *   1. 缺少 emotion 字段时回退 neutral，保证事件一定被投递，
         *      不会出现「有回答但屏幕没反应」；
         *   2. 仍然是投递到 Expression Manager 的事件队列，由 LVGL 定时器
         *      在 UI 线程里真正切屏——WebSocket 回调里不碰任何 LVGL 对象。
         *      词表外的取值也由 Expression Manager 统一回退 neutral。 */
        char emotion[24] = {0};
        char llm_text[128] = {0};
        if (!json_string_value(text, "emotion", emotion, sizeof(emotion))) {
            strncpy(emotion, "neutral", sizeof(emotion) - 1);
        }
        (void)json_string_value(text, "text", llm_text, sizeof(llm_text));

        /* 这条日志用独立的 tag "XIAOZHI"：联调时直接 grep 这一行就能确认
         * 服务端到底下发了什么情绪，不用在一堆 XIAOZHI_AUDIO 里翻。 */
        ESP_LOGI("XIAOZHI", "LLM emotion=%s text=%s", emotion, llm_text);
        if (expression_manager_post_emotion != NULL) {
            expression_manager_post_emotion(emotion);
        }
    } else if (strcmp(message_type, "stt") == 0) {
#if defined(CONFIG_CLOUD_PROTOCOL_V3)
        /* STT 到达 = 用户语音被有效处理，连续监听的空闲看门狗以此续期 */
        s_last_stt_ms = (uint32_t)(esp_timer_get_time() / 1000);
#endif
        ESP_LOGI(TAG, "语音识别结果：%.*s", (int)len, text);
    }
}

static void delete_voice_control_resources(void)
{
#if defined(CONFIG_CLOUD_PROTOCOL_V3)
    if (s_voice_events) vQueueDeleteWithCaps(s_voice_events);
    s_voice_events = NULL;
#endif
    if (s_capture_guard) vSemaphoreDelete(s_capture_guard);
    if (s_encoder_guard) vSemaphoreDelete(s_encoder_guard);
    if (s_rtc_request_guard) vSemaphoreDelete(s_rtc_request_guard);
    s_capture_guard = s_encoder_guard = s_rtc_request_guard = NULL;
}

static void abort_audio_service(void)
{
    /* 异步初始化失败时撤销所有对外回调，避免网络任务继续访问半初始化状态。 */
    video_streamer_set_agent_callbacks(NULL);
    stop_worker_tasks();
    delete_voice_control_resources();
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
    ESP_LOGI(TAG, "AUDIO_DIAGNOSTICS udp_ab=%d lifecycle_tx=%d recording=%d",
             CAPTURE_UPLINK_AB_DIAGNOSTIC, AUDIO_LIFECYCLE_DIAGNOSTIC,
             AUDIO_RECORDING_DIAGNOSTIC);
    if (audio_hw_init() != ESP_OK ||
        (AUDIO_LIFECYCLE_DIAGNOSTIC && !CAPTURE_UPLINK_AB_DIAGNOSTIC && lifecycle_prepare(s_audio.input_ctrl_if, s_audio.rx_channel, s_audio.tx_channel) != ESP_OK) ||
        audio_codec_init() != ESP_OK ||
        audio_work_buffers_init() != ESP_OK ||
        capture_diag_init() != ESP_OK) {
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

    mem_contig_log("AUDIO_TASKS_BEFORE");
    BaseType_t task_result = xTaskCreatePinnedToCoreWithCaps(
        output_task, "xiaozhi_spk", XIAOZHI_OUTPUT_STACK_BYTES, NULL,
        XIAOZHI_OUTPUT_PRIORITY, &s_output_task, XIAOZHI_OUTPUT_CORE,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (task_result == pdPASS) {
        mem_contig_log("WAKE_PROCESS_CREATE_BEFORE");
        task_result = xTaskCreatePinnedToCoreWithCaps(
            wake_process_task, "wake_process",
            XIAOZHI_WAKE_PROCESS_STACK_BYTES, NULL,
            XIAOZHI_WAKE_PROCESS_PRIORITY, &s_wake_process_task,
            XIAOZHI_WAKE_PROCESS_CORE,
            XIAOZHI_WAKE_PROCESS_STACK_CAPS);
        mem_contig_log("WAKE_PROCESS_CREATE_AFTER");
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
    if (task_result == pdPASS) {
        task_result = xTaskCreatePinnedToCoreWithCaps(
            encoder_task, "xiaozhi_enc", XIAOZHI_ENCODER_STACK_BYTES, NULL,
            XIAOZHI_ENCODER_PRIORITY, &s_encoder_task, XIAOZHI_CAPTURE_CORE,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    mem_contig_log("AUDIO_TASKS_AFTER");
    if (task_result != pdPASS) {
        ESP_LOGE(TAG, "创建小智音频工作任务失败");
        abort_audio_service();
        return;
    }

    ESP_LOGI(TAG,
             "小智音频流水线已就绪：MIC=CPU%d/P%d，ENC=CPU%d/P%d，DEC=CPU%d/P%d，"
             "SPK=CPU%d/P%d，WAKE=CPU%d/P%d，PCM=%u帧",
             XIAOZHI_CAPTURE_CORE, XIAOZHI_CAPTURE_PRIORITY,
             XIAOZHI_CAPTURE_CORE, XIAOZHI_ENCODER_PRIORITY,
             XIAOZHI_DECODE_CORE, XIAOZHI_DECODE_PRIORITY,
             XIAOZHI_OUTPUT_CORE, XIAOZHI_OUTPUT_PRIORITY,
             XIAOZHI_WAKE_PROCESS_CORE, XIAOZHI_WAKE_PROCESS_PRIORITY,
             XIAOZHI_PCM_QUEUE_DEPTH);

    if (AUDIO_LIFECYCLE_DIAGNOSTIC) lifecycle_start();

    /* 初始化已完成；收发由 MIC、SPK 和 WebSocket 任务负责，无需保留空转任务。 */
    s_service_task = NULL;
    vTaskDeleteWithCaps(NULL);
}

/* ================= 表情链路离线自检（验证完把宏改回 0）=================
 *
 * 为什么要这个：官方服务器在握手后 ~280 ms 就发 CLOSE 关连接且不给原因，
 * 下行里永远等不到 llm 消息 —— 「服务端回复 → 表情」这条路实机走不通。
 *
 * 那就把真正存疑的那一环单独隔离出来验：服务端下发的消息原文。
 * 下面把官方格式的 llm JSON 直接喂进 server_text_callback —— 这和
 * WEBSOCKET_EVENT_DATA 收到文本帧后调用的是**同一个函数**。所以
 * JSON 解析、llm 分支、两行联调日志、事件投递、LVGL 定时器切屏全部
 * 走真实代码，只有 TLS/TCP 没参与。
 *
 * 每步预期在串口出现两行，屏幕同时切脸（每张停 1800 ms，见
 * expression_manager.c 的 play_file(entry->file, 1800)）：
 *   XIAOZHI: LLM emotion=happy text=😀
 *   EXPRESSION: emotion happy -> EXP_HAPPY
 * 画面对照 src/demo/tools/gif 下的 8 张源图。
 * 若出现「表情播放请求失败：/sdcard/expressions/exp_NN.bin」，那是 SD 卡
 * 上没素材，不是这条链路的问题。
 *
 * ponytail: 写成文件内的宏而不是 Kconfig —— 改一行就能开关，
 * 不必为它再跑一次 reconfigure（这个坑踩过了）。 */
/* 状态日志与开关说明见下方注释块。当前=0：正式 Lummiss 模式不开机自检，
 * 只在真实服务端下发 emotion 事件时切表情。 */
#define EXPRESSION_SELFTEST 0

#if EXPRESSION_SELFTEST

/* 步进间隔必须大于表情停留时长（1800 ms），否则后一张脸会盖掉前一张，
 * 肉眼分不出到底切没切。 */
#define EXPRESSION_SELFTEST_STEP_MS 2400

/* 顺序刻意让相邻两张脸的差异最大，便于发现"切错素材"。
 * 最后一条用小智词表里**不存在**的词，用来验证
 * 「未识别的 emotion 统一回退 neutral」这条约定。 */
static const char *const k_expression_selftest[] = {
    "{\"session_id\":\"selftest\",\"type\":\"llm\",\"emotion\":\"happy\",\"text\":\"😀\"}",
    "{\"session_id\":\"selftest\",\"type\":\"llm\",\"emotion\":\"sad\",\"text\":\"😢\"}",
    "{\"session_id\":\"selftest\",\"type\":\"llm\",\"emotion\":\"angry\",\"text\":\"😠\"}",
    "{\"session_id\":\"selftest\",\"type\":\"llm\",\"emotion\":\"laughing\",\"text\":\"😄\"}",
    "{\"session_id\":\"selftest\",\"type\":\"llm\",\"emotion\":\"surprised\",\"text\":\"😲\"}",
    "{\"session_id\":\"selftest\",\"type\":\"llm\",\"emotion\":\"confused\",\"text\":\"😕\"}",
    "{\"session_id\":\"selftest\",\"type\":\"llm\",\"emotion\":\"thinking\",\"text\":\"🤔\"}",
    "{\"session_id\":\"selftest\",\"type\":\"llm\",\"emotion\":\"neutral\",\"text\":\"🙂\"}",
    "{\"session_id\":\"selftest\",\"type\":\"llm\",\"emotion\":\"no_such_emotion\",\"text\":\"回退测试\"}",
};

static void expression_selftest_task(void *arg)
{
    (void)arg;
    const size_t total = sizeof(k_expression_selftest) /
                         sizeof(k_expression_selftest[0]);

    /* 必须等 UI 任务把 expression_manager 建起来再投：本任务由 app_main 调用，
     * 而 expression_manager_init() 在 ui_task 里。实测两者相隔约 80 ms
     * （音频流水线就绪 → 表情管理器就绪），这里给 5 s 余量。
     * 投早了不会报错 —— expression_manager 的 post() 对未初始化是**静默丢弃**，
     * 只会表现为"屏幕没反应"，所以这个等待不能省。 */
    vTaskDelay(pdMS_TO_TICKS(5000));

    ESP_LOGW(TAG, "表情自检开始：%u 步 x %d ms，请看屏幕",
             (unsigned)total, EXPRESSION_SELFTEST_STEP_MS);
    for (size_t i = 0; i < total; i++) {
        ESP_LOGI(TAG, "自检 %u/%u", (unsigned)(i + 1), (unsigned)total);
        server_text_callback(k_expression_selftest[i],
                             strlen(k_expression_selftest[i]), NULL);
        vTaskDelay(pdMS_TO_TICKS(EXPRESSION_SELFTEST_STEP_MS));
    }
    ESP_LOGW(TAG, "表情自检结束（最后一条 EXPRESSION 日志应为 unknown -> neutral）");
    vTaskDelete(NULL);
}

#endif /* EXPRESSION_SELFTEST */

/* 档位 9 的音频自检与正式小智音频流水线完全隔离：复用同一套
 * ES8311/I2S 初始化代码，但不创建 WakeNet、Opus、网络或会话任务。 */
#define AUDIO_HW_TEST_FRAME_SAMPLES   480U  /* 24 kHz 下 20 ms/每声道 */
#define AUDIO_HW_TEST_MIC_CHANNELS    4U
#define AUDIO_HW_TEST_TONE_CHANNELS   1U
#define AUDIO_HW_TEST_MIC_SAMPLES     (AUDIO_HW_TEST_FRAME_SAMPLES * AUDIO_HW_TEST_MIC_CHANNELS)
#define AUDIO_HW_TEST_TONE_SAMPLES    (AUDIO_HW_TEST_FRAME_SAMPLES * AUDIO_HW_TEST_TONE_CHANNELS)
#define AUDIO_HW_TEST_MIC_BYTES       (AUDIO_HW_TEST_MIC_SAMPLES * sizeof(int16_t))
#define AUDIO_HW_TEST_TONE_BYTES      (AUDIO_HW_TEST_TONE_SAMPLES * sizeof(int16_t))
#define AUDIO_HW_TEST_TONE_CHUNKS     250U  /* 连续播放，每约5秒报告一次 */
#define AUDIO_HW_TEST_FIRST_TONE_MS   1500U
#define AUDIO_HW_TEST_RETRY_MS        3000U
#define AUDIO_HW_TEST_OUTPUT_VOLUME   10
#define AUDIO_HW_TEST_STACK_BYTES     4096U
#define AUDIO_HW_TEST_TASK_PRIORITY   5
#define AUDIO_HW_TEST_TASK_CORE       0

/* 24 个采样点对应 24 kHz 下的 1 kHz 正弦波，约37% PCM满量程。
 * codec音量10%，连续播放，方便测量模拟链路。 */
static const int16_t s_audio_test_sine_1khz[24] = {
        0,   3106,   6000,   8485,  10392,  11591,  12000,  11591,
    10392,   8485,   6000,   3106,      0,  -3106,  -6000,  -8485,
   -10392, -11591, -12000, -11591, -10392,  -8485,  -6000,  -3106,
};

/* Read hardware, not codec_dev's cached volume/mute state. Register fields
 * and volume conversion follow the bundled esp_codec_dev ES8311 driver. */
static bool audio_test_check_dac(void)
{
    int format = 0, mute = 0, volume = 0;
    int rf = esp_codec_dev_read_reg(s_audio.codec_device, 0x09, &format);
    int rm = esp_codec_dev_read_reg(s_audio.codec_device, 0x31, &mute);
    int rv = esp_codec_dev_read_reg(s_audio.codec_device, 0x32, &volume);
    if (rf || rm || rv) {
        ESP_LOGE(TAG, "ES8311 READBACK FAILED: reg09=%d reg31=%d reg32=%d", rf, rm, rv);
        return false;
    }
    /* Default curve: 10% -> -45 dB; hw_gain=0; (-45+95.5)*2=101. */
    const int expected_volume = (int)((-50.0f + AUDIO_HW_TEST_OUTPUT_VOLUME * 0.5f + 95.5f) * 2.0f);
    const bool ok = (format & 0x7f) == 0x0c && (mute & 0x60) == 0 &&
                    volume == expected_volume;
    ESP_LOGI(TAG, "ES8311 READBACK: reg09=0x%02X (I2S/16bit/left expected=0x0C) "
             "reg31=0x%02X mute_bits=0x%02X reg32=0x%02X expected=0x%02X %s",
             format, mute, mute & 0x60, volume, expected_volume, ok ? "PASS" : "MISMATCH");
    return ok;
}

static esp_err_t audio_test_reduce_ref_gain(void)
{
#if BOARD_AUDIO_HAS_ES7210
    int before[3] = {0}, after[3] = {0};
    for (int i = 0; i < 3; ++i) {
        if (esp_codec_dev_read_reg(s_audio.input_codec_device, 0x43 + i, &before[i])) {
            ESP_LOGE(TAG, "ES7210 gain read failed before update: reg=0x%02X", 0x43 + i);
            return ESP_FAIL;
        }
    }
    /* Physical MIC3 is mask BIT(2), although its TDM data is in SLOT1.
     * Call the codec interface directly to preserve its I2C error result. */
    int result = s_audio.input_codec_if->set_mic_channel_gain(
        s_audio.input_codec_if, ESP_CODEC_DEV_MAKE_CHANNEL_MASK(2), 0.0f);
    if (result != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "ES7210 MIC3 gain write failed: %d", result);
        return ESP_FAIL;
    }
    for (int i = 0; i < 3; ++i) {
        if (esp_codec_dev_read_reg(s_audio.input_codec_device, 0x43 + i, &after[i])) {
            ESP_LOGE(TAG, "ES7210 gain read failed after update: reg=0x%02X", 0x43 + i);
            return ESP_FAIL;
        }
    }
    const bool ok = before[0] == after[0] && before[1] == after[1] &&
                    after[2] == (before[2] & ~0x0f);
    ESP_LOGI(TAG, "ES7210 GAIN READBACK: MIC1 0x%02X->0x%02X MIC2 0x%02X->0x%02X "
             "MIC3 0x%02X->0x%02X (0dB) %s", before[0], after[0], before[1], after[1],
             before[2], after[2], ok ? "PASS" : "MISMATCH");
    return ok ? ESP_OK : ESP_FAIL;
#else
    return ESP_OK;
#endif
}

/* 档位9直接观察 ES7210 的原始物理帧。codec_dev 打开输入后已经把 TX
 * 扩展为 2 x 32 bit，使它与 RX 的 4 x 16 bit 同为 64 bit/帧。测试阶段
 * 只能扩展 RX slot mask，不能再把 TX 改回 2 x 16 bit，否则 P4 全双工
 * 两侧帧长不一致，RX DMA 会持续 ESP_ERR_TIMEOUT。 */
static esp_err_t audio_hardware_test_force_official_i2s(void)
{
#if BOARD_AUDIO_HAS_ES7210
    i2s_tdm_slot_config_t rx_slot_cfg = I2S_TDM_PHILIPS_SLOT_DEFAULT_CONFIG(
        I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO,
        I2S_TDM_SLOT0 | I2S_TDM_SLOT1 | I2S_TDM_SLOT2 | I2S_TDM_SLOT3);
    rx_slot_cfg.total_slot = 4;

    esp_err_t err = i2s_channel_disable(s_audio.rx_channel);
    if (err != ESP_OK) {
        return err;
    }
    err = i2s_channel_reconfig_tdm_slot(s_audio.rx_channel, &rx_slot_cfg);
    if (err == ESP_OK) {
        err = i2s_channel_enable(s_audio.rx_channel);
    }
    if (err == ESP_OK) {
        ESP_LOGI(TAG,
                 "AUDIO_HW_TEST 帧格式：TX=STD 2x32bit，RX=TDM 4x16bit，"
                 "均为64bit/帧；24kHz，MCLK=6.144MHz；双麦=SLOT0/MIC1 + "
                 "SLOT2/MIC2，SLOT1/MIC3=AEC参考，SLOT3/MIC4=禁用");
    }
    return err;
#else
    ESP_LOGI(TAG, "AUDIO_HW_TEST v1.3: ES8311 STD TX/RX, 24kHz/16bit mono; PA GPIO%d",
             BOARD_AUDIO_PA_EN);
    return ESP_OK;
#endif
}

static void audio_hardware_test_mic_task(void *arg)
{
    (void)arg;
#if !BOARD_AUDIO_HAS_ES7210
    uint32_t peak = 0, samples = 0, errors = 0;
    int64_t started = esp_timer_get_time();
    for (;;) {
        int result = esp_codec_dev_read(audio_input_device(), s_hardware_test_mic_buffer,
                                       AUDIO_HW_TEST_FRAME_SAMPLES * sizeof(int16_t));
        if (result == ESP_CODEC_DEV_OK) {
            for (size_t i = 0; i < AUDIO_HW_TEST_FRAME_SAMPLES; ++i) {
                int32_t sample = s_hardware_test_mic_buffer[i];
                uint32_t magnitude = sample < 0 ? -sample : sample;
                if (magnitude > peak) peak = magnitude;
            }
            samples += AUDIO_HW_TEST_FRAME_SAMPLES;
        } else {
            errors++;
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (esp_timer_get_time() - started >= 1000000) {
            ESP_LOGI(TAG, "AUDIO_HW_TEST ES8311 MIC peak=%" PRIu32 " samples=%" PRIu32
                     " read_err=%" PRIu32 " (no ES7210 MIC3 reference)", peak, samples, errors);
            peak = samples = errors = 0;
            started = esp_timer_get_time();
        }
    }
#else
    uint32_t report_peak[AUDIO_HW_TEST_MIC_CHANNELS] = {0};
    uint64_t report_abs_sum[AUDIO_HW_TEST_MIC_CHANNELS] = {0};
    uint32_t report_frames = 0;
    uint32_t read_errors = 0;
    int32_t ref_min = INT16_MAX, ref_max = INT16_MIN;
    int64_t ref_sum = 0;
    uint32_t ref_clipped = 0, ref_rising = 0;
    int32_t ref_previous = 0;
    bool ref_have_previous = false;
    esp_err_t last_read_error = ESP_OK;
    size_t last_bytes_read = 0;
    int64_t report_started_us = esp_timer_get_time();

    for (;;) {
        size_t bytes_read = 0;
        const esp_err_t result = i2s_channel_read(
            s_audio.rx_channel, s_hardware_test_mic_buffer,
            AUDIO_HW_TEST_MIC_BYTES, &bytes_read, pdMS_TO_TICKS(100));
        if (result != ESP_OK || bytes_read != AUDIO_HW_TEST_MIC_BYTES) {
            ref_have_previous = false;
            read_errors++;
            last_read_error = result;
            last_bytes_read = bytes_read;
            const int64_t now_us = esp_timer_get_time();
            if (now_us - report_started_us >= 1000000) {
                ESP_LOGW(TAG,
                         "AUDIO_HW_TEST MIC读取失败：result=%s bytes=%u/%u "
                         "read_err=%" PRIu32,
                         esp_err_to_name(last_read_error),
                         (unsigned)last_bytes_read,
                         (unsigned)AUDIO_HW_TEST_MIC_BYTES, read_errors);
                read_errors = 0;
                report_started_us = now_us;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        for (size_t frame = 0; frame < AUDIO_HW_TEST_FRAME_SAMPLES; ++frame) {
            const int32_t ref = s_hardware_test_mic_buffer[frame * AUDIO_HW_TEST_MIC_CHANNELS + 1];
            if (ref < ref_min) ref_min = ref;
            if (ref > ref_max) ref_max = ref;
            ref_sum += ref;
            if (ref <= -32760 || ref >= 32760) ref_clipped++;
            if (ref_have_previous && ref_previous < 0 && ref >= 0) ref_rising++;
            ref_previous = ref;
            ref_have_previous = true;
            for (size_t channel = 0; channel < AUDIO_HW_TEST_MIC_CHANNELS; ++channel) {
                const int32_t sample =
                    s_hardware_test_mic_buffer[
                        frame * AUDIO_HW_TEST_MIC_CHANNELS + channel];
                const uint32_t magnitude =
                    (uint32_t)(sample < 0 ? -sample : sample);
                if (magnitude > report_peak[channel]) {
                    report_peak[channel] = magnitude;
                }
                report_abs_sum[channel] += magnitude;
            }
        }
        report_frames += AUDIO_HW_TEST_FRAME_SAMPLES;

        const int64_t now_us = esp_timer_get_time();
        if (now_us - report_started_us >= 1000000) {
            uint32_t average[AUDIO_HW_TEST_MIC_CHANNELS] = {0};
            for (size_t channel = 0; channel < AUDIO_HW_TEST_MIC_CHANNELS;
                 ++channel) {
                average[channel] = report_frames > 0
                    ? (uint32_t)(report_abs_sum[channel] / report_frames) : 0;
            }
            ESP_LOGI(TAG,
                     "AUDIO_HW_TEST MIC1[SLOT0] peak/avg=%" PRIu32 "/%" PRIu32
                     " AEC_REF[SLOT1] peak/avg=%" PRIu32 "/%" PRIu32
                     " MIC2[SLOT2] peak/avg=%" PRIu32 "/%" PRIu32
                     " UNUSED[SLOT3] peak/avg=%" PRIu32 "/%" PRIu32
                     " frames=%" PRIu32 " read_err=%" PRIu32,
                     report_peak[0], average[0], report_peak[1], average[1],
                     report_peak[2], average[2], report_peak[3], average[3],
                     report_frames, read_errors);
            const uint32_t clip_permille = report_frames ? (uint32_t)((uint64_t)ref_clipped * 1000 / report_frames) : 0;
            const uint32_t crossing_hz = report_frames ? (uint32_t)((uint64_t)ref_rising * XIAOZHI_CODEC_SAMPLE_RATE / report_frames) : 0;
            ESP_LOGI(TAG, "AEC_REF DIAG: min=%" PRId32 " max=%" PRId32 " mean=%" PRId32
                     " clip=%" PRIu32 ".%" PRIu32 "%% (abs>=32760) rising_hz=%" PRIu32
                     " (zero-cross estimate, not tone verification)",
                     ref_min, ref_max, (int32_t)(ref_sum / report_frames),
                     clip_permille / 10, clip_permille % 10, crossing_hz);
            /* 48 consecutive samples = 2 ms / two expected 1 kHz cycles.
             * One bounded dump per second, never per DMA block. */
            char pcm_line[384];
            size_t used = 0;
            for (size_t i = 0; i < 48; ++i) {
                int n = snprintf(pcm_line + used, sizeof(pcm_line) - used, "%s%d",
                                 i ? "," : "", (int)s_hardware_test_mic_buffer[i * AUDIO_HW_TEST_MIC_CHANNELS + 1]);
                if (n < 0 || (size_t)n >= sizeof(pcm_line) - used) break;
                used += (size_t)n;
            }
            ESP_LOGI(TAG, "AEC_REF PCM48 fs=%d slot=1: %s", XIAOZHI_CODEC_SAMPLE_RATE, pcm_line);
            ref_min = INT16_MAX;
            ref_max = INT16_MIN;
            ref_sum = 0;
            ref_clipped = ref_rising = 0;
            ref_have_previous = false;
            memset(report_peak, 0, sizeof(report_peak));
            memset(report_abs_sum, 0, sizeof(report_abs_sum));
            report_frames = 0;
            read_errors = 0;
            report_started_us = now_us;
        }
    }
#endif
}

static void audio_hardware_test_spk_task(void *arg)
{
    (void)arg;
    uint32_t tone_count = 0;
    vTaskDelay(pdMS_TO_TICKS(AUDIO_HW_TEST_FIRST_TONE_MS));

    for (;;) {
        tone_count++;
        const int64_t started_us = esp_timer_get_time();
        uint32_t write_errors = 0;
        const int unmute_result =
            esp_codec_dev_set_out_mute(s_audio.codec_device, false);
        if (!audio_test_check_dac()) {
            ESP_LOGE(TAG, "AUDIO_HW_TEST SPK skipped: DAC register verification failed");
            vTaskDelay(pdMS_TO_TICKS(AUDIO_HW_TEST_RETRY_MS));
            continue;
        }
        ESP_LOGI(TAG,
                 "AUDIO_HW_TEST SPK tone #%" PRIu32
                 " block（1kHz持续播放，每约5秒报告，volume=%d%%，unmute=%d）",
                 tone_count, AUDIO_HW_TEST_OUTPUT_VOLUME, unmute_result);

        for (uint32_t i = 0; i < AUDIO_HW_TEST_TONE_CHUNKS; ++i) {
            if (esp_codec_dev_write(s_audio.codec_device,
                                    s_hardware_test_tone_buffer,
                                    AUDIO_HW_TEST_TONE_BYTES) !=
                ESP_CODEC_DEV_OK) {
                write_errors++;
            }
        }

        const uint32_t elapsed_ms =
            (uint32_t)((esp_timer_get_time() - started_us) / 1000);
        if (write_errors == 0) {
            ESP_LOGI(TAG,
                     "AUDIO_HW_TEST SPK tone #%" PRIu32 " OK，耗时=%" PRIu32 " ms",
                     tone_count, elapsed_ms);
        } else {
            ESP_LOGE(TAG,
                     "AUDIO_HW_TEST SPK tone #%" PRIu32
                     " FAILED，write_err=%" PRIu32 "，耗时=%" PRIu32 " ms",
                     tone_count, write_errors, elapsed_ms);
        }
        /* I2S writes pace playback. Only back off on failure; no silent gap. */
        if (write_errors != 0) {
            vTaskDelay(pdMS_TO_TICKS(AUDIO_HW_TEST_RETRY_MS));
        }
    }
}

esp_err_t xiaozhi_audio_start_hardware_test(void)
{
    if (s_hardware_test_started) {
        return ESP_OK;
    }
    if (s_started) {
        ESP_LOGE(TAG, "正式小智音频已启动，不能同时启动本地音频自检");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = audio_hw_init();
    if (err != ESP_OK) {
        return err;
    }
    err = audio_hardware_test_force_official_i2s();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "音频自检切换官方STD/TDM帧格式失败：%s",
                 esp_err_to_name(err));
        goto fail;
    }

    /* MIC 缓冲直接参与 I2S 读取，放在 INTERNAL+DMA；正式播放链路已经证明
     * codec write 可从 PSRAM 输入，所以测试音缓冲不占用紧张的 DMA 堆。 */
    s_hardware_test_mic_buffer = heap_caps_malloc(
        AUDIO_HW_TEST_MIC_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    s_hardware_test_tone_buffer = heap_caps_malloc(
        AUDIO_HW_TEST_TONE_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_hardware_test_mic_buffer == NULL ||
        s_hardware_test_tone_buffer == NULL) {
        ESP_LOGE(TAG, "分配音频自检缓冲失败：MIC=%p SPK=%p",
                 s_hardware_test_mic_buffer, s_hardware_test_tone_buffer);
        err = ESP_ERR_NO_MEM;
        goto fail;
    }
    for (size_t frame = 0; frame < AUDIO_HW_TEST_FRAME_SAMPLES; ++frame) {
        const int16_t sample = s_audio_test_sine_1khz[frame %
            (sizeof(s_audio_test_sine_1khz) / sizeof(s_audio_test_sine_1khz[0]))];
        s_hardware_test_tone_buffer[frame] = sample;
    }

    /* 使用清晰可听但非满幅的诊断音量，并显式解除静音。这样 I2S 写成功但
     * 没有声音时，排查范围可直接收敛到 ES8311 模拟输出、PA 和扬声器。 */
    if (esp_codec_dev_set_out_vol(s_audio.codec_device,
                                  AUDIO_HW_TEST_OUTPUT_VOLUME) != ESP_CODEC_DEV_OK ||
        esp_codec_dev_set_out_mute(s_audio.codec_device, false) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "设置音频自检扬声器音量失败");
        err = ESP_FAIL;
        goto fail;
    }
    s_output_volume = AUDIO_HW_TEST_OUTPUT_VOLUME;

    err = audio_test_reduce_ref_gain();
    if (err != ESP_OK || !audio_test_check_dac()) {
        err = ESP_FAIL;
        goto fail;
    }

    BaseType_t created = xTaskCreatePinnedToCoreWithCaps(
        audio_hardware_test_mic_task, "audio_test_mic",
        AUDIO_HW_TEST_STACK_BYTES, NULL, AUDIO_HW_TEST_TASK_PRIORITY,
        &s_hardware_test_mic_task, AUDIO_HW_TEST_TASK_CORE,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (created == pdPASS) {
        created = xTaskCreatePinnedToCoreWithCaps(
            audio_hardware_test_spk_task, "audio_test_spk",
            AUDIO_HW_TEST_STACK_BYTES, NULL, AUDIO_HW_TEST_TASK_PRIORITY,
            &s_hardware_test_spk_task, AUDIO_HW_TEST_TASK_CORE,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (created != pdPASS) {
        ESP_LOGE(TAG, "创建麦克风/扬声器自检任务失败");
        err = ESP_ERR_NO_MEM;
        goto fail;
    }

    s_hardware_test_started = true;
    ESP_LOGI(TAG,
             "AUDIO_HW_TEST ready：MIC=GPIO%d，每秒打印电平；"
             "SPK持续播放1kHz，音量=%d%%（已显式解除静音）",
             BOARD_AUDIO_DIN, AUDIO_HW_TEST_OUTPUT_VOLUME);
    return ESP_OK;

fail:
    if (s_hardware_test_mic_task != NULL) {
        vTaskDeleteWithCaps(s_hardware_test_mic_task);
        s_hardware_test_mic_task = NULL;
    }
    if (s_hardware_test_spk_task != NULL) {
        vTaskDeleteWithCaps(s_hardware_test_spk_task);
        s_hardware_test_spk_task = NULL;
    }
    heap_caps_free(s_hardware_test_mic_buffer);
    heap_caps_free(s_hardware_test_tone_buffer);
    s_hardware_test_mic_buffer = NULL;
    s_hardware_test_tone_buffer = NULL;
    audio_hw_cleanup();
    return err;
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

    s_capture_guard = xSemaphoreCreateMutex();
    s_encoder_guard = xSemaphoreCreateMutex();
    s_rtc_request_guard = xSemaphoreCreateMutex();
#if defined(CONFIG_CLOUD_PROTOCOL_V3)
    s_voice_events = xQueueCreateWithCaps(12, sizeof(voice_event_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_voice_events || !s_capture_guard || !s_encoder_guard || !s_rtc_request_guard) {
        if (s_voice_events) vQueueDeleteWithCaps(s_voice_events);
#else
    if (!s_capture_guard || !s_encoder_guard || !s_rtc_request_guard) {
#endif
        if (s_capture_guard) vSemaphoreDelete(s_capture_guard);
        if (s_encoder_guard) vSemaphoreDelete(s_encoder_guard);
        if (s_rtc_request_guard) vSemaphoreDelete(s_rtc_request_guard);
        vEventGroupDelete(s_events);
        s_events = NULL;
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
    if (wake_word_init(XIAOZHI_AFE_CHANNELS) == ESP_OK) {
        wake_word_set_callback(wake_word_detected_cb, NULL);
        s_wake_word_ready = true;
#if defined(CONFIG_CLOUD_PROTOCOL_V3)
        /* V3：唤醒词检测是纯本地 WakeNet 运算，不依赖网络 —— 开机即启用
         * （WAKE_IDLE 状态）。唤醒后再按需重新 hello 建立会话
         * （见 v3_open_session_for_wake）。 */
        wake_word_start();
        xEventGroupSetBits(s_events, XIAOZHI_EVENT_WAKE_ACTIVE);
        set_voice_state(VOICE_STATE_WAKE_IDLE);
        ESP_LOGI(TAG, "唤醒词引擎已就绪并启用（WAKE_IDLE），说“你好小智”开始对话");
#else
        ESP_LOGI(TAG, "唤醒词引擎已就绪，等待 WebSocket 连接后启用");
#endif
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
        wake_word_deinit();
        s_wake_word_ready = false;
        delete_voice_control_resources();
        vEventGroupDelete(s_events);
        s_events = NULL;
        return ESP_ERR_NO_MEM;
    }

    /* 回退官方测试时加的启动状态行（用户要求的清晰日志的一部分）。 */
    ESP_LOGI(TAG, "Expression self-test=%s",
             EXPRESSION_SELFTEST ? "ON" : "OFF");

#if EXPRESSION_SELFTEST
    /* 表情链路离线自检，说明见本文件上方；验证完把 EXPRESSION_SELFTEST 改回 0。
     * 创建失败不影响音频服务，只打一条日志。 */
    if (xTaskCreatePinnedToCoreWithCaps(expression_selftest_task, "expr_selftest",
                                       3072, NULL, 2, NULL, tskNO_AFFINITY,
                                       MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGW(TAG, "表情自检任务创建失败，跳过");
    }
#endif
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
