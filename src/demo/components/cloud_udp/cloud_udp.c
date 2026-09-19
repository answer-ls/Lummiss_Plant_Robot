#include "cloud_udp.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "mbedtls/aes.h"

static const char *TAG = "CLOUD_UDP";

#define CLOUD_UDP_HEADER_LEN      16
#define CLOUD_UDP_TYPE_AUDIO      1
/* 单包上限：16 字节头 + Opus。实测最大码流 227 字节，512 有充足余量。
 * ponytail: 真遇到更大的 Opus 帧（换更高码率）时再上调，现在不需要。 */
#define CLOUD_UDP_PACKET_MAX      512
#define CLOUD_UDP_RX_TIMEOUT_MS   1000
/* 任务栈 8192（PSRAM）：本地缓冲 ~1 KB（buffer+decrypted）+ mbedtls AES 上下文
 * + 下行回调链（incoming_audio_callback → 播放队列入队）。2026-09-18 实机验证：
 * 4096 在收到第一个下行音频包时栈保护越界 abort —— 别再改小。 */
#define CLOUD_UDP_TASK_STACK      8192
#define CLOUD_UDP_TASK_PRIO       5
#define CLOUD_UDP_TASK_CORE       0

/* 周期性统计间隔。作用：把"下行静默"变成可观测 —— 上行在发而 rx 恒为 0，
 * 就能把问题定位在「网关没回包 / NAT 丢包」，而不是凭空猜。 */
#define CLOUD_UDP_STATS_PERIOD_MS 10000

static int s_sock = -1;
static struct sockaddr_in s_peer;
static bool s_ready;

static uint8_t s_key[16];

/* 包头模板 = Server Hello 下发的 16 字节 nonce。
 * 上游实现（78/xiaozhi-esp32 mqtt_protocol.cc SendAudio）以此为模板，
 * 只覆写 2-3（payload_len）/ 8-11（timestamp）/ 12-15（sequence）三个字段；
 * nonce 的 0-1 即 type/flags、4-7 即 ssrc(connection_id) ——
 * 「connection_id 由网关分配」实际上就是它藏在 nonce 里。 */
static uint8_t s_nonce_template[16];
static bool s_has_nonce_template;

static uint32_t s_sequence;
static uint32_t s_connection_id;

static cloud_udp_audio_cb_t s_audio_cb;
static void *s_audio_cb_ctx;

static TaskHandle_t s_rx_task;
static bool s_running;

/* 统计：只用于报告，方便判断"到底有没有真的收发" */
static uint32_t s_tx_packets;
static uint32_t s_tx_errors;
static uint32_t s_rx_packets;
static uint32_t s_rx_errors;

static void write_be16(uint8_t *dst, uint16_t value)
{
    dst[0] = (uint8_t)(value >> 8);
    dst[1] = (uint8_t)(value & 0xFFu);
}

static void write_be32(uint8_t *dst, uint32_t value)
{
    dst[0] = (uint8_t)(value >> 24);
    dst[1] = (uint8_t)((value >> 16) & 0xFFu);
    dst[2] = (uint8_t)((value >> 8) & 0xFFu);
    dst[3] = (uint8_t)(value & 0xFFu);
}

static uint16_t read_be16(const uint8_t *src)
{
    return (uint16_t)(((uint16_t)src[0] << 8) | (uint16_t)src[1]);
}

static uint32_t read_be32(const uint8_t *src)
{
    return ((uint32_t)src[0] << 24) | ((uint32_t)src[1] << 16) |
           ((uint32_t)src[2] << 8) | (uint32_t)src[3];
}

