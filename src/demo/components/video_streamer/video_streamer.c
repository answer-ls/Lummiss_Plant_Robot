#include "video_streamer.h"
#include "dma2d_yuv.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "driver/jpeg_decode.h"
#include "esp_h264_alloc.h"
#include "esp_h264_enc_single.h"
#include "esp_h264_enc_single_hw.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_websocket_client.h"

#include "esp_crt_bundle.h"
#include "network_manager.h"

static const char *TAG = "VIDEO_STREAM";

static video_streamer_config_t s_ws_config;

/* WebSocket 地址和动态 Token 必须来自项目 OTA，正式固件不保留明文 WS fallback。 */
/* 分辨率统一取 video_streamer.h 的公共常量，与摄像头侧"可编码帧"门控一致。 */
#define VIDEO_WIDTH                   VIDEO_STREAM_WIDTH
#define VIDEO_HEIGHT                  VIDEO_STREAM_HEIGHT
/* 帧率**上限**，不是目标值：GOP 与 PTS 除数跟随该宏，避免改帧率时漏改。
 * 实际帧率由编解码链耗时决定，通常低于本值——2026-09-12 完整档位实测只有
 * 15～20 fps，瓶颈是 UVC 丢帧（2.6%～32%），不是编解码耗时。
 *
 * 这个宏还有第二个作用，而且更隐蔽：它同时是编码器速率控制的分母
 * （每帧预算 = VIDEO_BITRATE / 本值，详见 VIDEO_BITRATE 处的说明）。
 * 所以它不能随便写大——写成 30 而实际只跑 20，每帧比特预算就被摊薄三分之一，
 * 画质直接变糊。**改这里前先确认它与实测帧率接近。** */
#define VIDEO_ENCODE_FPS              20
#define VIDEO_GOP                     VIDEO_ENCODE_FPS
/* 目标码率。真正决定画质和线上流量的不是这个值本身，而是**每帧比特预算**：
 *
 *     每帧预算 = VIDEO_BITRATE / VIDEO_ENCODE_FPS        ← 编码器速率控制用的口径
 *     线上实际 kbps = 每帧预算 × 实际帧率                  ← 实际帧率通常低于宏值
 *     每像素比特 bpp = 每帧预算 / (宽 × 高)                ← 主观清晰度的直接来源
 *
 * 当前宏值下每帧预算 = 4000000 / 20 = 200 kbit，800×600 合 0.42 bpp。
 *
 * 下面这组实测是 VIDEO_ENCODE_FPS 还写着 30 的时候测的，换了分母之后数字都要
 * 重算，只保留作为"预算会被打满但不会精确打满"的证据：
 * 720p@15.8fps 是 134.4 kbit（当时预算 133.3 kbit，已饱和，说明 QP 压到了
 * qp_min）；800×600@23～26fps 在 83～140 kbit 之间浮动、均值约 118 kbit
 * （略低于预算，因为 QP 还没探到下限）。
 * 两种情况都**不是**被 qp_min 卡住——那是我一度写错的说法。
 *
 * 所以：VIDEO_ENCODE_FPS 一旦与实际帧率不符，线上码率和 bpp 都会跟着跑偏。
 * 调这个宏前先想清楚要动的是"每帧多少比特"，不是"每秒多少比特"。 */
#define VIDEO_BITRATE                 4000000
#define VIDEO_QP_MIN                  20
#define VIDEO_QP_MAX                  40
/* 摄像头侧 MJPEG 输入环槽数（提交时 memcpy 进槽）。 */
#define VIDEO_SLOT_COUNT              3
/* H.264 编码输出（码流）槽数：编码任务写满一个槽就交给发送任务，发送完成归还。
 * 深度要够 WS **单帧耗时峰值**除以帧间隔，否则一次停顿就丢帧：
 * 800×600@25fps（帧间隔 40ms）下 WS 均值 18～28ms 尚可，峰值却到 269.8ms，
 * 4 槽只吸收得了约 160ms —— 于是"输出槽满"从 0 涨到 25（约 1.2%）。
 * 当前收紧到 4 槽，最多吸收约 160ms 的发送抖动；超过这个窗口时，编码任务
 * 会在编码前丢弃输入帧，避免产生无法连续解码的孤立 H.264 P 帧。
 * 注意这只吸收抖动，不消除峰值本身 —— 峰值若持续存在要查码率占用和链路。 */
#define VIDEO_OUT_SLOT_COUNT          4
/* 摄像头 MJPEG 实测为 YUV422 采样；JPEG 硬件直接输出 U Y0 V Y1，16bpp。 */
#define VIDEO_YUV422_SIZE             (VIDEO_WIDTH * VIDEO_HEIGHT * 2)
/* H.264 硬件输入固定为 O_UYY_E_VYY 交错 YUV420，1.5 字节/像素。 */
#define VIDEO_H264_INPUT_SIZE         (VIDEO_WIDTH * VIDEO_HEIGHT * 3 / 2)
/* 压缩码流不需要按原始 YUV 帧大小分配。编码器每帧会使整个输出容量
 * 的缓存失效，过大的容量会延长缓存同步临界区。128 KB 保持 128 字节
 * 对齐，并为当前 4 Mbps/20 FPS 留出突发余量；溢出仍由编码器报错处理。 */
#define VIDEO_H264_OUTPUT_SIZE        (128U * 1024U)

/* 每帧 H.264 码流前置的 16 字节自描述头（小端）：
 *   [0..3]  magic 'L','M','V','1'    [4..5]  width       [6..7]  height
 *   [8..9]  fps                      [10]    frame_type  [11]    reserved
 *   [12..15] sequence
 * frame_type 即 esp_h264_frame_type_t 原值：0=IDR、1=I、2=P。
 * PC 端把 0/1 当作可随机接入的帧（用于预览重同步），不要改动这个映射。
 * 分辨率随每一帧携带，而不是在连接建立时一次性协商，PC 端据此自适应几何参数 ——
 * 将来改分辨率只需要改一处常量，协议和 PC 端都不用动。
 * 编码器直接写进槽内偏移 +16 处，发送时在同一块缓冲头部原地填头，全程零拷贝。 */
#define VIDEO_WS_FRAME_HEADER_SIZE    16
#define VIDEO_WS_FRAME_MAGIC          0x31564D4CU  /* 'L','M','V','1' 小端 */

/* H.264 硬件编码器每编完一帧都要对输出缓冲做一次 cache 失效
 * （esp_h264_enc_dual_hw.c:168 → esp_h264_cache.c:17）。该调用用的是
 * DIR_M2C 且**不带** UNALIGNED 标志，要求起始地址与长度都按 cache line
 * （128 字节）对齐：不满足时每帧刷一条 esp_cache_msync 报错，更要命的是
 * 失效根本没生效——CPU 读硬件 DMA 刚写好的码流时读到的是旧缓存内容。
 * 所以编码输出放在槽内 +128 处（地址与 720000 长度都保持对齐），
 * 16 字节帧头紧贴其前放在 +112，PC 端收到的仍是「帧头 + 紧邻码流」。
 * 切勿把编码缓冲改回 +16 之类的非 128 对齐偏移。 */
#define VIDEO_WS_ALIGN                128U
#define VIDEO_WS_FRAME_HEADER_OFFSET  (VIDEO_WS_ALIGN - VIDEO_WS_FRAME_HEADER_SIZE)  /* 112 */
#define VIDEO_H264_SLOT_SIZE          (VIDEO_WS_ALIGN + VIDEO_H264_OUTPUT_SIZE)

#define VIDEO_TASK_STACK              8192
/* 编解码任务：JPEG 解码 + YUV 重排 + H.264 编码，全程不等待网络。 */
#define VIDEO_CODEC_PRIORITY          8
#define VIDEO_CODEC_CORE              1
/* 网络发送任务：只做 WebSocket 上传与失败处理，与编解码任务并行。 */
#define VIDEO_UPLOAD_PRIORITY         9
#define VIDEO_UPLOAD_CORE             0
#define VIDEO_REPORT_PRIORITY         4
#define VIDEO_REPORT_CORE             0

/* WebSocket 客户端会自建一个任务：钉核 0、优先级低于上传任务（9），
 * 免得收命令和自动重连去抢上传任务的 CPU。 */
#define VIDEO_WS_TASK_STACK           6144
#define VIDEO_WS_TASK_PRIORITY        5
#define VIDEO_WS_TASK_CORE            0
/* WebSocket 收发缓冲大小。这个值直接决定上行一帧被切成几个 WebSocket 分片，
 * 而分片数决定每帧要付多少次运输层操作——这是上行耗时的真正来源。
 *
 * 每发一片，_ws_write()（IDF tcp_transport/transport_ws.c）都要做：
 *   1 次 esp_transport_poll_write + 2 次 esp_transport_write（帧头 + 载荷）
 *   + 2 遍逐字节 XOR（RFC6455 客户端掩码：发前异或、发后异或回来）
 * 每一次 write 都要经 ESP-Hosted 走 SDIO RPC 到 C6。
 *
 * 按 4096 算，实测单帧约 10.8KB 要切 3 片 = 3 poll + 6 write = 9 次 socket 操作；
 * 32KB 只需 1 片 = 1 poll + 2 write = 3 次。实测 WS 单帧 20～35ms、峰值 150ms
 * 主要就是这个开销（10.8KB 走 WiFi 本身用不了这么久）。
 * 注意：memcpy 与 XOR 的总字节数不随分片数变化，唯一变量是操作次数。
 *
 * 选值公式：单帧字节数 = 码率 ÷ 8 ÷ 帧率，与分辨率无关。
 * 32KB 在 20fps 下覆盖到约 5.2 Mbps（4Mbps 时单帧 25KB，1 片发完）。
 * 开了 CONFIG_ESP_WS_CLIENT_ENABLE_DYNAMIC_BUFFER 才会每帧重分配，
 * 当前没开，所以这是启动时一次性 malloc(buffer_size) 的 tx + rx 共 2 份常驻。 */
#define VIDEO_WS_BUFFER_SIZE          32768
/* 断线后 2 秒重试，比组件默认的 10 秒恢复更快。 */
#define VIDEO_WS_RECONNECT_MS         2000
/* 单帧发送超时，也是决定这条连接生死的那个超时。esp_websocket_client 把它
 * 换算成 transport_ws 里**一次** select 可写等待，等不到就返回 ≤0，被
 * esp_websocket_client.c 判为致命错误直接 abort 连接，再等 VIDEO_WS_RECONNECT_MS
 * 才重连——所以这个值必须大于"最坏情况下等一个写窗口"的时间。
 *
 * 100 ms 实测不够：单帧 30 KB 以上时 "poll 说可写" 不再保证装得下一帧
 * （TCP_SNDLOWAT = TCP_SND_BUF/2 = 32767 字节），2026-09-12 的完整档位日志里
 * 因此出现连续 5 次"连上不到 1 秒又断"，累计 56 秒黑屏。放宽到 2 秒的代价是
 * 阻塞期间占住输出槽，但断线代价是重连 2 秒起，取小的那个。 */
