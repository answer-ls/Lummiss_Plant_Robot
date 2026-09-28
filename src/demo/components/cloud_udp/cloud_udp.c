#include "cloud_udp.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "mbedtls/aes.h"
#include "mbedtls/gcm.h"

static const char *TAG = "CLOUD_UDP";

#define CLOUD_UDP_HEADER_LEN      16
#define CLOUD_UDP_GCM_IV_LEN      12
#define CLOUD_UDP_GCM_TAG_LEN     16
#define CLOUD_UDP_TYPE_AUDIO      1
/* Opus 明文/密文上限。GCM 报文还会在尾部附加 16 字节认证标签。
 * 实测最大码流 227 字节，512 有充足余量。
 * ponytail: 真遇到更大的 Opus 帧（换更高码率）时再上调，现在不需要。 */
#define CLOUD_UDP_PACKET_MAX      512
#define CLOUD_UDP_WIRE_MAX        (CLOUD_UDP_HEADER_LEN + CLOUD_UDP_PACKET_MAX + \
                                   CLOUD_UDP_GCM_TAG_LEN)
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

typedef enum {
    CLOUD_UDP_CRYPTO_NONE = 0,
    CLOUD_UDP_CRYPTO_CTR,
    CLOUD_UDP_CRYPTO_GCM,
} cloud_udp_crypto_mode_t;

static cloud_udp_crypto_mode_t s_crypto_mode;

/* GCM 上、下行分别使用独立上下文，避免两个任务同时修改同一个 GCM 状态。
 * 上下文只在会话建立时设置一次密钥，并明确放到 PSRAM；每个 Opus 包不做
 * malloc/free，也不占用紧张的 INTERNAL/DMA 堆。 */
static mbedtls_gcm_context *s_gcm_tx;
static mbedtls_gcm_context *s_gcm_rx;
static bool s_gcm_initialized;

/* 静态互斥锁不占用动态堆；用于防止 stop 清理上下文时仍有收发任务正在加解密。 */
static StaticSemaphore_t s_crypto_mutex_storage;
static SemaphoreHandle_t s_crypto_mutex;

static uint32_t s_sequence;
static uint32_t s_connection_id;
static uint32_t s_rx_sequence;
static bool s_rx_sequence_valid;

static cloud_udp_audio_cb_t s_audio_cb;
static void *s_audio_cb_ctx;

static TaskHandle_t s_rx_task;
static bool s_running;
/* start/stop 可能来自唤醒任务和 MQTT 回调任务；串行化整个会话生命周期，
 * 防止一次 stop 尚未清理完毕时，另一任务已经安装新 socket/密钥。 */
static bool s_lifecycle_busy;

static void cloud_udp_lifecycle_lock(void)
{
    bool expected = false;
    while (!__atomic_compare_exchange_n(&s_lifecycle_busy, &expected, true,
                                        false, __ATOMIC_ACQUIRE,
                                        __ATOMIC_RELAXED)) {
        expected = false;
        vTaskDelay(1);
    }
}

static void cloud_udp_lifecycle_unlock(void)
{
    __atomic_store_n(&s_lifecycle_busy, false, __ATOMIC_RELEASE);
}

/* 统计：只用于报告，方便判断"到底有没有真的收发" */
static uint32_t s_tx_packets;
static uint32_t s_tx_errors;
static uint32_t s_rx_packets;
static uint32_t s_rx_errors;
static uint32_t s_rx_auth_failures;
static uint32_t s_rx_replay_drops;

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

/* udp.server 由服务端下发，既可能是数字 IPv4，也可能是域名。
 * DNS 查询运行在音频会话任务中，不会阻塞 MQTT 回调。当前 UDP 网关使用 IPv4。 */