/* AES-128-CTR，IV 就是本包 16 字节头（文档 §4.2）。加密与解密是同一个操作。 */
static void aes_ctr_xor(const uint8_t header[CLOUD_UDP_HEADER_LEN],
                        const uint8_t *in, uint8_t *out, size_t len)
{
    mbedtls_aes_context aes;
    uint8_t nonce_counter[16];
    uint8_t stream_block[16];
    size_t nc_off = 0;

    memcpy(nonce_counter, header, CLOUD_UDP_HEADER_LEN);
    mbedtls_aes_init(&aes);
    if (mbedtls_aes_setkey_enc(&aes, s_key, 128) == 0) {
        (void)mbedtls_aes_crypt_ctr(&aes, len, &nc_off, nonce_counter,
                                    stream_block, in, out);
    }
    mbedtls_aes_free(&aes);
}

static void build_header(uint8_t *header, uint16_t payload_len,
                         uint32_t timestamp_ms)
{
    if (s_has_nonce_template) {
        /* 上游实现（mqtt_protocol.cc SendAudio）：以服务端 nonce 为模板，
         * 只覆写 len/timestamp/sequence 三个字段；nonce 的 0-1 即 type/flags、
         * 4-7 即 ssrc(connection_id)，保持服务端原值 —— 设备不自行编造。 */
        memcpy(header, s_nonce_template, CLOUD_UDP_HEADER_LEN);
    } else {
        /* 无 nonce 时的退化模板（当前这版服务端实测也接受上行，但缺 ssrc） */
        header[0] = CLOUD_UDP_TYPE_AUDIO;
        header[1] = 0;
        write_be32(header + 4, s_connection_id);
    }
    write_be16(header + 2, payload_len);
    write_be32(header + 8, timestamp_ms);
    /* 首包序列号必须为 1（上游 78/xiaozhi-esp32 用 ++local_sequence_）。
     * 服务端接收侧 remote_sequence_ 从 0 起算，并**拒绝 sequence <= 期望值**
     * 的包 —— 首包若是 0 会被当成重放直接丢掉。 */
    write_be32(header + 12, ++s_sequence);
}

esp_err_t cloud_udp_send_opus(const uint8_t *opus, size_t len)
{
    if (!s_ready || s_sock < 0) {
        return ESP_ERR_INVALID_STATE;
    }
    if (opus == NULL || len == 0 || len > CLOUD_UDP_PACKET_MAX) {
        s_tx_errors++;
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t packet[CLOUD_UDP_HEADER_LEN + CLOUD_UDP_PACKET_MAX];
    /* timestamp 用开机以来的毫秒数：单调、不受墙钟跳变影响，
     * 正是音频时间戳需要的性质。 */
    const uint32_t timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000);
    build_header(packet, (uint16_t)len, timestamp_ms);
    aes_ctr_xor(packet, opus, packet + CLOUD_UDP_HEADER_LEN, len);

    const int sent = sendto(s_sock, packet, (int)(CLOUD_UDP_HEADER_LEN + len), 0,
                            (const struct sockaddr *)&s_peer, sizeof(s_peer));
    if (sent < 0) {
        s_tx_errors++;
        return ESP_FAIL;
    }
    s_tx_packets++;
    return ESP_OK;
}