#define VIDEO_WS_SEND_TIMEOUT_MS      2000
#define VIDEO_WS_AUDIO_SEND_TIMEOUT_MS 200
/* 建连超时。名字看着像"轮询超时"，但 esp_websocket_client.c:1204 把它原样传给
 * esp_transport_connect(host, port, network_timeout_ms)，TLS 路径下即
 * esp_tls_cfg_t.timeout_ms——**整个 TLS 握手的超时预算**。
 *
 * 另有三处用途：读轮询（:1085）、PING/PONG/CLOSE（:1142/:1306/:1359）。
 * 数据帧发送不走它：send_with_opcode 用的是调用方传进来的 timeout（:740），
 * 那才是 VIDEO_WS_SEND_TIMEOUT_MS。所以这两个宏仍然要分开，只是不能把建连
 * 那一侧压得太短。
 *
 * 100 是错的：公网到 www.lummiss.com 的 TLS 握手偶尔能挤进 100 ms（于是首次
 * 连接侥幸成功），但大多数时候超时，报
 *   esp-tls: Failed to open new connection in specified timeout
 *   transport_error=ESP_ERR_ESP_TLS_CONNECTION_TIMEOUT
 * 且**每次重连都失败**——现象就是连上一次之后再也连不上，OTA 明明还是通的
 * （一次性 HTTPS 请求不吃这个超时）。恢复组件默认的 10 秒。 */
#define VIDEO_WS_NETWORK_TIMEOUT_MS   10000
/* 发送起始速率的闸门。跟随 VIDEO_ENCODE_FPS 而不是写死数字：上游 20 fps 的
 * 提交门控已经限过流，这里再拿一个过时的 25 去放行只会让两处口径对不上。 */
#define VIDEO_WS_SEND_INTERVAL_US     (1000000LL / VIDEO_ENCODE_FPS)
/* 下行命令的累积缓冲：服务器下发的 JSON 命令很短。 */
#define VIDEO_WS_COMMAND_MAX          1024
#define VIDEO_WS_AUDIO_PACKET_MAX     1400
#define VIDEO_WS_AUDIO_QUEUE_DEPTH    4
#define VIDEO_REPORT_INTERVAL_US      (5 * 1000 * 1000LL)
/* 门控按 VIDEO_ENCODE_FPS 的帧周期推进；允许提前 5 ms 接帧，
 * 避免整数取整导致的周期漂移。 */
#define VIDEO_SUBMIT_EARLY_US         5000

typedef struct {
    uint8_t *data;
    const uint8_t *input;
    size_t data_len;   /* 本帧 JPEG 压缩数据长度（可变），解码按它喂位流 */
    uint32_t sequence;
    int64_t submitted_us;
    bool busy;
    video_streamer_input_release_cb_t release_cb;
    void *release_ctx;
} video_input_slot_t;

/* H.264 输出槽：编码任务写入码流后连同元数据交给发送任务。
 * data_len/sequence/pts/frame_type 由编码任务填写，发送任务只读。 */
typedef struct {
    uint8_t *data;
    size_t data_len;
    uint32_t sequence;
    uint32_t pts;
    int frame_type;
    int64_t ready_us;
} video_out_slot_t;

typedef struct {
    uint16_t size;
    uint8_t data[VIDEO_WS_AUDIO_PACKET_MAX];
} video_agent_audio_packet_t;

/* 结构校验的失败原因。camera_driver 侧只检查 SOI/EOI 是否在场，而等时传输
 * 丢包打坏帧头段长度、头尾标记却仍然完好的帧只有这里能拦住。把原因拆开计数
 * 才能把"整帧丢失"和"帧内损坏"两种丢帧分开归因。 */
typedef enum {
    VIDEO_JPEG_OK = 0,
    VIDEO_JPEG_ERR_HEAD,     /* 长度不足，或缺 SOI */
    VIDEO_JPEG_ERR_MARKER,   /* 该是 0xFF 的地方不是，或头部出现非法 marker */
    VIDEO_JPEG_ERR_SEGMENT,  /* 段长度 <2，或越过帧尾 */
    VIDEO_JPEG_ERR_SOF,      /* 非顺序 SOF，或尺寸不符 / SOS 前没有 SOF */
    VIDEO_JPEG_ERR_NO_SOS,   /* 头部走完仍没见到 SOS */
    VIDEO_JPEG_ERR_SCAN,     /* 熵编码区出现非法 marker，或走完仍未终止 */
    VIDEO_JPEG_ERR_NO_EOI,   /* 尾扫没找到 FFD9，未进函数直接判定 */
    VIDEO_JPEG_ERR_COUNT,
} video_jpeg_validate_result_t;

static const char *const k_jpeg_invalid_names[VIDEO_JPEG_ERR_COUNT] = {
    "ok", "head", "marker", "segment", "sof", "no_sos", "scan", "no_eoi",
};

typedef struct {
    uint32_t submitted;
    uint32_t rate_limited;
    uint32_t dropped;        /* 输入槽满：提交时 MJPEG 环槽全忙，编解码任务跟不上 */
    uint32_t slot_dropped;   /* 输出槽满：编码前拿不到空闲输出槽，上传任务跟不上 */
    /* 结构校验不通过的输入帧：有 SOI/EOI，但 Marker 结构或尺寸损坏，未送进
     * JPEG 解码器。由编解码任务统计（见 video_jpeg_structurally_valid）。 */
    uint32_t input_invalid;
    /* 上面那个总数的原因拆分，索引取 video_jpeg_validate_result_t。 */
    uint32_t invalid_reason[VIDEO_JPEG_ERR_COUNT];
    uint32_t jpeg_decoded;
    uint32_t yuv_converted;
    uint32_t encoded;
    uint32_t sent;
    uint32_t send_failed;
    uint64_t encoded_bytes;
    uint64_t jpeg_decode_us;
    uint64_t input_queue_us;
    uint64_t validation_us;
    uint64_t yuv_repack_us;
    uint64_t h264_encode_us;
    uint32_t h264_max_us;     /* 报告周期内 H.264 单帧最长耗时（重置式） */
    uint64_t output_queue_us;
    uint64_t ws_send_us;
    uint32_t send_attempts;
    uint32_t upload_received;
    uint64_t ws_max_us;      /* 报告周期内 WebSocket 单帧最长耗时（重置式） */
} video_stream_stats_t;

static video_input_slot_t s_slots[VIDEO_SLOT_COUNT];
static video_out_slot_t s_out_slots[VIDEO_OUT_SLOT_COUNT];
static QueueHandle_t s_input_queue;
/* 输出槽所有权两条队列：free=可写槽，ready=已编码待发送槽。 */
static QueueHandle_t s_out_free;
static QueueHandle_t s_out_ready;
static QueueHandle_t s_agent_audio_queue;
static StaticQueue_t s_agent_audio_queue_control;
static uint8_t *s_agent_audio_queue_storage;
static QueueSetHandle_t s_upload_queue_set;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static video_stream_stats_t s_stats;
static int64_t s_next_submit_us;
static uint32_t s_next_sequence;
static bool s_initialized;
static bool s_stream_enabled = true;
/* 发送任务和 UVC 回调都读取该状态；断线时在入口丢帧，避免 JPEG 解码、
 * H.264 编码和输出槽继续为一个已经不可写的 socket 消耗资源。 */
static volatile bool s_ws_connected;
static volatile bool s_yuv_busy;
static int64_t s_next_ws_send_us;
static esp_websocket_client_handle_t s_ws_client;
static video_streamer_agent_callbacks_t s_agent_callbacks;

/* 下行命令的累积缓冲。WebSocket 事件回调运行在客户端自己的任务里，
 * 与上传任务并发，但这两个变量只有该回调会写，因此不需要加锁。 */
static char s_ws_command[VIDEO_WS_COMMAND_MAX];
static size_t s_ws_command_len;
static uint8_t s_ws_audio_packet[VIDEO_WS_AUDIO_PACKET_MAX];
static size_t s_ws_audio_len;
static bool s_ws_receiving_binary;

static void video_drop_pending_output(void)
{
    unsigned index;
    uint32_t dropped = 0;
    while (xQueueReceive(s_out_ready, &index, 0) == pdTRUE) {
        if (xQueueSend(s_out_free, &index, 0) == pdTRUE) {
            dropped++;
        }
    }
    if (dropped != 0) {
        ESP_LOGW(TAG, "WebSocket 不可用，丢弃已编码旧帧=%" PRIu32, dropped);
    }
}

/* 限制发送调用的启动频率。它不改变当前低于 25fps 的编码速率，只防止网络
 * 短暂恢复后多个待发槽连续冲击 ESP-Hosted 的 SDIO/WiFi TX 缓冲池。 */
static void video_upload_pace(void)
{
    const int64_t now_us = esp_timer_get_time();
    int64_t next_us = s_next_ws_send_us;
    if (next_us > now_us) {
        const int64_t wait_us = next_us - now_us;
        const TickType_t wait_ticks = pdMS_TO_TICKS((wait_us + 999) / 1000);
        if (wait_ticks > 0) {
            vTaskDelay(wait_ticks);
        }
    }

    const int64_t after_wait_us = esp_timer_get_time();
    s_next_ws_send_us = after_wait_us + VIDEO_WS_SEND_INTERVAL_US;
}

/* 归还由外部持有的输入帧。调用方在进入编解码任务后独占该槽，
 * 因此回调前先摘下 owner，再把槽标记为空闲，避免 UVC 缓冲过早复用。 */
static void video_input_slot_release(video_input_slot_t *slot)
{
    video_streamer_input_release_cb_t release_cb = slot->release_cb;
    void *release_ctx = slot->release_ctx;
    slot->release_cb = NULL;
    slot->release_ctx = NULL;
    slot->input = NULL;
    slot->data_len = 0;

    if (release_cb != NULL) {
        release_cb(release_ctx);
    }

    portENTER_CRITICAL(&s_lock);
    slot->busy = false;
    portEXIT_CRITICAL(&s_lock);
}

/* 网络或服务器异常时最多每 5 秒输出一次详情，避免 20 FPS 的失败日志刷屏。 */
static void video_stream_log_error_limited(const char *message, int error)
{
    static int64_t last_log_us;
    const int64_t now_us = esp_timer_get_time();
    if (last_log_us == 0 || now_us - last_log_us >= 5 * 1000 * 1000LL) {
        ESP_LOGW(TAG, "%s：%d", message, error);
        last_log_us = now_us;
    }
}

/* WEBSOCKET_EVENT_ERROR 的 error_handle 是 esp_websocket_error_codes_t：前三个
 * 字段是"与 esp_tls_last_error 兼容的部分"，只在 esp-tls 层面失败时才被填；
 * 后面的 error_type / esp_ws_handshake_status_code / esp_transport_sock_errno
 * 是 esp-websocket 与 tcp_transport 的扩展，走 TLS 连接失败这条路时**不会赋值**。
 *
 * 这里原先读的恰好是最后的 esp_transport_sock_errno，于是打出
 * "WebSocket 出错：1341551172" ——那不是错误码，是未初始化内存里残留的 PSRAM
 * 地址，把真正的 ESP_ERR_ESP_TLS_CONNECTION_TIMEOUT 藏掉了。改报前三个字段。
 * 仍按 5 秒限频：断线重连期间这个事件每 2 秒来一次。 */
static void video_stream_log_ws_error(const esp_websocket_event_data_t *data)
{
    static int64_t last_log_us;
    const int64_t now_us = esp_timer_get_time();
    if (last_log_us != 0 && now_us - last_log_us < 5 * 1000 * 1000LL) {
        return;
    }
    last_log_us = now_us;
    if (data == NULL) {
        ESP_LOGW(TAG, "WebSocket 出错：无错误详情");
        return;
    }
    ESP_LOGW(TAG,
             "WebSocket 出错：type=%d last_err=%s(0x%x) tls_stack_err=%d handshake=%d",
             (int)data->error_handle.error_type,
             esp_err_to_name(data->error_handle.esp_tls_last_esp_err),
             (unsigned)data->error_handle.esp_tls_last_esp_err,
             data->error_handle.esp_tls_stack_err,
             data->error_handle.esp_ws_handshake_status_code);
}