static esp_err_t resolve_udp_peer(const char *server, uint16_t port,
                                  struct sockaddr_in *peer)
{
    if (server == NULL || server[0] == '\0' || peer == NULL || port == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(peer, 0, sizeof(*peer));
    peer->sin_family = AF_INET;
    peer->sin_port = htons(port);
    if (inet_pton(AF_INET, server, &peer->sin_addr) == 1) {
        return ESP_OK;
    }

    const struct addrinfo hints = {
        .ai_family = AF_INET,
        .ai_socktype = SOCK_DGRAM,
        .ai_protocol = IPPROTO_UDP,
    };
    struct addrinfo *result = NULL;
    const int gai_ret = getaddrinfo(server, NULL, &hints, &result);
    if (gai_ret != 0 || result == NULL || result->ai_addr == NULL ||
        result->ai_addrlen < sizeof(struct sockaddr_in)) {
        ESP_LOGE(TAG, "解析 udp.server 失败：host=%s getaddrinfo=%d",
                 server, gai_ret);
        if (result != NULL) {
            freeaddrinfo(result);
        }
        return ESP_ERR_NOT_FOUND;
    }

    const struct sockaddr_in *resolved =
        (const struct sockaddr_in *)result->ai_addr;
    peer->sin_addr = resolved->sin_addr;
    freeaddrinfo(result);

    char address[INET_ADDRSTRLEN] = {0};
    if (inet_ntop(AF_INET, &peer->sin_addr, address, sizeof(address)) != NULL) {
        ESP_LOGI(TAG, "udp.server DNS 已解析：%s -> %s", server, address);
    }
    return ESP_OK;
}

/* AES-128-CTR 迁移兼容路径：IV 是完整 16 字节包头，加密与解密相同。 */
static int aes_ctr_xor(const uint8_t header[CLOUD_UDP_HEADER_LEN],
                       const uint8_t *in, uint8_t *out, size_t len)
{
    mbedtls_aes_context aes;
    uint8_t nonce_counter[16];
    uint8_t stream_block[16];
    size_t nc_off = 0;
    int ret;

    memcpy(nonce_counter, header, CLOUD_UDP_HEADER_LEN);
    mbedtls_aes_init(&aes);
    ret = mbedtls_aes_setkey_enc(&aes, s_key, 128);
    if (ret == 0) {
        ret = mbedtls_aes_crypt_ctr(&aes, len, &nc_off, nonce_counter,
                                    stream_block, in, out);
    }
    mbedtls_aes_free(&aes);
    return ret;
}

/* 正式协议 GCM 上行：
 * IV  = header[4..15]（connection_id + timestamp + sequence，共 12 字节）
 * AAD = 完整 16 字节 header
 * Tag = 16 字节。 */
static int aes_gcm_encrypt(const uint8_t header[CLOUD_UDP_HEADER_LEN],
                           const uint8_t *plaintext, uint8_t *ciphertext,
                           size_t len, uint8_t tag[CLOUD_UDP_GCM_TAG_LEN])
{
    return mbedtls_gcm_crypt_and_tag(s_gcm_tx, MBEDTLS_GCM_ENCRYPT, len,
                                     header + 4, CLOUD_UDP_GCM_IV_LEN,
                                     header, CLOUD_UDP_HEADER_LEN,
                                     plaintext, ciphertext,
                                     CLOUD_UDP_GCM_TAG_LEN, tag);
}

/* 正式协议 GCM 下行：只有 Tag 验证成功（返回 0）才允许把明文交给 Opus。 */
static int aes_gcm_auth_decrypt(const uint8_t header[CLOUD_UDP_HEADER_LEN],
                                const uint8_t *ciphertext, uint8_t *plaintext,
                                size_t len,
                                const uint8_t tag[CLOUD_UDP_GCM_TAG_LEN])
{
    return mbedtls_gcm_auth_decrypt(s_gcm_rx, len,
                                    header + 4, CLOUD_UDP_GCM_IV_LEN,
                                    header, CLOUD_UDP_HEADER_LEN,
                                    tag, CLOUD_UDP_GCM_TAG_LEN,
                                    ciphertext, plaintext);
}

static void gcm_contexts_free(void)
{
    if (!s_gcm_initialized) {
        return;
    }
    if (s_gcm_tx != NULL) {
        mbedtls_gcm_free(s_gcm_tx);
        heap_caps_free(s_gcm_tx);
        s_gcm_tx = NULL;
    }
    if (s_gcm_rx != NULL) {
        mbedtls_gcm_free(s_gcm_rx);
        heap_caps_free(s_gcm_rx);
        s_gcm_rx = NULL;
    }
    s_gcm_initialized = false;
}

static esp_err_t gcm_contexts_init(void)
{
    /* 上一会话若在接收任务自停止路径中没有及时释放，这里先统一清理。 */
    gcm_contexts_free();
    s_gcm_tx = heap_caps_calloc(1, sizeof(*s_gcm_tx),
                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_gcm_rx = heap_caps_calloc(1, sizeof(*s_gcm_rx),
                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_gcm_tx == NULL || s_gcm_rx == NULL) {
        ESP_LOGE(TAG, "PSRAM 分配 GCM 上下文失败");
        if (s_gcm_tx != NULL) {
            heap_caps_free(s_gcm_tx);
            s_gcm_tx = NULL;
        }
        if (s_gcm_rx != NULL) {
            heap_caps_free(s_gcm_rx);
            s_gcm_rx = NULL;
        }
        return ESP_ERR_NO_MEM;
    }
    mbedtls_gcm_init(s_gcm_tx);
    mbedtls_gcm_init(s_gcm_rx);
    s_gcm_initialized = true;

    int ret = mbedtls_gcm_setkey(s_gcm_tx, MBEDTLS_CIPHER_ID_AES,
                                 s_key, 128);
    if (ret == 0) {
        ret = mbedtls_gcm_setkey(s_gcm_rx, MBEDTLS_CIPHER_ID_AES,
                                 s_key, 128);
    }
    if (ret != 0) {
        ESP_LOGE(TAG, "AES-128-GCM 设置会话密钥失败：-0x%04x", (unsigned)-ret);
        gcm_contexts_free();
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* 32 位 sequence 回绕时仍按模 2^32 判断新旧；相同或旧包一律丢弃。 */
static bool sequence_is_newer(uint32_t sequence, uint32_t previous)
{
    return (int32_t)(sequence - previous) > 0;
}

static void build_header(uint8_t *header, uint16_t payload_len,
                         uint32_t timestamp_ms)
{
    memset(header, 0, CLOUD_UDP_HEADER_LEN);
    header[0] = CLOUD_UDP_TYPE_AUDIO;
    /* 正式 GCM 的 flags 固定为 1；迁移期 CTR 固定为 0。 */
    header[1] = (s_crypto_mode == CLOUD_UDP_CRYPTO_GCM) ? 1U : 0U;
    write_be16(header + 2, payload_len);
    write_be32(header + 4, s_connection_id);
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

    uint8_t packet[CLOUD_UDP_WIRE_MAX];
    int send_ret = -1;
    if (xSemaphoreTake(s_crypto_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        s_tx_errors++;
        return ESP_ERR_TIMEOUT;
    }
    /* stop 可能在等待互斥锁期间关闭会话，拿到锁后必须再次检查。 */
    if (!s_ready || s_sock < 0) {
        xSemaphoreGive(s_crypto_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    /* sequence 的分配、加密和 sendto 必须在同一临界区按顺序完成。
     * 如果只保护加密，多个发送者仍可能让较大的 sequence 先到达网关。 */
    const uint32_t timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000);
    build_header(packet, (uint16_t)len, timestamp_ms);
    size_t wire_len = CLOUD_UDP_HEADER_LEN + len;
    int crypto_ret = -1;
    if (s_crypto_mode == CLOUD_UDP_CRYPTO_GCM) {
        crypto_ret = aes_gcm_encrypt(
            packet, opus, packet + CLOUD_UDP_HEADER_LEN, len,
            packet + CLOUD_UDP_HEADER_LEN + len);
        wire_len += CLOUD_UDP_GCM_TAG_LEN;
    } else if (s_crypto_mode == CLOUD_UDP_CRYPTO_CTR) {
        crypto_ret = aes_ctr_xor(packet, opus,
                                 packet + CLOUD_UDP_HEADER_LEN, len);
    }
    if (crypto_ret == 0) {
        send_ret = sendto(s_sock, packet, (int)wire_len, 0,
                          (const struct sockaddr *)&s_peer, sizeof(s_peer));
    }
    xSemaphoreGive(s_crypto_mutex);

    if (crypto_ret != 0) {
        ESP_LOGE(TAG, "UDP 上行加密失败：mode=%s ret=-0x%04x",
                 s_crypto_mode == CLOUD_UDP_CRYPTO_GCM ? "GCM" : "CTR",
                 (unsigned)-crypto_ret);
        s_tx_errors++;
        return ESP_FAIL;
    }
    if (send_ret < 0 || (size_t)send_ret != wire_len) {
        s_tx_errors++;
        return ESP_FAIL;
    }
    s_tx_packets++;
    return ESP_OK;
}

static void cloud_udp_rx_task(void *arg)
{
    (void)arg;
    const int task_sock = s_sock;
    uint8_t buffer[CLOUD_UDP_WIRE_MAX];
    uint8_t decrypted[CLOUD_UDP_PACKET_MAX];
    uint32_t timeout_rounds = 0;
    bool first_packet_logged = false;

    while (__atomic_load_n(&s_running, __ATOMIC_ACQUIRE)) {
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);
        const int got = recvfrom(task_sock, buffer, sizeof(buffer), 0,
                                 (struct sockaddr *)&from, &from_len);
        /* stop 已撤销会话时，不再解密刚从旧 socket 返回的尾包。 */
        if (!__atomic_load_n(&s_running, __ATOMIC_ACQUIRE)) {
            break;
        }
        if (got < 0) {
            /* SO_RCVTIMEO 到点。周期性报收发统计：上行在发而 rx 恒为 0 时，
             * 能立刻把问题定位在「网关没回包 / NAT 丢弃」。 */
            if (++timeout_rounds >=
                (CLOUD_UDP_STATS_PERIOD_MS / CLOUD_UDP_RX_TIMEOUT_MS)) {
                timeout_rounds = 0;
                ESP_LOGI(TAG,
                         "UDP 统计：tx=%u(错%u) rx=%u(错%u) auth_fail=%u replay=%u",
                         (unsigned)s_tx_packets, (unsigned)s_tx_errors,
                         (unsigned)s_rx_packets, (unsigned)s_rx_errors,
                         (unsigned)s_rx_auth_failures,
                         (unsigned)s_rx_replay_drops);
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
        const size_t trailer_len =
            (s_crypto_mode == CLOUD_UDP_CRYPTO_GCM) ? CLOUD_UDP_GCM_TAG_LEN : 0U;
        if (got <= (int)(CLOUD_UDP_HEADER_LEN + trailer_len)) {
            s_rx_errors++;
            continue;
        }
        if (buffer[0] != CLOUD_UDP_TYPE_AUDIO) {
            ESP_LOGW(TAG, "收到非音频类型 UDP 包（type=%u），已忽略", buffer[0]);
            s_rx_errors++;
            continue;
        }
        const uint8_t expected_flags =
            (s_crypto_mode == CLOUD_UDP_CRYPTO_GCM) ? 1U : 0U;
        if (buffer[1] != expected_flags) {
            ESP_LOGW(TAG, "UDP flags=%u 与协商加密模式不符，已丢弃", buffer[1]);
            s_rx_errors++;
            continue;
        }

        const uint16_t payload_len = read_be16(buffer + 2);
        const size_t expected_len = CLOUD_UDP_HEADER_LEN +
                                    (size_t)payload_len + trailer_len;
        if (payload_len == 0 || expected_len != (size_t)got ||
            payload_len > CLOUD_UDP_PACKET_MAX) {
            ESP_LOGW(TAG, "UDP 包长度字段(%u)与实际密文长度(%d)不符，已丢弃",
                     payload_len,
                     got - CLOUD_UDP_HEADER_LEN - (int)trailer_len);
            s_rx_errors++;
            continue;
        }

        int crypto_ret = -1;
        if (xSemaphoreTake(s_crypto_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
            s_rx_errors++;
            continue;
        }
        if (!__atomic_load_n(&s_running, __ATOMIC_ACQUIRE)) {
            xSemaphoreGive(s_crypto_mutex);
            break;
        }
        if (s_crypto_mode == CLOUD_UDP_CRYPTO_GCM) {
            crypto_ret = aes_gcm_auth_decrypt(
                buffer, buffer + CLOUD_UDP_HEADER_LEN, decrypted, payload_len,
                buffer + CLOUD_UDP_HEADER_LEN + payload_len);
        } else if (s_crypto_mode == CLOUD_UDP_CRYPTO_CTR) {
            crypto_ret = aes_ctr_xor(buffer,
                                     buffer + CLOUD_UDP_HEADER_LEN,
                                     decrypted, payload_len);
        }
        xSemaphoreGive(s_crypto_mutex);

        if (crypto_ret != 0) {
            /* GCM Tag 验证失败后，绝不能把未认证明文送入 Opus。 */
            if (s_crypto_mode == CLOUD_UDP_CRYPTO_GCM) {
                s_rx_auth_failures++;
                if (s_rx_auth_failures <= 3U) {
                    ESP_LOGW(TAG, "UDP GCM Tag 验证失败，已丢包（ret=-0x%04x）",
                             (unsigned)-crypto_ret);
                }
            } else {
                s_rx_errors++;
            }
            continue;
        }

        const uint32_t rx_sequence = read_be32(buffer + 12);
        if (s_rx_sequence_valid &&
            !sequence_is_newer(rx_sequence, s_rx_sequence)) {
            s_rx_replay_drops++;
            continue;
        }
        s_rx_sequence = rx_sequence;
        s_rx_sequence_valid = true;
        s_rx_packets++;
        if (__atomic_load_n(&s_running, __ATOMIC_ACQUIRE) && s_audio_cb != NULL) {
            s_audio_cb(decrypted, payload_len, s_audio_cb_ctx);
        }
    }
    /* 若 stop 无法及时等到本任务退出，由本任务在最后一次使用密钥后清理；
     * 必须先清理再将任务句柄置空，避免新会话与旧清理并发。 */
    if (!__atomic_load_n(&s_running, __ATOMIC_ACQUIRE) &&
        s_crypto_mutex != NULL) {
        xSemaphoreTake(s_crypto_mutex, portMAX_DELAY);
        gcm_contexts_free();
        s_crypto_mode = CLOUD_UDP_CRYPTO_NONE;
        memset(s_key, 0, sizeof(s_key));
        xSemaphoreGive(s_crypto_mutex);
    }
    /* 本任务用 WithCaps 创建（栈在 PSRAM），必须配对用 WithCaps 删除，
     * 否则每次 stop/start 都会泄漏一块 PSRAM 栈。 */
    s_rx_task = NULL;
    vTaskDeleteWithCaps(NULL);
}

void cloud_udp_set_audio_callback(cloud_udp_audio_cb_t cb, void *ctx)
{
    s_audio_cb = cb;
    s_audio_cb_ctx = ctx;
}

bool cloud_udp_is_ready(void) { return s_ready; }

static esp_err_t cloud_udp_start_locked(const cloud_mqtt_session_t *session)
{
    if (s_ready) {
        return ESP_OK;   /* 幂等 */
    }
    if (s_rx_task != NULL) {
        ESP_LOGE(TAG, "上一 UDP 接收任务尚未退出，拒绝覆盖加密会话");
        return ESP_ERR_INVALID_STATE;
    }
    if (session == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (session->udp_server[0] == '\0' || session->udp_port == 0) {
        ESP_LOGE(TAG, "Server Hello 未给出 udp.server / udp.port，无法建立 UDP");
        return ESP_ERR_INVALID_ARG;
    }
    /* 加密是硬要求：正式主路径为 GCM，同时保留迁移期 CTR；绝不退回明文。 */
    if (strcmp(session->udp_encryption, "aes-128-gcm") == 0) {
        s_crypto_mode = CLOUD_UDP_CRYPTO_GCM;
    } else if (strcmp(session->udp_encryption, "aes-128-ctr") == 0) {
        s_crypto_mode = CLOUD_UDP_CRYPTO_CTR;
    } else {
        ESP_LOGE(TAG, "不支持的 UDP 加密方式：%s（支持 aes-128-gcm / aes-128-ctr）",
                 session->udp_encryption[0] ? session->udp_encryption : "(空)");
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (!session->has_key) {
        ESP_LOGE(TAG, "Server Hello 的 UDP key 缺失或非法（需要 32 个十六进制字符）");
        return ESP_ERR_INVALID_ARG;
    }

    memcpy(s_key, session->key, sizeof(s_key));

    if (s_crypto_mutex == NULL) {
        s_crypto_mutex = xSemaphoreCreateMutexStatic(&s_crypto_mutex_storage);
        if (s_crypto_mutex == NULL) {
            ESP_LOGE(TAG, "创建 UDP 加密互斥锁失败");
            memset(s_key, 0, sizeof(s_key));
            return ESP_ERR_NO_MEM;
        }
    }
    if (s_crypto_mode == CLOUD_UDP_CRYPTO_GCM &&
        gcm_contexts_init() != ESP_OK) {
        memset(s_key, 0, sizeof(s_key));
        s_crypto_mode = CLOUD_UDP_CRYPTO_NONE;
        return ESP_FAIL;
    }

    /* Server Hello 的 nonce[4..7] 携带网关分配的 connection_id。
     * GCM 的 type/flags/length/timestamp/sequence 均按正式包头规则重新生成，
     * 不把整个 nonce 当作 GCM IV。 */
    if (session->has_nonce) {
        s_connection_id = read_be32(session->nonce + 4);
        ESP_LOGI(TAG,
                 "从服务端 nonce 读取 connection_id=0x%08x",
                 (unsigned)s_connection_id);
    } else {
        s_connection_id = 0;
        ESP_LOGW(TAG,
                 "Server Hello 未携带 nonce：connection_id 使用 0，"
                 "正式 GCM 网关可能拒绝该会话");
    }

    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_sock < 0) {
        ESP_LOGE(TAG, "创建 UDP socket 失败");
        gcm_contexts_free();
        s_crypto_mode = CLOUD_UDP_CRYPTO_NONE;
        return ESP_FAIL;
    }

    /* 接收超时：让接收任务能周期性检查退出标志。 */
    struct timeval tv = {
        .tv_sec = CLOUD_UDP_RX_TIMEOUT_MS / 1000,
        .tv_usec = (CLOUD_UDP_RX_TIMEOUT_MS % 1000) * 1000,
    };
    (void)setsockopt(s_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    const esp_err_t resolve_err =
        resolve_udp_peer(session->udp_server, session->udp_port, &s_peer);
    if (resolve_err != ESP_OK) {
        close(s_sock);
        s_sock = -1;
        gcm_contexts_free();
        s_crypto_mode = CLOUD_UDP_CRYPTO_NONE;
        return resolve_err;
    }

    s_sequence = 0;
    s_rx_sequence = 0;
    s_rx_sequence_valid = false;
    s_tx_packets = 0;
    s_tx_errors = 0;
    s_rx_packets = 0;
    s_rx_errors = 0;
    s_rx_auth_failures = 0;
    s_rx_replay_drops = 0;

    __atomic_store_n(&s_running, true, __ATOMIC_RELEASE);
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
        __atomic_store_n(&s_running, false, __ATOMIC_RELEASE);
        close(s_sock);
        s_sock = -1;
        gcm_contexts_free();
        s_crypto_mode = CLOUD_UDP_CRYPTO_NONE;
        return ESP_ERR_NO_MEM;
    }

    if (s_crypto_mode == CLOUD_UDP_CRYPTO_GCM) {
        ESP_LOGI(TAG,
                 "UDP 通道就绪：peer=%s:%u 加密=aes-128-gcm "
                 "key 已装载(16 字节)，connection_id=0x%08x，"
                 "IV=header[4..15] AAD=header Tag=16",
                 session->udp_server, (unsigned)session->udp_port,
                 (unsigned)s_connection_id);
    } else {
        ESP_LOGW(TAG,
                 "UDP 通道使用迁移兼容模式：peer=%s:%u 加密=aes-128-ctr "
                 "connection_id=0x%08x",
                 session->udp_server, (unsigned)session->udp_port,
                 (unsigned)s_connection_id);
    }
    return ESP_OK;
}

esp_err_t cloud_udp_start(const cloud_mqtt_session_t *session)
{
    cloud_udp_lifecycle_lock();
    const esp_err_t err = cloud_udp_start_locked(session);
    cloud_udp_lifecycle_unlock();
    return err;
}

static void cloud_udp_stop_locked(void)
{
    if (s_sock < 0 && !s_ready && s_rx_task == NULL) {
        return;
    }
    __atomic_store_n(&s_running, false, __ATOMIC_RELEASE);
    s_ready = false;

    /* shutdown 不释放 fd，可先唤醒阻塞中的收发；close 必须等加密锁，
     * 不能再在 100 ms 获取锁失败后无保护地关闭或复用同一个 fd。 */
    if (s_sock >= 0) {
        shutdown(s_sock, SHUT_RDWR);
    }
    if (s_crypto_mutex != NULL) {
        xSemaphoreTake(s_crypto_mutex, portMAX_DELAY);
    }
    const int sock = s_sock;
    s_sock = -1;
    if (sock >= 0) {
        close(sock);
    }
    if (s_crypto_mutex != NULL) {
        xSemaphoreGive(s_crypto_mutex);
    }
    /* shutdown 会立即唤醒阻塞中的 recvfrom。等待接收任务退出后再清理 GCM
     * 上下文，避免 stop 与 Tag 验证并发导致 use-after-free。 */
    const TaskHandle_t rx_task = s_rx_task;
    if (rx_task != NULL && rx_task != xTaskGetCurrentTaskHandle()) {
        for (unsigned i = 0; i < 200U && s_rx_task == rx_task; ++i) {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
        if (s_rx_task == rx_task) {
            ESP_LOGW(TAG, "UDP 接收任务未及时退出，暂缓释放加密上下文");
        }
    }
    if (s_crypto_mutex != NULL) {
        xSemaphoreTake(s_crypto_mutex, portMAX_DELAY);
    }
    if (s_rx_task == NULL) {
        gcm_contexts_free();
        s_crypto_mode = CLOUD_UDP_CRYPTO_NONE;
        memset(s_key, 0, sizeof(s_key));
    }
    if (s_crypto_mutex != NULL) {
        xSemaphoreGive(s_crypto_mutex);
    }
    ESP_LOGW(TAG,
             "UDP 通道已关闭（tx=%u/%u rx=%u/%u auth_fail=%u replay=%u）",
             (unsigned)s_tx_packets, (unsigned)s_tx_errors,
             (unsigned)s_rx_packets, (unsigned)s_rx_errors,
             (unsigned)s_rx_auth_failures,
             (unsigned)s_rx_replay_drops);
}

void cloud_udp_stop(void)
{
    cloud_udp_lifecycle_lock();
    cloud_udp_stop_locked();
    cloud_udp_lifecycle_unlock();
}