static void cloud_udp_rx_task(void *arg)
{
    (void)arg;
    uint8_t buffer[CLOUD_UDP_HEADER_LEN + CLOUD_UDP_PACKET_MAX];
    uint8_t decrypted[CLOUD_UDP_PACKET_MAX];
    uint32_t timeout_rounds = 0;
    bool first_packet_logged = false;

    while (s_running) {
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);
        const int got = recvfrom(s_sock, buffer, sizeof(buffer), 0,
                                 (struct sockaddr *)&from, &from_len);
        if (got < 0) {
            /* SO_RCVTIMEO 到点。周期性报收发统计：上行在发而 rx 恒为 0 时，
             * 能立刻把问题定位在「网关没回包 / NAT 丢弃」。 */
            if (++timeout_rounds >=
                (CLOUD_UDP_STATS_PERIOD_MS / CLOUD_UDP_RX_TIMEOUT_MS)) {
                timeout_rounds = 0;
                ESP_LOGI(TAG, "UDP 统计：tx=%u(错%u) rx=%u(错%u)",
                         (unsigned)s_tx_packets, (unsigned)s_tx_errors,
                         (unsigned)s_rx_packets, (unsigned)s_rx_errors);
            }
            continue;
        }
        timeout_rounds = 0;
        if (!first_packet_logged) {
            first_packet_logged = true;
            /* 第一个包的来源很关键：如果 TTS 音频来自别的源端口，
             * 端口受限型 NAT 会把它丢掉 —— 这是"上行通、下行静"的
             * 一个常见原因。 */
            ESP_LOGI(TAG, "收到第一个 UDP 包：来源 %s:%u",
                     inet_ntoa(from.sin_addr),
                     (unsigned)ntohs(from.sin_port));
        }
        if (got <= CLOUD_UDP_HEADER_LEN) {
            s_rx_errors++;
            continue;
        }
        if (buffer[0] != CLOUD_UDP_TYPE_AUDIO) {
            ESP_LOGW(TAG, "收到非音频类型 UDP 包（type=%u），已忽略", buffer[0]);
            s_rx_errors++;
            continue;
        }

        const uint16_t payload_len = read_be16(buffer + 2);
        if (payload_len == 0 || payload_len != (uint16_t)(got - CLOUD_UDP_HEADER_LEN) ||
            payload_len > CLOUD_UDP_PACKET_MAX) {
            ESP_LOGW(TAG, "UDP 包长度字段(%u)与实际(%d)不符，已丢弃",
                     payload_len, got - CLOUD_UDP_HEADER_LEN);
            s_rx_errors++;
            continue;
        }

        aes_ctr_xor(buffer, buffer + CLOUD_UDP_HEADER_LEN, decrypted, payload_len);
        s_rx_packets++;
        if (s_audio_cb != NULL) {
            s_audio_cb(decrypted, payload_len, s_audio_cb_ctx);
        }
    }
    /* 本任务用 WithCaps 创建（栈在 PSRAM），必须配对用 WithCaps 删除，
     * 否则每次 stop/start 都会泄漏一块 PSRAM 栈。 */
    vTaskDeleteWithCaps(NULL);
}

void cloud_udp_set_audio_callback(cloud_udp_audio_cb_t cb, void *ctx)
{
    s_audio_cb = cb;
    s_audio_cb_ctx = ctx;
}

bool cloud_udp_is_ready(void) { return s_ready; }