/*
 * JPEG 硬件解码器对输入格式比普通软件解码器严格：只确认 SOI/EOI 齐全仍可能把
 * 丢包后的半截 JPEG 送进去，随后就是 "data units 数量与软件配置的分辨率不符"、
 * unsupported marker，以及连带的 dma2d_force_end 报错。
 *
 * 这段校验原先写在 UVC 帧回调里，现在搬到本任务：扫描整帧是 O(帧长) 的工作，
 * 而帧回调运行在 USB 等时传输的上下文里，每多占一毫秒，transfer 就晚一毫秒
 * 重新入队——等时传输没有重传，那段时间的包就是白丢的。搬过来之后 USB 回调
 * 只剩常数时间的工作，硬件解码器受到的防护强度不变。
 *
 * 只走一遍 Marker 结构，不解码像素数据；帧尾允许存在填充字节。
 *
 * 返回失败原因而不是布尔值，原因枚举见 video_jpeg_validate_result_t。 */
static video_jpeg_validate_result_t video_jpeg_structurally_valid(const uint8_t *data,
                                                                  size_t data_len)
{
    if (data == NULL || data_len < 4 || data[0] != 0xff || data[1] != 0xd8) {
        return VIDEO_JPEG_ERR_HEAD;
    }

    size_t pos = 2;
    bool saw_sof = false;
    bool saw_sos = false;

    /* Parse headers until Start Of Scan. */
    while (pos < data_len) {
        if (data[pos++] != 0xff) {
            return VIDEO_JPEG_ERR_MARKER;
        }
        while (pos < data_len && data[pos] == 0xff) {
            ++pos;
        }
        if (pos >= data_len) {
            return VIDEO_JPEG_ERR_MARKER;
        }

        const uint8_t marker = data[pos++];
        if (marker == 0x00 || marker == 0xd8 || marker == 0xd9 ||
            (marker >= 0xd0 && marker <= 0xd7)) {
            return VIDEO_JPEG_ERR_MARKER;
        }

        if (pos + 2 > data_len) {
            return VIDEO_JPEG_ERR_SEGMENT;
        }
        const size_t segment_length = ((size_t)data[pos] << 8) | data[pos + 1];
        if (segment_length < 2 || pos + segment_length > data_len) {
            return VIDEO_JPEG_ERR_SEGMENT;
        }

        /* ESP32-P4 JPEG hardware accepts sequential SOF, not progressive SOF. */
        const bool is_sof = (marker >= 0xc0 && marker <= 0xcf &&
                             marker != 0xc4 && marker != 0xc8 && marker != 0xcc);
        if (is_sof) {
            if ((marker != 0xc0 && marker != 0xc1) || segment_length < 8) {
                return VIDEO_JPEG_ERR_SOF;
            }
            const unsigned height = ((unsigned)data[pos + 3] << 8) | data[pos + 4];
            const unsigned width = ((unsigned)data[pos + 5] << 8) | data[pos + 6];
            if (width != VIDEO_WIDTH || height != VIDEO_HEIGHT) {
                return VIDEO_JPEG_ERR_SOF;
            }
            saw_sof = true;
        }

        if (marker == 0xda) { /* SOS */
            if (!saw_sof || segment_length < 6) {
                return VIDEO_JPEG_ERR_SOF;
            }
            pos += segment_length;
            saw_sos = true;
            break;
        }

        pos += segment_length;
    }

    if (!saw_sos) {
        return VIDEO_JPEG_ERR_NO_SOS;
    }

    /* Parse entropy-coded data. Inside the scan only stuffed bytes, restart
     * markers and EOI are legal. Any other marker means the frame is corrupt
     * or uses a JPEG feature unsupported by the P4 hardware decoder. */
    while (pos < data_len) {
        if (data[pos++] != 0xff) {
            continue;
        }
        while (pos < data_len && data[pos] == 0xff) {
            ++pos;
        }
        if (pos >= data_len) {
            return VIDEO_JPEG_ERR_SCAN;
        }

        const uint8_t marker = data[pos++];
        if (marker == 0x00 || (marker >= 0xd0 && marker <= 0xd7)) {
            continue;
        }
        if (marker == 0xd9) { /* EOI */
            return VIDEO_JPEG_OK;
        }
        return VIDEO_JPEG_ERR_SCAN;
    }

    return VIDEO_JPEG_ERR_SCAN;
}

/* 只在编解码任务中截断 UVC 帧尾的填充字节。这个扫描不能放回 USB 回调，
 * 否则一张损坏帧缺少 EOI 时会把回调长时间占住。 */
static size_t video_jpeg_find_eoi(const uint8_t *data, size_t data_len)
{
    if (data == NULL || data_len < 2) {
        return 0;
    }
    for (size_t end = data_len; end >= 2; --end) {
        if (data[end - 2] == 0xff && data[end - 1] == 0xd9) {
            return end;
        }
    }
    return 0;
}

/* ESP32-P4 v1.x 的 H.264 硬件输入格式为交错 YUV420（O_UYY_E_VYY）。
 * 如果相机 JPEG 本身就是 YUV420，JPEG 解码器可以直接输出同一布局，
 * 走零拷贝；YUV422 首选 DMA2D CSC，下面的 CPU 函数只作故障回退。
 * 这里保留 IRAM + 手工展开实现；不能在 PSRAM 数据路径中使用逐次
 * memcpy 打包，否则编译结果会退化为大量小块复制。
 *
 * 转换按 8 个 2 行宏块分段。每段只连续读写约 45 KB（800x600），
 * 段间主动让出一次调度并插入很短的总线空隙，避免 CPU 长时间占住
 * PSRAM 访问窗口，给 USB Host/ISOC DMA 留出仲裁机会。 */
#define VIDEO_YUV_TILE_ROW_PAIRS      8U
#define VIDEO_YUV_TILE_GAP_US         20U
static void IRAM_ATTR yuv422_to_h264_yuv420(const uint8_t *restrict src,
                                            uint8_t *restrict dst)
{
    const size_t src_line_bytes = VIDEO_WIDTH * 2;
    const size_t dst_line_bytes = VIDEO_WIDTH * 3 / 2;
    const unsigned row_pairs = VIDEO_HEIGHT / 2;

    for (unsigned tile_start = 0; tile_start < row_pairs;
         tile_start += VIDEO_YUV_TILE_ROW_PAIRS) {
        unsigned tile_end = tile_start + VIDEO_YUV_TILE_ROW_PAIRS;
        if (tile_end > row_pairs) {
            tile_end = row_pairs;
        }

        for (unsigned by = tile_start; by < tile_end; ++by) {
            const uint8_t *src_even = src + (by * 2) * src_line_bytes;
            const uint8_t *src_odd = src_even + src_line_bytes;
            uint8_t *dst_even = dst + (by * 2) * dst_line_bytes;
            uint8_t *dst_odd = dst_even + dst_line_bytes;

            /* 每次展开 4 个 2x2 块（8 个像素）。restrict 让编译器知道输入
             * 和输出不别名；IRAM_ATTR 避免这段高频循环依赖 PSRAM 指令取数。 */
            unsigned bx = 0;
            for (; bx + 4 <= VIDEO_WIDTH / 2; bx += 4) {
                /* 输入：U0 Y00 V0 Y01 / U1 Y10 V1 Y11。 */
                dst_even[0] = (uint8_t)(((unsigned)src_even[0] + src_odd[0] + 1) >> 1);
                dst_even[1] = src_even[1];
                dst_even[2] = src_even[3];
                dst_odd[0] = (uint8_t)(((unsigned)src_even[2] + src_odd[2] + 1) >> 1);
                dst_odd[1] = src_odd[1];
                dst_odd[2] = src_odd[3];
                dst_even[3] = (uint8_t)(((unsigned)src_even[4] + src_odd[4] + 1) >> 1);
                dst_even[4] = src_even[5];
                dst_even[5] = src_even[7];
                dst_odd[3] = (uint8_t)(((unsigned)src_even[6] + src_odd[6] + 1) >> 1);
                dst_odd[4] = src_odd[5];
                dst_odd[5] = src_odd[7];
                dst_even[6] = (uint8_t)(((unsigned)src_even[8] + src_odd[8] + 1) >> 1);
                dst_even[7] = src_even[9];
                dst_even[8] = src_even[11];
                dst_odd[6] = (uint8_t)(((unsigned)src_even[10] + src_odd[10] + 1) >> 1);
                dst_odd[7] = src_odd[9];
                dst_odd[8] = src_odd[11];
                dst_even[9] = (uint8_t)(((unsigned)src_even[12] + src_odd[12] + 1) >> 1);
                dst_even[10] = src_even[13];
                dst_even[11] = src_even[15];
                dst_odd[9] = (uint8_t)(((unsigned)src_even[14] + src_odd[14] + 1) >> 1);
                dst_odd[10] = src_odd[13];
                dst_odd[11] = src_odd[15];

                src_even += 16;
                src_odd += 16;
                dst_even += 12;
                dst_odd += 12;
            }
            for (; bx < VIDEO_WIDTH / 2; ++bx) {
                dst_even[0] = (uint8_t)(((unsigned)src_even[0] + src_odd[0] + 1) >> 1);
                dst_even[1] = src_even[1];
                dst_even[2] = src_even[3];
                dst_odd[0] = (uint8_t)(((unsigned)src_even[2] + src_odd[2] + 1) >> 1);
                dst_odd[1] = src_odd[1];
                dst_odd[2] = src_odd[3];
                src_even += 4;
                src_odd += 4;
                dst_even += 3;
                dst_odd += 3;
            }
        }

        if (tile_end < row_pairs) {
            taskYIELD();
            esp_rom_delay_us(VIDEO_YUV_TILE_GAP_US);
        }
    }
}

/* 就地把 16 字节帧头写进输出槽开头（小端）。用 memcpy 而非指针强转，
 * 不依赖对齐假设；RISC-V 本身是小端，布局与 PC 端解析一致。 */
static void video_ws_frame_header_fill(uint8_t *header,
                                       uint32_t sequence,
                                       int frame_type)
{
    const uint32_t magic = VIDEO_WS_FRAME_MAGIC;
    const uint16_t width = VIDEO_WIDTH;
    const uint16_t height = VIDEO_HEIGHT;
    const uint16_t fps = VIDEO_ENCODE_FPS;

    memcpy(header + 0, &magic, sizeof(magic));
    memcpy(header + 4, &width, sizeof(width));
    memcpy(header + 6, &height, sizeof(height));
    memcpy(header + 8, &fps, sizeof(fps));
    header[10] = (uint8_t)frame_type;
    header[11] = 0;  /* 保留字段 */
    memcpy(header + 12, &sequence, sizeof(sequence));
}

/* 从 {"type":"XXX"} 或 {"cmd":"XXX"} 里取出命令字。
 * 优先解析 "type"（云端协议），找不到时回退到 "cmd"（旧版 PC 调试协议）。
 * 格式固定且简单，手写解析即可，不为它引入 JSON 依赖。
 * 返回 false 表示格式不符合预期。 */
static bool video_ws_extract_cmd(const char *json, char *out, size_t out_size)
{
    const char *key = strstr(json, "\"type\"");
    if (key == NULL) {
        key = strstr(json, "\"cmd\"");
    }
    if (key == NULL) {
        return false;
    }
    const char *colon = strchr(key + 5, ':');
    if (colon == NULL) {
        return false;
    }
    const char *open = strchr(colon + 1, '"');
    if (open == NULL) {
        return false;
    }
    const char *close = strchr(open + 1, '"');
    if (close == NULL) {
        return false;
    }

    const size_t length = (size_t)(close - open - 1);
    if (length == 0 || length >= out_size) {
        return false;
    }
    memcpy(out, open + 1, length);
    out[length] = '\0';
    return true;
}

void video_streamer_set_enabled(bool enabled)
{
    portENTER_CRITICAL(&s_lock);
    s_stream_enabled = enabled;
    portEXIT_CRITICAL(&s_lock);
    ESP_LOGI(TAG, "视频上传流已%s", enabled ? "开启" : "关闭");
}

bool video_streamer_is_enabled(void)
{
    bool enabled;
    portENTER_CRITICAL(&s_lock);
    enabled = s_stream_enabled;
    portEXIT_CRITICAL(&s_lock);
    return enabled;
}

/* 处理服务器下发的控制命令并回包。
 * 支持新旧两套协议：{"type":"..."}（云端）与 {"cmd":"..."}（旧 PC 调试）。 */
static void video_ws_handle_command(esp_websocket_client_handle_t client,
                                    const char *command)
{
    char cmd[16];
    char reply[256];

    if (!video_ws_extract_cmd(command, cmd, sizeof(cmd))) {
        /* 服务端 Hello 包含 session_id 和 audio_params，不是简单命令格式。
         * 只打日志保存，不回复。后续音频联调时再从 JSON 中提取参数。 */
        if (strstr(command, "\"session_id\"") != NULL) {
            ESP_LOGI(TAG, "服务端 Hello：%s", command);
        }
        return;
    }

    if (strcmp(cmd, "ping") == 0) {
        snprintf(reply, sizeof(reply),
                 "{\"type\":\"pong\",\"version\":1}");
    } else if (strcmp(cmd, "hello") == 0) {
        /* 服务端 Hello 响应，包含 session_id 和音频参数，打日志后不回复。 */
        ESP_LOGI(TAG, "服务端 Hello：%s", command);
        if (strstr(command, "\"session_id\"") != NULL) {
            const char *sid = strstr(command, "\"session_id\"");
            ESP_LOGI(TAG, "会话已建立%s",
                     sid ? "（session_id 已提取）" : "");
        }
        return;
    } else if (strcmp(cmd, "status") == 0) {
        portENTER_CRITICAL(&s_lock);
        const uint32_t encoded = s_stats.encoded;
        const uint32_t sent = s_stats.sent;
        const uint32_t failed = s_stats.send_failed;
        const uint32_t dropped = s_stats.dropped + s_stats.slot_dropped;
        const bool enabled = s_stream_enabled;
        portEXIT_CRITICAL(&s_lock);

        snprintf(reply, sizeof(reply),
                 "{\"type\":\"status\",\"ok\":true,"
                 "\"width\":%d,\"height\":%d,"
                 "\"fps\":%d,\"encoded\":%" PRIu32 ",\"sent\":%" PRIu32
                 ",\"failed\":%" PRIu32 ",\"dropped\":%" PRIu32
                 ",\"video_enabled\":%s}",
                 VIDEO_WIDTH, VIDEO_HEIGHT, VIDEO_ENCODE_FPS,
                 encoded, sent, failed, dropped, enabled ? "true" : "false");
    } else if (strcmp(cmd, "video_on") == 0 || strcmp(cmd, "video_off") == 0) {
        const bool enabled = strcmp(cmd, "video_on") == 0;
        video_streamer_set_enabled(enabled);
        snprintf(reply, sizeof(reply),
                 "{\"type\":\"video_state\",\"ok\":true,"
                 "\"video_enabled\":%s}",
                 enabled ? "true" : "false");
    } else if (strcmp(cmd, "tts") == 0 || strcmp(cmd, "stt") == 0 ||
               strcmp(cmd, "listen") == 0 || strcmp(cmd, "abort") == 0) {
        /* 语音协议交给已注册的 Agent 回调处理，此处不重复打印。 */
        return;
    } else {
        /* 未知命令：不回复，避免在云端协议下产生噪音。
         * tts/stt/listen/abort 等语音/控制协议命令在此接收但暂不处理。 */
        ESP_LOGI(TAG, "未处理的下行命令：%s", command);
        return;
    }

    ESP_LOGI(TAG, "下行命令 %s → %s", command, reply);
    if (esp_websocket_client_send_text(client, reply, (int)strlen(reply),
                                       pdMS_TO_TICKS(VIDEO_WS_SEND_TIMEOUT_MS)) < 0) {
        ESP_LOGW(TAG, "回发命令响应失败");
    }
}

/* WebSocket 事件回调。由客户端的任务在派发事件前主动释放 client->lock 后调用
 * （见组件连接态分支），所以这里直接 send_text 回包不会自死锁。
 * 只做轻量处理：收命令、打日志，不做任何等待。 */
static void video_ws_event_handler(void *handler_args,
                                   esp_event_base_t base,
                                   int32_t event_id,
                                   void *event_data)
{
    (void)handler_args;
    (void)base;
    const esp_websocket_event_data_t *data = (const esp_websocket_event_data_t *)event_data;

    switch ((esp_websocket_event_id_t)event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        __atomic_store_n(&s_ws_connected, true, __ATOMIC_SEQ_CST);
        ESP_LOGI(TAG, "WebSocket 已连接：%s", s_ws_config.ws_url);

        /* 发送应用层 Hello，遵循 OTA 与 WebSocket 接口规范。 */
        {
            char hello[512];
            snprintf(hello, sizeof(hello),
                     "{\"type\":\"hello\",\"version\":1,"
                     "\"transport\":\"websocket\","
                     "\"features\":{\"mcp\":false,\"aec\":false,"
                     "\"emoji\":false},"
                     "\"capability_manifest\":{"
                     "\"variantCode\":\"DESKTOP_PET_V1\","
                     "\"manifestVersion\":1,"
                     "\"capabilities\":["
                     "{\"code\":\"audio.play\",\"version\":1},"
                     "{\"code\":\"camera.capture\",\"version\":1}"
                     "]}}");
            int sent = esp_websocket_client_send_text(
                data->client, hello, (int)strlen(hello),
                pdMS_TO_TICKS(VIDEO_WS_SEND_TIMEOUT_MS));
            if (sent > 0) {
                ESP_LOGI(TAG, "Hello 已发送");
            } else {
                ESP_LOGW(TAG, "Hello 发送失败：%d", sent);
            }
        }
        if (s_agent_callbacks.connection_changed != NULL) {
            s_agent_callbacks.connection_changed(true, s_agent_callbacks.ctx);
        }
        break;

    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_CLOSED:
        /* 重连由组件负责，这里只复位可能残留的半条命令。 */
        __atomic_store_n(&s_ws_connected, false, __ATOMIC_SEQ_CST);
        ESP_LOGW(TAG, "WebSocket 连接断开，等待自动重连");
        s_ws_command_len = 0;
        s_ws_audio_len = 0;
        s_ws_receiving_binary = false;
        if (s_agent_callbacks.connection_changed != NULL) {
            s_agent_callbacks.connection_changed(false, s_agent_callbacks.ctx);
        }
        break;

    case WEBSOCKET_EVENT_ERROR:
        __atomic_store_n(&s_ws_connected, false, __ATOMIC_SEQ_CST);
        video_stream_log_ws_error(data);
        break;

    case WEBSOCKET_EVENT_DATA:
        /* 文本帧承载 Hello/STT/TTS/控制；服务端下行二进制帧是一个完整
         * Opus packet。两类消息都可能被组件按 payload_offset 分片。 */
        if (data == NULL) {
            break;
        }
        if (data->op_code == 0x02 && data->payload_offset == 0) {
            s_ws_audio_len = 0;
            s_ws_receiving_binary = true;
        }
        if (data->op_code == 0x02 ||
            (data->op_code == 0x00 && s_ws_receiving_binary)) {
            const size_t offset = (size_t)data->payload_offset;
            const size_t chunk = (size_t)data->data_len;
            if (offset + chunk > sizeof(s_ws_audio_packet)) {
                ESP_LOGW(TAG, "下行 Opus 包过大，已丢弃：%u bytes",
                         (unsigned)(offset + chunk));
                s_ws_audio_len = 0;
                s_ws_receiving_binary = false;
                break;
            }
            memcpy(s_ws_audio_packet + offset, data->data_ptr, chunk);
            s_ws_audio_len = offset + chunk;
            if (data->fin) {
                if (s_agent_callbacks.audio_received != NULL) {
                    s_agent_callbacks.audio_received(
                        s_ws_audio_packet, s_ws_audio_len,
                        s_agent_callbacks.ctx);
                }
                s_ws_audio_len = 0;
                s_ws_receiving_binary = false;
            }
            break;
        }
        if (data->op_code != 0x01 && data->op_code != 0x00) {
            break;
        }
        s_ws_receiving_binary = false;
        if (data->op_code == 0x01 && data->payload_offset == 0) {
            s_ws_command_len = 0;
        }
        if (s_ws_command_len + (size_t)data->data_len >= sizeof(s_ws_command)) {
            s_ws_command_len = 0;
            ESP_LOGW(TAG, "下行命令过长，已丢弃");
            break;
        }
        memcpy(s_ws_command + s_ws_command_len, data->data_ptr, (size_t)data->data_len);
        s_ws_command_len += (size_t)data->data_len;
        if (data->fin) {
            s_ws_command[s_ws_command_len] = '\0';

            /* 拦截鉴权失败文本（服务端在鉴权失败时先发非 JSON 文本再关连接）。
             * 清除旧 token 日志，方便排查是否需要重新 OTA。 */
            if (strstr(s_ws_command, "认证失败") != NULL) {
                ESP_LOGE(TAG, "WebSocket 鉴权失败！请检查 Device-Id / Client-Id / Token 是否与 OTA 一致");
            }

            video_ws_handle_command(data->client, s_ws_command);
            if (s_agent_callbacks.text_received != NULL) {
                s_agent_callbacks.text_received(
                    s_ws_command, s_ws_command_len,
                    s_agent_callbacks.ctx);
            }
            s_ws_command_len = 0;
        }
        break;

    default:
        break;
    }
}