esp_err_t cloud_udp_start(const cloud_mqtt_session_t *session)
{
    if (s_ready) {
        return ESP_OK;   /* 幂等 */
    }
    if (session == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (session->udp_server[0] == '\0' || session->udp_port == 0) {
        ESP_LOGE(TAG, "Server Hello 未给出 udp.server / udp.port，无法建立 UDP");
        return ESP_ERR_INVALID_ARG;
    }
    /* 加密是硬要求：不认识就不发，绝不退回明文。 */
    if (strcmp(session->udp_encryption, "aes-128-ctr") != 0) {
        ESP_LOGE(TAG, "不支持的 UDP 加密方式：%s（只支持 aes-128-ctr）",
                 session->udp_encryption[0] ? session->udp_encryption : "(空)");
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (!session->has_key) {
        ESP_LOGE(TAG, "Server Hello 的 UDP key 缺失或非法（需要 32 个十六进制字符）");
        return ESP_ERR_INVALID_ARG;
    }

    memcpy(s_key, session->key, sizeof(s_key));

    /* 包头模板：上游实现直接以服务端下发的 nonce 为包头基础
     * （nonce 的 4-7 字节 = ssrc/connection_id，即"网关分配"的那个值）。
     * 设备不自行编造 —— 之前手搓 connection_id=0，疑似导致网关无法
     * 把下行音频路由回设备。 */
    if (session->has_nonce) {
        memcpy(s_nonce_template, session->nonce, sizeof(s_nonce_template));
        s_has_nonce_template = true;
        ESP_LOGI(TAG,
                 "包头模板 = 服务端 nonce：nonce[0]=0x%02x ssrc=0x%08x",
                 s_nonce_template[0],
                 (unsigned)read_be32(s_nonce_template + 4));
        if (s_nonce_template[0] != CLOUD_UDP_TYPE_AUDIO) {
            ESP_LOGW(TAG,
                     "nonce 首字节不是 0x01 —— 上游实现会原样作为包头 type 字节发出，"
                     "若服务端按 0x01 校验将丢弃上行");
        }
    } else {
        s_has_nonce_template = false;
        ESP_LOGW(TAG,
                 "Server Hello 未携带 nonce：包头缺 ssrc，"
                 "网关可能无法把下行音频路由回设备");
    }

    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_sock < 0) {
        ESP_LOGE(TAG, "创建 UDP socket 失败");
        return ESP_FAIL;
    }

    /* 接收超时：让接收任务能周期性检查退出标志。 */
    struct timeval tv = {
        .tv_sec = CLOUD_UDP_RX_TIMEOUT_MS / 1000,
        .tv_usec = (CLOUD_UDP_RX_TIMEOUT_MS % 1000) * 1000,
    };
    (void)setsockopt(s_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    memset(&s_peer, 0, sizeof(s_peer));
    s_peer.sin_family = AF_INET;
    s_peer.sin_port = htons(session->udp_port);
    if (inet_pton(AF_INET, session->udp_server, &s_peer.sin_addr) != 1) {
        ESP_LOGE(TAG, "udp.server 不是合法 IPv4 地址：%s", session->udp_server);
        close(s_sock);
        s_sock = -1;
        return ESP_ERR_INVALID_ARG;
    }

    s_sequence = 0;
    /* connection_id "由网关分配"（文档 §4.2），但 Server Hello 里并没有这个字段。
     * 先固定为 0，并靠"设备先发包、网关记录来源地址"（文档 §4.1 末段）建立会话；
     * 等后端明确下发方式后再改。这是当前协议的已知空白，不是实现偷懒。 */
    s_connection_id = 0;

    s_running = true;
    s_ready = true;
    /* 任务栈放 PSRAM：与 cloud_mcp / xiaozhi service_task 同一套做法，
     * 给内部 RAM 的 LVGL 显示缓冲让路。 */
    if (xTaskCreatePinnedToCoreWithCaps(cloud_udp_rx_task, "cloud_udp_rx",
                                        CLOUD_UDP_TASK_STACK, NULL,
                                        CLOUD_UDP_TASK_PRIO, &s_rx_task,
                                        CLOUD_UDP_TASK_CORE,
                                        MALLOC_CAP_SPIRAM |
                                            MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGE(TAG, "创建 UDP 接收任务失败");
        s_ready = false;
        s_running = false;
        close(s_sock);
        s_sock = -1;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG,
             "UDP 通道就绪：peer=%s:%u 加密=%s key 已装载(16 字节) "
             "sequence/timestamp 已启用，connection_id=%u（协议未定义下发方式，暂用 0）",
             session->udp_server, (unsigned)session->udp_port,
             session->udp_encryption, (unsigned)s_connection_id);
    return ESP_OK;
}

void cloud_udp_stop(void)
{
    /* 抢占式取走 fd：并发的第二次调用会看到 s_sock 已是 -1，直接返回，
     * 避免对同一个 fd 双重 close（fd 复用会让新连接被误关）。
     * 本函数可能被三方并发触发：capture 任务（空闲看门狗）、
     * wake 任务（唤醒失败）、MQTT 任务（服务端 goodbye）。 */
    if (s_sock < 0 && !s_ready) {
        return;
    }
    const int sock = s_sock;
    s_sock = -1;
    s_running = false;
    s_ready = false;
    if (sock >= 0) {
        shutdown(sock, SHUT_RDWR);
        close(sock);
    }
    s_rx_task = NULL;
    memset(s_key, 0, sizeof(s_key));
    ESP_LOGW(TAG, "UDP 通道已关闭（tx=%u/%u rx=%u/%u）",
             (unsigned)s_tx_packets, (unsigned)s_tx_errors,
             (unsigned)s_rx_packets, (unsigned)s_rx_errors);
}