static void video_stream_report(int64_t now_us)
{
    static int64_t last_report_us;
    static video_stream_stats_t previous;
    if (last_report_us == 0) {
        last_report_us = now_us;
        return;
    }
    if (now_us - last_report_us < VIDEO_REPORT_INTERVAL_US) {
        return;
    }

    video_stream_stats_t current;
    portENTER_CRITICAL(&s_lock);
    current = s_stats;
    /* 报告周期内的最大耗时：读出后清零，只统计当前窗口。 */
    s_stats.ws_max_us = 0;
    s_stats.h264_max_us = 0;
    portEXIT_CRITICAL(&s_lock);

    const double seconds = (now_us - last_report_us) / 1000000.0;
    const uint32_t encoded_delta = current.encoded - previous.encoded;
    /* decoded_delta / yuv_delta 只被下面 System B 的隔离档位日志用到。两个宏都是 0
     * 时它们声明了却没人读，会被 -Wunused-variable 点名。所以跟着各自的 #if 出现。 */
#if VIDEO_STREAM_JPEG_ONLY_TEST || VIDEO_STREAM_YUV_ONLY_TEST
    const uint32_t decoded_delta = current.jpeg_decoded - previous.jpeg_decoded;
#endif
#if VIDEO_STREAM_YUV_ONLY_TEST
    const uint32_t yuv_delta = current.yuv_converted - previous.yuv_converted;
#endif
    const uint32_t sent_delta = current.sent - previous.sent;
    const uint64_t bytes_delta = current.encoded_bytes - previous.encoded_bytes;
    const uint64_t decode_us_delta = current.jpeg_decode_us - previous.jpeg_decode_us;
    const uint64_t input_queue_us_delta = current.input_queue_us - previous.input_queue_us;
    const uint64_t validation_us_delta = current.validation_us - previous.validation_us;
    const uint64_t repack_us_delta = current.yuv_repack_us - previous.yuv_repack_us;
    const uint64_t encode_us_delta = current.h264_encode_us - previous.h264_encode_us;
    const uint64_t output_queue_us_delta = current.output_queue_us - previous.output_queue_us;
    const uint64_t send_us_delta = current.ws_send_us - previous.ws_send_us;
    const uint32_t send_attempts_delta = current.send_attempts - previous.send_attempts;
    const uint32_t upload_received_delta = current.upload_received - previous.upload_received;
    const uint32_t processed_delta =
#if VIDEO_STREAM_JPEG_ONLY_TEST
        decoded_delta;
#elif VIDEO_STREAM_YUV_ONLY_TEST
        yuv_delta;
#else
        encoded_delta;
#endif
    const double sample_count = processed_delta > 0 ? processed_delta : 1;
    const UBaseType_t input_used = s_input_queue != NULL ? uxQueueMessagesWaiting(s_input_queue) : 0;
    const UBaseType_t input_capacity = s_input_queue != NULL ?
                                       uxQueueSpacesAvailable(s_input_queue) + input_used : 0;
    const UBaseType_t output_used = s_out_ready != NULL ? uxQueueMessagesWaiting(s_out_ready) : 0;
    const UBaseType_t output_capacity = s_out_ready != NULL ?
                                        uxQueueSpacesAvailable(s_out_ready) + output_used : 0;

#if VIDEO_STREAM_JPEG_ONLY_TEST
    ESP_LOGI(TAG,
             "[VIDEO] JPEG decoded=%.1f fps encoded=%.1f fps sent=%.1f fps",
             decoded_delta / seconds, encoded_delta / seconds, sent_delta / seconds);
#elif VIDEO_STREAM_YUV_ONLY_TEST
    ESP_LOGI(TAG,
             "[VIDEO] JPEG decoded=%.1f fps YUV converted=%.1f fps",
             decoded_delta / seconds, yuv_delta / seconds);
#else
    ESP_LOGI(TAG,
             "[VIDEO] encoded=%.1f fps sent=%.1f fps bitrate=%.0f kbps",
             encoded_delta / seconds, sent_delta / seconds,
             bytes_delta * 8.0 / seconds / 1000.0);
#endif
    ESP_LOGI(TAG,
             "[VIDEO] avg ms input_wait=%.2f validate=%.2f JPEG_DEC=%.2f "
             "YUV_CONV=%.2f H264_ENC=%.2f max_H264=%.2f output_wait=%.2f "
             "SEND=%.2f max_SEND=%.2f",
             input_queue_us_delta / sample_count / 1000.0,
             validation_us_delta / sample_count / 1000.0,
             decode_us_delta / sample_count / 1000.0,
             repack_us_delta / sample_count / 1000.0,
             encode_us_delta / sample_count / 1000.0,
             current.h264_max_us / 1000.0,
             upload_received_delta > 0 ? output_queue_us_delta / upload_received_delta / 1000.0 : 0.0,
             send_attempts_delta > 0 ? send_us_delta / send_attempts_delta / 1000.0 : 0.0,
             current.ws_max_us / 1000.0);
    /* rate_limit 必须和几个 drop 计数器并排出现：它是 20 fps 提交门控主动跳过的
     * 帧数，不计入 drop_input/drop_output/invalid/send_fail 中的任何一个。少了
     * 它，"UVC complete − invalid" 和 "encoded" 之间会凭空少掉几帧/秒，看起来
     * 像是丢帧但查不出处。 */
    ESP_LOGI(TAG,
             "[VIDEO] Queue MJPEG=%u/%u YUV=%u/1 H264=%u/%u "
             "drop_input=%" PRIu32 " drop_output=%" PRIu32 " rate_limit=%" PRIu32
             " invalid=%" PRIu32 " send_fail=%" PRIu32,
             (unsigned)input_used, (unsigned)input_capacity,
             s_yuv_busy ? 1U : 0U, (unsigned)output_used, (unsigned)output_capacity,
             current.dropped, current.slot_dropped, current.rate_limited,
             current.input_invalid, current.send_failed);

    /* 三个内存池分开打。之前只打 PSRAM_free，而 PSRAM 一直有 23 MB 空闲——看不出
     * 问题：实机报的是 `esp-aes: Failed to allocate memory for len buffer`，那是
     * mbedtls 硬件 AES-GCM 从 MALLOC_CAP_DMA 里要 **16 字节**描述符都要不到
     * （esp_aes_dma_core.c:355 AES_DMA_ALLOC_CAPS = DMA|8BIT，:841 的
     * aes_dma_calloc(4, sizeof(uint32_t))），池子满了而不是要得太大。
     *
     * 这个池子就是开机日志 "Reserving pool of 146K of internal memory for
     * DMA/internal allocations" 的那 146 KiB（CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=
     * 150000）。同时养着 JPEG 解码器，正好对得上后半段 JPEG_DEC 从 15 ms 涨到 662 ms。
     *
     * free 和 largest 要一起看：free 高而 largest 小是碎片化，两个都低才是真耗尽。
     * INT 列是内部 RAM 总量（DMA 是它的子集），用来区分"内部 RAM 整体紧张"和
     * "只有 DMA 那一档被吃光"。 */
    ESP_LOGI(TAG,
             "[VIDEO] MEM DMA=%u/%u KB INT=%u/%u KB PSRAM=%u/%u KB",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_DMA) / 1024U),
             (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_DMA) / 1024U),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024U),
             (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024U),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024U),
             (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024U));

    /* invalid 的原因拆分，按窗口增量打印且只列非零项。这是唯一能把"整帧丢失"
     * 和"帧内损坏"分开的地方：camera_driver 的 SOI/EOI 计数看不到后者。 */
    char invalid_detail[160];
    size_t invalid_detail_len = 0;
    invalid_detail[0] = '\0';
    for (unsigned i = 1; i < VIDEO_JPEG_ERR_COUNT; ++i) {
        const uint32_t reason_delta = current.invalid_reason[i] - previous.invalid_reason[i];
        if (reason_delta == 0) {
            continue;
        }
        const int written = snprintf(invalid_detail + invalid_detail_len,
                                     sizeof(invalid_detail) - invalid_detail_len,
                                     "%s%s=%" PRIu32,
                                     invalid_detail_len == 0 ? "" : " ",
                                     k_jpeg_invalid_names[i], reason_delta);
        if (written < 0 || (size_t)written >= sizeof(invalid_detail) - invalid_detail_len) {
            break;
        }
        invalid_detail_len += (size_t)written;
    }
    if (invalid_detail_len != 0) {
        ESP_LOGW(TAG, "[VIDEO] invalid %" PRIu32 " 帧归因：%s",
                 current.input_invalid - previous.input_invalid, invalid_detail);
    }

    previous = current;
    last_report_us = now_us;
}

/* 独立统计任务负责定时汇总，避免把“是否有发送完成”当成计时源。
 * 因此即使 WebSocket 暂停或完全断开，也会每 5 秒输出一个诊断窗口。 */
static void video_report_task(void *arg)
{
    (void)arg;
    while (true) {
        video_stream_report(esp_timer_get_time());
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static esp_err_t video_encoder_create(esp_h264_enc_handle_t *encoder)
{
    const esp_h264_enc_cfg_hw_t config = {
        .pic_type = ESP_H264_RAW_FMT_O_UYY_E_VYY,
        .gop = VIDEO_GOP,
        .fps = VIDEO_ENCODE_FPS,
        .res = {
            .width = VIDEO_WIDTH,
            .height = VIDEO_HEIGHT,
        },
        .rc = {
            .bitrate = VIDEO_BITRATE,
            .qp_min = VIDEO_QP_MIN,
            .qp_max = VIDEO_QP_MAX,
        },
    };

    esp_h264_err_t err = esp_h264_enc_hw_new(&config, encoder);
    if (err != ESP_H264_ERR_OK) {
        ESP_LOGE(TAG, "创建 H.264 硬件编码器失败：%d", err);
        return ESP_FAIL;
    }
    err = esp_h264_enc_open(*encoder);
    if (err != ESP_H264_ERR_OK) {
        ESP_LOGE(TAG, "打开 H.264 硬件编码器失败：%d", err);
        esp_h264_enc_del(*encoder);
        *encoder = NULL;
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* 网络发送任务（钉核 0，优先 9）：
 * 只做 H.264 码流上传与失败恢复，不参与任何编解码。
 * 正常连接时按顺序发送；断线时快速丢弃过期输出槽，避免恢复后把旧 P 帧
 * 排队发送。PC 端会等待下一个 IDR 重新同步。 */
static void video_upload_task(void *arg)
{
    (void)arg;

    if (strncmp(s_ws_config.ws_url, "wss://", 6) != 0 ||
        s_ws_config.token[0] == '\0' ||
        s_ws_config.device_id[0] == '\0' ||
        s_ws_config.client_id[0] == '\0') {
        ESP_LOGE(TAG, "缺少 OTA WSS 地址、Token 或设备身份，视频不建立 WebSocket");
        video_streamer_set_enabled(false);
        for (;;) {
            unsigned dropped_index;
            if (xQueueReceive(s_out_ready, &dropped_index,
                              portMAX_DELAY) == pdTRUE) {
                xQueueSend(s_out_free, &dropped_index, portMAX_DELAY);
            }
        }
    }

    const char *ws_uri = s_ws_config.ws_url;

    esp_websocket_client_config_t ws_config = {
        .uri = ws_uri,
        .buffer_size = VIDEO_WS_BUFFER_SIZE,
        .task_stack = VIDEO_WS_TASK_STACK,
        .task_prio = VIDEO_WS_TASK_PRIORITY,
        .task_core_id_set = true,
        .task_core_id = VIDEO_WS_TASK_CORE,
        .disable_auto_reconnect = false,
        .enable_close_reconnect = true,
        .reconnect_timeout_ms = VIDEO_WS_RECONNECT_MS,
        .network_timeout_ms = VIDEO_WS_NETWORK_TIMEOUT_MS,
    };

    ws_config.transport = WEBSOCKET_TRANSPORT_OVER_SSL;
    ws_config.cert_pem = NULL;
    ws_config.crt_bundle_attach = esp_crt_bundle_attach;

    /* 构建自定义请求头：Device-Id、Client-Id、Authorization。 */
    char ws_headers[1024] = {0};
    int header_off = 0;

    if (s_ws_config.device_id[0] != '\0') {
        header_off += snprintf(ws_headers + header_off,
                               sizeof(ws_headers) - header_off,
                               "Device-Id: %s\r\n", s_ws_config.device_id);
    }
    if (s_ws_config.client_id[0] != '\0') {
        header_off += snprintf(ws_headers + header_off,
                               sizeof(ws_headers) - header_off,
                               "Client-Id: %s\r\n", s_ws_config.client_id);
    }
    if (s_ws_config.token[0] != '\0') {
        header_off += snprintf(ws_headers + header_off,
                               sizeof(ws_headers) - header_off,
                               "Authorization: Bearer %s\r\n",
                               s_ws_config.token);
    }
    if (header_off > 0) {
        ws_config.headers = ws_headers;
    }

    esp_websocket_client_handle_t ws_client = esp_websocket_client_init(&ws_config);
    if (ws_client == NULL) {
        ESP_LOGE(TAG, "创建 WebSocket 客户端失败");
        vTaskDelete(NULL);
        return;
    }
    __atomic_store_n(&s_ws_client, ws_client, __ATOMIC_SEQ_CST);
    esp_websocket_register_events(ws_client, WEBSOCKET_EVENT_ANY,
                                  video_ws_event_handler, NULL);
    if (esp_websocket_client_start(ws_client) != ESP_OK) {
        ESP_LOGE(TAG, "启动 WebSocket 客户端失败");
        __atomic_store_n(&s_ws_client, NULL, __ATOMIC_SEQ_CST);
        esp_websocket_client_destroy(ws_client);
        vTaskDelete(NULL);
        return;
    }

    unsigned out_index;
    while (true) {
        QueueSetMemberHandle_t ready = xQueueSelectFromSet(
            s_upload_queue_set, portMAX_DELAY);

        /* 音频包只有几十到几百字节且每 60 ms 必须发送，优先于大块 H.264。
         * 发送仍集中在本任务，避免多个任务并发争用 WebSocket 客户端锁。 */
        video_agent_audio_packet_t audio_packet;
        if (xQueueReceive(s_agent_audio_queue, &audio_packet, 0) == pdTRUE) {
            if (__atomic_load_n(&s_ws_connected, __ATOMIC_SEQ_CST) &&
                esp_websocket_client_is_connected(ws_client)) {
                int sent = esp_websocket_client_send_bin(
                    ws_client, (const char *)audio_packet.data,
                    audio_packet.size,
                    pdMS_TO_TICKS(VIDEO_WS_AUDIO_SEND_TIMEOUT_MS));
                if (sent != audio_packet.size) {
                    video_stream_log_error_limited("Opus 音频发送失败", ESP_FAIL);
                }
            }
            continue;
        }
        if (ready != s_out_ready ||
            xQueueReceive(s_out_ready, &out_index, 0) != pdTRUE) {
            continue;
        }
        video_out_slot_t *slot = &s_out_slots[out_index];

        /* 关闭期间清空已编码但尚未发送的旧帧，恢复后只发送新帧。 */
        if (!video_streamer_is_enabled()) {
            xQueueSend(s_out_free, &out_index, portMAX_DELAY);
            continue;
        }

        /* 槽里已经是「16 字节帧头 + Annex-B 码流」一整条，一次发完。 */
        const int frame_bytes = (int)(VIDEO_WS_FRAME_HEADER_SIZE + slot->data_len);

        esp_err_t send_error = ESP_ERR_INVALID_STATE;
        const int64_t send_started_us = esp_timer_get_time();
        const int64_t output_wait_us = slot->ready_us != 0 ?
                                       send_started_us - slot->ready_us : 0;
        /* 不等待 WiFi/WS 恢复：输出槽里的 P 帧带有旧参考时间轴，断线后继续
         * 发送只会制造延迟，且会把输出槽全部占满。恢复后从新产生的帧继续，
         * PC 端会等待下一个 IDR 重新同步。 */
        if (__atomic_load_n(&s_ws_connected, __ATOMIC_SEQ_CST) &&
            network_manager_is_connected() &&
            esp_websocket_client_is_connected(ws_client)) {
            video_upload_pace();

            /* 发送间隔等待期间连接可能已经断开，重新检查状态，避免在
             * 断线窗口继续申请 ESP-Hosted 传输缓冲。 */
            if (!__atomic_load_n(&s_ws_connected, __ATOMIC_SEQ_CST) ||
                !network_manager_is_connected() ||
                !esp_websocket_client_is_connected(ws_client)) {
                send_error = ESP_ERR_INVALID_STATE;
            } else {
            /* 返回实际写入字节数；不等于整帧长度即视为本帧失败。 */
            const int sent = esp_websocket_client_send_bin(
                ws_client, (const char *)(slot->data + VIDEO_WS_FRAME_HEADER_OFFSET), frame_bytes,
                pdMS_TO_TICKS(VIDEO_WS_SEND_TIMEOUT_MS));
            send_error = (sent == frame_bytes) ? ESP_OK : ESP_FAIL;
            }
        }
        const int64_t send_elapsed_us = esp_timer_get_time() - send_started_us;

        portENTER_CRITICAL(&s_lock);
        s_stats.output_queue_us += output_wait_us;
        s_stats.upload_received++;
        s_stats.send_attempts++;
        s_stats.ws_send_us += send_elapsed_us;
        if (send_elapsed_us > (int64_t)s_stats.ws_max_us) {
            s_stats.ws_max_us = send_elapsed_us;
        }
        if (send_error == ESP_OK) {
            s_stats.sent++;
        } else {
            s_stats.send_failed++;
        }
        portEXIT_CRITICAL(&s_lock);

        if (send_error != ESP_OK) {
            __atomic_store_n(&s_ws_connected, false, __ATOMIC_SEQ_CST);
            /* 重连由 WebSocket 客户端负责，下一帧自然会重新尝试。 */
            video_stream_log_error_limited("H.264 发送失败", send_error);
            /* 旧 P 帧即使稍后发送成功，也只会制造延迟；实时预览从新帧
             * 重新开始，输出槽立即回收，避免断线期间持续积压。 */
            video_drop_pending_output();
        }

        /* 无论成败都归还输出槽，编码任务才可能继续推进。 */
        xQueueSend(s_out_free, &out_index, portMAX_DELAY);
    }
}

/* 编解码任务（钉核 1，优先 8）：
 * JPEG 解码 → YUV 重排 → H.264 编码，完成后把输出槽索引交给上传任务，
 * 自身不等待网络，保证目标帧率下编码侧稳定供帧。 */
static void video_codec_task(void *arg)
{
    (void)arg;
#if !VIDEO_STREAM_JPEG_ONLY_TEST
    uint32_t aligned_input_size = VIDEO_H264_INPUT_SIZE;
    uint8_t *h264_input = esp_h264_aligned_calloc(
        VIDEO_WS_ALIGN, 1, VIDEO_H264_INPUT_SIZE, &aligned_input_size, ESP_H264_MEM_SPIRAM);
    esp_h264_enc_handle_t encoder = NULL;
#else
    uint8_t *h264_input = NULL;
    esp_h264_enc_handle_t encoder = NULL;
#endif
    jpeg_decoder_handle_t jpeg_decoder = NULL;
    uint8_t *jpeg_yuv = NULL;
    uint8_t *out_buffers[VIDEO_OUT_SLOT_COUNT] = { 0 };
    unsigned initialized_out = 0;
    bool upload_started = false;
    bool jpeg_sampling_known = false;
    jpeg_down_sampling_type_t jpeg_sampling = JPEG_DOWN_SAMPLING_YUV422;

#if !VIDEO_STREAM_JPEG_ONLY_TEST
    if (h264_input == NULL ||
        video_encoder_create(&encoder) != ESP_OK) {
        ESP_LOGE(TAG, "H.264 编码资源初始化失败");
        goto codec_fail;
    }
#endif

    /* JPEG 硬件解码引擎：一帧 40ms 超时（30fps 解码 >34ms 即认为超时）。 */
    const jpeg_decode_engine_cfg_t jpeg_engine_cfg = {
        .timeout_ms = 40,
    };
    if (jpeg_new_decoder_engine(&jpeg_engine_cfg, &jpeg_decoder) != ESP_OK) {
        ESP_LOGE(TAG, "创建 JPEG 硬件解码引擎失败");
        goto codec_fail;
    }

    /* YUV422 解码输出缓冲必须用 jpeg_alloc_decoder_mem 申请（PSRAM、cache 对齐）。
     * 解码输出只在编解码任务内使用，单份即可。 */
    const jpeg_decode_memory_alloc_cfg_t yuv_alloc_cfg = {
        .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER,
    };
    size_t jpeg_yuv_size = 0;
    jpeg_yuv = jpeg_alloc_decoder_mem(VIDEO_YUV422_SIZE,
                                      &yuv_alloc_cfg, &jpeg_yuv_size);
    if (jpeg_yuv == NULL || jpeg_yuv_size < VIDEO_YUV422_SIZE) {
        ESP_LOGE(TAG, "分配 JPEG 解码输出缓冲失败");
        goto codec_fail;
    }

#if !VIDEO_STREAM_JPEG_ONLY_TEST
    const esp_err_t dma2d_yuv_init_ret = dma2d_yuv_converter_init();
    const bool dma2d_yuv_ready = dma2d_yuv_init_ret == ESP_OK;
    ESP_LOGI(TAG, "YUV422→YUV420 转换：%s",
             dma2d_yuv_ready ? "DMA2D 硬件 CSC" : "CPU 回退");
    if (!dma2d_yuv_ready) {
        ESP_LOGW(TAG, "DMA2D YUV CSC 当前不可用（%s），禁用硬件尝试，避免每帧超时",
                 esp_err_to_name(dma2d_yuv_init_ret));
    }
#endif

    /* H.264 输出槽池与两条所有权队列：编解码任务独占写入，上传任务独占读出。 */
#if !VIDEO_STREAM_JPEG_ONLY_TEST && !VIDEO_STREAM_YUV_ONLY_TEST
    s_out_free = xQueueCreate(VIDEO_OUT_SLOT_COUNT, sizeof(unsigned));
    s_out_ready = xQueueCreate(VIDEO_OUT_SLOT_COUNT, sizeof(unsigned));
    s_agent_audio_queue_storage = heap_caps_calloc(
        VIDEO_WS_AUDIO_QUEUE_DEPTH,
        sizeof(video_agent_audio_packet_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_agent_audio_queue_storage != NULL) {
        s_agent_audio_queue = xQueueCreateStatic(
            VIDEO_WS_AUDIO_QUEUE_DEPTH,
            sizeof(video_agent_audio_packet_t),
            s_agent_audio_queue_storage,
            &s_agent_audio_queue_control);
    }
    s_upload_queue_set = xQueueCreateSet(
        VIDEO_OUT_SLOT_COUNT + VIDEO_WS_AUDIO_QUEUE_DEPTH);
    if (s_out_free == NULL || s_out_ready == NULL ||
        s_agent_audio_queue == NULL || s_upload_queue_set == NULL ||
        xQueueAddToSet(s_out_ready, s_upload_queue_set) != pdPASS ||
        xQueueAddToSet(s_agent_audio_queue, s_upload_queue_set) != pdPASS) {
        ESP_LOGE(TAG, "创建视频/音频上传队列失败");
        goto codec_fail;
    }
    for (unsigned i = 0; i < VIDEO_OUT_SLOT_COUNT; ++i) {
        uint32_t actual_size = 0;
        /* 按 128 对齐分配：编码缓冲位于槽内 +128，基址对齐才能保证它也对齐。 */
        out_buffers[i] = esp_h264_aligned_calloc(
            VIDEO_WS_ALIGN, 1, VIDEO_H264_SLOT_SIZE, &actual_size, ESP_H264_MEM_SPIRAM);
        if (out_buffers[i] == NULL || actual_size < VIDEO_H264_SLOT_SIZE) {
            ESP_LOGE(TAG, "分配第 %u 个 H.264 输出缓冲失败", i);
            goto codec_fail;
        }
        initialized_out = i + 1;
        s_out_slots[i].data = out_buffers[i];
        s_out_slots[i].data_len = 0;
        xQueueSend(s_out_free, &i, 0);
    }
#endif

#if !VIDEO_STREAM_CODEC_ONLY_TEST && !VIDEO_STREAM_JPEG_ONLY_TEST && !VIDEO_STREAM_YUV_ONLY_TEST
    BaseType_t created = xTaskCreatePinnedToCore(video_upload_task,
                                                 "video_upload",
                                                 VIDEO_TASK_STACK,
                                                 NULL,
                                                 VIDEO_UPLOAD_PRIORITY,
                                                 NULL,
                                                 VIDEO_UPLOAD_CORE);
    if (created != pdPASS) {
        ESP_LOGE(TAG, "创建网络发送任务失败");
        goto codec_fail;
    }
    upload_started = true;
#endif

    if (xTaskCreatePinnedToCore(video_report_task, "video_report", 4096, NULL,
                                VIDEO_REPORT_PRIORITY, NULL, VIDEO_REPORT_CORE) != pdPASS) {
        ESP_LOGW(TAG, "创建视频统计任务失败，保留编解码和传输任务");
    }

#if VIDEO_STREAM_CODEC_ONLY_TEST
    ESP_LOGI(TAG,
             "编码对照模式已就绪：MJPEG→JPEG硬解→YUV→硬编 H.264 "
             "%ux%u@%dfps，%dkbps，输出槽=%d，禁止 WebSocket（编码后立即回收）",
             VIDEO_WIDTH, VIDEO_HEIGHT, VIDEO_ENCODE_FPS,
             VIDEO_BITRATE / 1000, VIDEO_OUT_SLOT_COUNT);
#elif VIDEO_STREAM_JPEG_ONLY_TEST
    ESP_LOGI(TAG,
             "JPEG 解码对照模式已就绪：MJPEG→JPEG硬解输出 YUV422，"
             "%ux%u@%dfps，跳过 YUV/H.264/WebSocket",
             VIDEO_WIDTH, VIDEO_HEIGHT, VIDEO_ENCODE_FPS);
#elif VIDEO_STREAM_YUV_ONLY_TEST
    ESP_LOGI(TAG,
             "YUV 转换对照模式已就绪：MJPEG→JPEG硬解→YUV422→YUV420，"
             "%ux%u@%dfps，跳过 H.264/WebSocket",
             VIDEO_WIDTH, VIDEO_HEIGHT, VIDEO_ENCODE_FPS);
#else
    ESP_LOGI(TAG,
             "编解码/上传双任务已就绪：MJPEG→JPEG硬解→硬编 H.264 "
             "%ux%u@%dfps，%dkbps，GOP=%d，输出槽=%d",
             VIDEO_WIDTH, VIDEO_HEIGHT, VIDEO_ENCODE_FPS,
             VIDEO_BITRATE / 1000, VIDEO_GOP,
             VIDEO_OUT_SLOT_COUNT);
#endif

    unsigned slot_index;
    while (true) {
        if (xQueueReceive(s_input_queue, &slot_index, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        video_input_slot_t *slot = &s_slots[slot_index];
        const int64_t codec_started_us = esp_timer_get_time();
        const int64_t input_queue_us = slot->submitted_us != 0 ?
                                       codec_started_us - slot->submitted_us : 0;
        portENTER_CRITICAL(&s_lock);
        s_stats.input_queue_us += input_queue_us;
        portEXIT_CRITICAL(&s_lock);
        const uint32_t sequence = slot->sequence;
        const uint8_t *jpeg_data = slot->input;
        const size_t frame_data_len = slot->data_len;
        const int64_t validation_started_us = esp_timer_get_time();
        const size_t jpeg_size = video_jpeg_find_eoi(jpeg_data, frame_data_len);
        const video_jpeg_validate_result_t invalid_reason =
            jpeg_size == 0 ? VIDEO_JPEG_ERR_NO_EOI :
                             video_jpeg_structurally_valid(jpeg_data, jpeg_size);
        const bool structurally_valid = invalid_reason == VIDEO_JPEG_OK;
        const int64_t validation_elapsed_us = esp_timer_get_time() - validation_started_us;
        portENTER_CRITICAL(&s_lock);
        s_stats.validation_us += validation_elapsed_us;
        portEXIT_CRITICAL(&s_lock);

        if (!video_streamer_is_enabled()) {
            video_input_slot_release(slot);
            continue;
        }

        /* 结构校验放在这里而不是 USB 回调里，理由见上面
         * video_jpeg_structurally_valid 的说明。不通过就不进硬件解码器：
         * P4 的 JPEG 单元遇到损坏 Marker 会卡在 data-units 数量不符上，
         * 还会连带 DMA2D 报 transaction not in-flight。 */
        if (!structurally_valid) {
            portENTER_CRITICAL(&s_lock);
            s_stats.input_invalid++;
            s_stats.invalid_reason[invalid_reason]++;
            portEXIT_CRITICAL(&s_lock);
            video_input_slot_release(slot);
            continue;
        }

        /* JPEG 硬件解码：环槽里的 MJPEG 帧直接作为位流源，无需二次拷贝。
         * 首帧读取 JPEG 采样方式；相机若原生输出 YUV420，则直接把解码
         * 缓冲交给 H.264，避免 YUV422→YUV420 软件重排。 */
        if (!jpeg_sampling_known) {
            jpeg_decode_picture_info_t picture_info = { 0 };
            if (jpeg_decoder_get_info(jpeg_data,
                                      (uint32_t)jpeg_size,
                                      &picture_info) == ESP_OK) {
                jpeg_sampling = picture_info.sample_method;
                jpeg_sampling_known = true;
#if VIDEO_STREAM_JPEG_ONLY_TEST
                ESP_LOGI(TAG, "JPEG 采样格式：%s（JPEG-only，仅统计解码）",
                         jpeg_sampling == JPEG_DOWN_SAMPLING_YUV420 ? "YUV420" : "YUV422");
#else
                ESP_LOGI(TAG, "JPEG 采样格式：%s",
                         jpeg_sampling == JPEG_DOWN_SAMPLING_YUV420 ? "YUV420 原生直通" :
                         (dma2d_yuv_ready ? "YUV422 DMA2D 硬件转换" : "YUV422 CPU 回退路径"));
#endif
            }
        }
        const bool native_yuv420 = jpeg_sampling_known &&
                                   jpeg_sampling == JPEG_DOWN_SAMPLING_YUV420;
        const uint32_t expected_decode_size = native_yuv420 ?
                                               VIDEO_H264_INPUT_SIZE : VIDEO_YUV422_SIZE;
        const jpeg_decode_cfg_t decode_cfg = {
            .output_format = native_yuv420 ? JPEG_DECODE_OUT_FORMAT_YUV420 :
                                             JPEG_DECODE_OUT_FORMAT_YUV422,
            /* YUV 输出不使用 rgb_order/conv_std，这里保留合法默认值。 */
            .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_RGB,
            .conv_std = JPEG_YUV_RGB_CONV_STD_BT601,
        };
        uint32_t decoded_size = 0;
        const int64_t decode_started_us = esp_timer_get_time();
        esp_err_t decode_error = jpeg_decoder_process(
            jpeg_decoder, &decode_cfg,
            (uint8_t *)jpeg_data, (uint32_t)jpeg_size,
            jpeg_yuv, (uint32_t)jpeg_yuv_size, &decoded_size);
        const int64_t decode_elapsed_us = esp_timer_get_time() - decode_started_us;

        video_input_slot_release(slot);

        /* 如果命令在解码期间到达，不再继续占用编码和发送资源。 */
        if (!video_streamer_is_enabled()) {
            continue;
        }

        if (decode_error != ESP_OK || decoded_size != expected_decode_size) {
            video_stream_log_error_limited("JPEG 解码失败", decode_error);
            continue;
        }

        portENTER_CRITICAL(&s_lock);
        s_stats.jpeg_decoded++;
        s_stats.jpeg_decode_us += decode_elapsed_us;
        portEXIT_CRITICAL(&s_lock);

#if VIDEO_STREAM_JPEG_ONLY_TEST
        /* JPEG-only 对照：解码成功后立即释放输入，完全不进入 YUV 重排
         * 和 H.264 编码阶段。 */
        continue;
#endif

 #if !VIDEO_STREAM_JPEG_ONLY_TEST
#if !VIDEO_STREAM_YUV_ONLY_TEST
        /* 没有空闲输出槽说明上传侧暂时落后：在编码之前丢弃本帧。
         * 丢弃的是“输入帧”而非“已编码 P 帧”，码流时间轴保持连续，
         * PC 端不会因缺帧花屏。 */
        unsigned out_index;
        if (xQueueReceive(s_out_free, &out_index, 0) != pdTRUE) {
            portENTER_CRITICAL(&s_lock);
            s_stats.slot_dropped++;
            portEXIT_CRITICAL(&s_lock);
            continue;
        }
        video_out_slot_t *out_slot = &s_out_slots[out_index];
#endif

        /* 原生 YUV420 与 H.264 输入布局一致时零拷贝；YUV422 优先尝试
         * DMA2D RX CSC。当前 P4 v1.x 不支持该 CSC，因此正常走下面的
         * 分块 CPU 回退路径。 */
        const int64_t repack_started_us = esp_timer_get_time();
        s_yuv_busy = !native_yuv420;
        uint8_t *h264_frame = jpeg_yuv;
        uint32_t h264_frame_len = VIDEO_H264_INPUT_SIZE;
        if (!native_yuv420) {
            esp_err_t dma2d_ret = ESP_ERR_NOT_SUPPORTED;
            if (dma2d_yuv_ready) {
                dma2d_ret = dma2d_yuv422_to_h264_yuv420(
                    jpeg_yuv, VIDEO_YUV422_SIZE,
                    h264_input, VIDEO_H264_INPUT_SIZE,
                    VIDEO_WIDTH, VIDEO_HEIGHT, 100);
            }
            if (dma2d_ret != ESP_OK) {
                if (dma2d_yuv_ready) {
                    ESP_LOGW(TAG, "DMA2D YUV 转换失败（%s），本次回退 CPU",
                             esp_err_to_name(dma2d_ret));
                }
                yuv422_to_h264_yuv420(jpeg_yuv, h264_input);
            }
            h264_frame = h264_input;
        }
        const int64_t repack_elapsed_us = native_yuv420 ? 0 :
                                           esp_timer_get_time() - repack_started_us;
        s_yuv_busy = false;

#if !VIDEO_STREAM_YUV_ONLY_TEST
        portENTER_CRITICAL(&s_lock);
        s_stats.yuv_converted++;
        s_stats.yuv_repack_us += repack_elapsed_us;
        portEXIT_CRITICAL(&s_lock);
#endif

#if VIDEO_STREAM_YUV_ONLY_TEST
        portENTER_CRITICAL(&s_lock);
        s_stats.yuv_converted++;
        s_stats.yuv_repack_us += repack_elapsed_us;
        portEXIT_CRITICAL(&s_lock);
        /* YUV-only 对照：转换完成后立即结束本帧，不进入 H.264。 */
        continue;
#endif

#if !VIDEO_STREAM_YUV_ONLY_TEST
        esp_h264_enc_in_frame_t input_frame = {
            .raw_data = {
                .buffer = h264_frame,
                .len = native_yuv420 ? h264_frame_len : aligned_input_size,
            },
            .pts = sequence * (90000 / VIDEO_ENCODE_FPS),
        };
        /* 码流写进槽内偏移 +128 处（见 VIDEO_WS_ALIGN 处的对齐说明），
         * 帧头放在其前 16 字节的 +112 处，发送时原地补齐。 */
        esp_h264_enc_out_frame_t output_frame = {
            .raw_data = {
                .buffer = out_slot->data + VIDEO_WS_ALIGN,
                .len = VIDEO_H264_OUTPUT_SIZE,
            },
        };

        const int64_t encode_started_us = esp_timer_get_time();
        esp_h264_err_t encode_error =
            esp_h264_enc_process(encoder, &input_frame, &output_frame);
        const int64_t encode_elapsed_us = esp_timer_get_time() - encode_started_us;

        portENTER_CRITICAL(&s_lock);
        s_stats.h264_encode_us += encode_elapsed_us;
        if ((uint64_t)encode_elapsed_us > s_stats.h264_max_us) {
            s_stats.h264_max_us = (uint32_t)encode_elapsed_us;
        }
        portEXIT_CRITICAL(&s_lock);

        if (encode_error == ESP_H264_ERR_OK && output_frame.length > 0) {
            /* 先补齐帧头再入队：上传任务拿到槽时整条消息已经完整。 */
            video_ws_frame_header_fill(out_slot->data + VIDEO_WS_FRAME_HEADER_OFFSET,
                                       sequence, output_frame.frame_type);
            out_slot->data_len = output_frame.length;
            out_slot->sequence = sequence;
            out_slot->pts = input_frame.pts;
            out_slot->frame_type = output_frame.frame_type;
            out_slot->ready_us = esp_timer_get_time();
            portENTER_CRITICAL(&s_lock);
            s_stats.encoded++;
            s_stats.encoded_bytes += output_frame.length;
            portEXIT_CRITICAL(&s_lock);

#if VIDEO_STREAM_CODEC_ONLY_TEST
            /* B 组对照：不把码流交给 WebSocket，立即归还输出槽。
             * 这样编码任务不会受到网络发送阻塞或输出队列积压影响。 */
            xQueueSend(s_out_free, &out_index, 0);
#else
            /* 队列深度与输出槽数一致，槽在被发送任务归还前不会再入队，
             * 此发送不可能失败，只做防御性检查。 */
            if (xQueueSend(s_out_ready, &out_index, 0) != pdTRUE) {
                xQueueSend(s_out_free, &out_index, 0);
                portENTER_CRITICAL(&s_lock);
                s_stats.slot_dropped++;
                portEXIT_CRITICAL(&s_lock);
            }
#endif
        } else {
            video_stream_log_error_limited("H.264 编码失败", encode_error);
            /* 编码失败没有产出码流，槽直接归还，不影响上传侧连续解码。 */
            xQueueSend(s_out_free, &out_index, 0);
        }
#endif
#endif
    }

codec_fail:
    ESP_LOGE(TAG, "编解码任务初始化失败，H.264 实时流不可用");
    if (upload_started) {
        /* 上传任务一旦启动就独占 HTTP client，无法在此回收，保持现状并退出。 */
        vTaskDelete(NULL);
        return;
    }
    if (s_upload_queue_set != NULL) {
        if (s_out_ready != NULL) {
            xQueueRemoveFromSet(s_out_ready, s_upload_queue_set);
        }
        if (s_agent_audio_queue != NULL) {
            xQueueRemoveFromSet(s_agent_audio_queue, s_upload_queue_set);
        }
        vQueueDelete(s_upload_queue_set);
        s_upload_queue_set = NULL;
    }
    if (s_agent_audio_queue != NULL) {
        vQueueDelete(s_agent_audio_queue);
        s_agent_audio_queue = NULL;
    }
    heap_caps_free(s_agent_audio_queue_storage);
    s_agent_audio_queue_storage = NULL;
    if (s_out_ready != NULL) {
        vQueueDelete(s_out_ready);
        s_out_ready = NULL;
    }
    if (s_out_free != NULL) {
        vQueueDelete(s_out_free);
        s_out_free = NULL;
    }
    for (unsigned i = 0; i < initialized_out; ++i) {
        heap_caps_free(out_buffers[i]);
    }
    if (jpeg_yuv != NULL) {
        heap_caps_free(jpeg_yuv);
    }
    if (jpeg_decoder != NULL) {
        jpeg_del_decoder_engine(jpeg_decoder);
    }
    if (encoder != NULL) {
        esp_h264_enc_close(encoder);
        esp_h264_enc_del(encoder);
    }
    if (h264_input != NULL) {
        esp_h264_free(h264_input);
    }
    vTaskDelete(NULL);
}

void video_streamer_set_config(const video_streamer_config_t *config)
{
    if (config != NULL && config->ws_url[0] != '\0') {
        memcpy(&s_ws_config, config, sizeof(s_ws_config));
        ESP_LOGI(TAG, "WebSocket 配置已接收（set_config）：%s", s_ws_config.ws_url);
    }
}

void video_streamer_set_agent_callbacks(
    const video_streamer_agent_callbacks_t *callbacks)
{
    portENTER_CRITICAL(&s_lock);
    if (callbacks != NULL) {
        s_agent_callbacks = *callbacks;
    } else {
        memset(&s_agent_callbacks, 0, sizeof(s_agent_callbacks));
    }
    portEXIT_CRITICAL(&s_lock);
}

esp_err_t video_streamer_agent_send_text(const char *text)
{
    if (text == NULL || text[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    esp_websocket_client_handle_t client =
        __atomic_load_n(&s_ws_client, __ATOMIC_SEQ_CST);
    if (client == NULL ||
        !__atomic_load_n(&s_ws_connected, __ATOMIC_SEQ_CST)) {
        return ESP_ERR_INVALID_STATE;
    }
    const int length = (int)strlen(text);
    int sent = esp_websocket_client_send_text(
        client, text, length, pdMS_TO_TICKS(VIDEO_WS_SEND_TIMEOUT_MS));
    return sent == length ? ESP_OK : ESP_FAIL;
}

esp_err_t video_streamer_agent_send_audio(const uint8_t *data, size_t len)
{
    if (data == NULL || len == 0 || len > VIDEO_WS_AUDIO_PACKET_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_agent_audio_queue == NULL ||
        !__atomic_load_n(&s_ws_connected, __ATOMIC_SEQ_CST)) {
        return ESP_ERR_INVALID_STATE;
    }
    video_agent_audio_packet_t packet = {
        .size = (uint16_t)len,
    };
    memcpy(packet.data, data, len);
    return xQueueSend(s_agent_audio_queue, &packet, 0) == pdTRUE
               ? ESP_OK
               : ESP_ERR_TIMEOUT;
}

esp_err_t video_streamer_init(const video_streamer_config_t *config)
{
    if (s_initialized) {
        return ESP_OK;
    }

    if (config != NULL && config->ws_url[0] != '\0') {
        memcpy(&s_ws_config, config, sizeof(s_ws_config));
        ESP_LOGI(TAG, "WebSocket 配置已接收（init 参数）：%s", s_ws_config.ws_url);
    } else if (s_ws_config.ws_url[0] == '\0') {
        /* 未通过 set_config 或 init 参数传入时保持空配置，上传任务会拒绝建连。 */
        memset(&s_ws_config, 0, sizeof(s_ws_config));
    }

    s_input_queue = xQueueCreate(VIDEO_SLOT_COUNT, sizeof(unsigned));
    if (s_input_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }

    /* 槽缓冲用解码器专用分配接口申请，返回的 PSRAM 缓冲可直接作为
     * JPEG 硬件解码的位流源（输入方向无需额外对齐）。 */
    const jpeg_decode_memory_alloc_cfg_t slot_alloc_cfg = {
        .buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER,
    };
    size_t slot_size = 0;
    for (unsigned i = 0; i < VIDEO_SLOT_COUNT; ++i) {
        s_slots[i].data = jpeg_alloc_decoder_mem(VIDEO_STREAM_JPEG_MAX_SIZE,
                                                  &slot_alloc_cfg, &slot_size);
        if (s_slots[i].data == NULL) {
            ESP_LOGE(TAG, "分配第 %u 个 MJPEG PSRAM 缓冲失败", i);
            while (i > 0) {
                heap_caps_free(s_slots[--i].data);
                s_slots[i].data = NULL;
            }
            vQueueDelete(s_input_queue);
            s_input_queue = NULL;
            return ESP_ERR_NO_MEM;
        }
        s_slots[i].input = s_slots[i].data;
        s_slots[i].release_cb = NULL;
        s_slots[i].release_ctx = NULL;
    }

    BaseType_t created = xTaskCreatePinnedToCore(video_codec_task,
                                                 "video_codec",
                                                 VIDEO_TASK_STACK,
                                                 NULL,
                                                 VIDEO_CODEC_PRIORITY,
                                                 NULL,
                                                 VIDEO_CODEC_CORE);
    if (created != pdPASS) {
        for (unsigned i = 0; i < VIDEO_SLOT_COUNT; ++i) {
            heap_caps_free(s_slots[i].data);
            s_slots[i].data = NULL;
        }
        vQueueDelete(s_input_queue);
        s_input_queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_initialized = true;
    return ESP_OK;
}

static bool video_streamer_submit_jpeg_internal(
    const uint8_t *data,
    size_t data_len,
    video_streamer_input_release_cb_t release_cb,
    void *release_ctx)
{
    if (!s_initialized || !video_streamer_is_enabled() || data == NULL ||
        data_len == 0 || data_len > VIDEO_STREAM_JPEG_MAX_SIZE) {
        return false;
    }

#if !VIDEO_STREAM_CODEC_ONLY_TEST && !VIDEO_STREAM_JPEG_ONLY_TEST && !VIDEO_STREAM_YUV_ONLY_TEST
    if (!__atomic_load_n(&s_ws_connected, __ATOMIC_SEQ_CST) ||
        !network_manager_is_connected()) {
        return false;
    }
#endif

    const int64_t now_us = esp_timer_get_time();
    const int64_t frame_interval_us = 1000000LL / VIDEO_ENCODE_FPS;
    if (s_next_submit_us != 0 &&
        now_us + VIDEO_SUBMIT_EARLY_US < s_next_submit_us) {
        portENTER_CRITICAL(&s_lock);
        s_stats.rate_limited++;
        portEXIT_CRITICAL(&s_lock);
        return false;
    }

    unsigned selected = VIDEO_SLOT_COUNT;
    portENTER_CRITICAL(&s_lock);
    for (unsigned i = 0; i < VIDEO_SLOT_COUNT; ++i) {
        if (!s_slots[i].busy) {
            s_slots[i].busy = true;
            selected = i;
            break;
        }
    }
    if (selected == VIDEO_SLOT_COUNT) {
        s_stats.dropped++;
    }
    portEXIT_CRITICAL(&s_lock);

    if (selected == VIDEO_SLOT_COUNT) {
        return false;
    }

    video_input_slot_t *slot = &s_slots[selected];
    if (release_cb == NULL) {
        memcpy(slot->data, data, data_len);
        slot->input = slot->data;
    } else {
        slot->input = data;
    }
    slot->data_len = data_len;
    slot->sequence = ++s_next_sequence;
    slot->submitted_us = now_us;
    slot->release_cb = release_cb;
    slot->release_ctx = release_ctx;

    if (xQueueSend(s_input_queue, &selected, 0) != pdTRUE) {
        video_input_slot_release(slot);
        portENTER_CRITICAL(&s_lock);
        s_stats.dropped++;
        portEXIT_CRITICAL(&s_lock);
        return false;
    }

    portENTER_CRITICAL(&s_lock);
    s_stats.submitted++;
    portEXIT_CRITICAL(&s_lock);
    /* 以既定时间轴前进，避免把每帧抖动累计到输出帧率；若任务曾长时间
     * 停顿，则从当前时刻重新建立基准，避免恢复后短时间突发提交。 */
    if (s_next_submit_us == 0 ||
        now_us - s_next_submit_us >= frame_interval_us) {
        s_next_submit_us = now_us + frame_interval_us;
    } else {
        s_next_submit_us += frame_interval_us;
    }
    return true;
}

bool video_streamer_submit_jpeg(const uint8_t *data,
                                size_t data_len)
{
    return video_streamer_submit_jpeg_internal(data, data_len, NULL, NULL);
}

bool video_streamer_submit_jpeg_owned(
    const uint8_t *data,
    size_t data_len,
    video_streamer_input_release_cb_t release_cb,
    void *release_ctx)
{
    if (release_cb == NULL) {
        return false;
    }
    return video_streamer_submit_jpeg_internal(data, data_len, release_cb, release_ctx);
}
