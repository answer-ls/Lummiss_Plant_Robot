#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "network_manager.h"
#include "mjpeg_streamer.h"

static const char *TAG = "MJPEG_STREAM";

#define MJPEG_STREAM_WS_URL       "ws://192.168.1.66:8001/ws"
#define MJPEG_WS_HEADER_SIZE      16U
#define MJPEG_WS_MAGIC            0x314A4D4CU /* 'L','M','J','1' 小端 */
#define MJPEG_SLOT_COUNT          3U
#define MJPEG_TASK_STACK          6144
#define MJPEG_TASK_PRIORITY       9
#define MJPEG_TASK_CORE           0
#define MJPEG_RECONNECT_MS        2000
/* 读超时与写超时量的是两件事，不能共用一个宏。
 *
 * 写超时是 esp_websocket_client_send_bin() 的预算，最终落在
 * transport_ws.c:385 的一次 select 可写等待上。等不到就返回 <=0，而
 * esp_websocket_client.c:745 把 wlen <= 0 判为致命 → abort_connection →
 * MJPEG_RECONNECT_MS 之后才重连。也就是说"链路抖 X 毫秒"会被放大成
 * "X 毫秒 + 2 秒全黑"：原来 300ms 的预算意味着 300ms 的不可能写就要赔 2 秒。
 * 实测 33 秒里断了 5 次（每次 ~2.06s）≈ 33% 的时间没有画面，必须放宽。
 *
 * 读超时只是 esp_transport_read() 单次读的上限（组件内 :1085）。稳态收不到
 * 东西时主循环停在 esp_transport_poll_read(1000) 上、且已提前放掉 client->lock
 * （:1372），所以它不占锁、不影响发送；真正走到这次读，一定已经确认有数据，
 * 而 PC 端只下发几十字节的文本命令。100ms 足够。
 *
 * （写超时只约束那次 select，不约束随后的 send()——socket 是阻塞的。
 * 链路彻底卡死时 send() 要等 TCP 重传耗尽才返回，这是已知代价。） */
#define MJPEG_READ_TIMEOUT_MS     100
#define MJPEG_SEND_TIMEOUT_MS     2000
#define MJPEG_REPORT_INTERVAL_US  (10 * 1000 * 1000LL)
#define MJPEG_SUBMIT_EARLY_US     5000

typedef struct {
    uint8_t *data; /* [16 字节帧头][原始 JPEG] */
    size_t data_len;
    uint32_t sequence;
    bool busy;
} mjpeg_slot_t;

static mjpeg_slot_t s_slots[MJPEG_SLOT_COUNT];
static QueueHandle_t s_ready_queue;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_initialized;
static bool s_enabled = true;
static uint32_t s_sequence;
static int64_t s_next_submit_us;
static uint32_t s_submitted;
static uint32_t s_sent;
static uint32_t s_dropped;
static uint32_t s_failed;
static uint64_t s_sent_bytes;

static bool mjpeg_extract_cmd(const char *json, char *out, size_t out_size)
{
    const char *key = strstr(json, "\"cmd\"");
    if (key == NULL) {
        return false;
    }
    const char *colon = strchr(key + 5, ':');
    const char *open = colon ? strchr(colon + 1, '"') : NULL;
    const char *close = open ? strchr(open + 1, '"') : NULL;
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

static void mjpeg_fill_header(uint8_t *header, uint32_t sequence)
{
    const uint32_t magic = MJPEG_WS_MAGIC;
    const uint16_t width = VIDEO_STREAM_WIDTH;
    const uint16_t height = VIDEO_STREAM_HEIGHT;
    const uint16_t fps = 30;
    memcpy(header + 0, &magic, sizeof(magic));
    memcpy(header + 4, &width, sizeof(width));
    memcpy(header + 6, &height, sizeof(height));
    memcpy(header + 8, &fps, sizeof(fps));
    header[10] = 0; /* MJPEG 没有 H.264 frame_type */
    header[11] = 0;
    memcpy(header + 12, &sequence, sizeof(sequence));
}

void mjpeg_streamer_set_enabled(bool enabled)
{
    portENTER_CRITICAL(&s_lock);
    s_enabled = enabled;
    portEXIT_CRITICAL(&s_lock);
    ESP_LOGI(TAG, "MJPEG 直通已%s", enabled ? "开启" : "关闭");
}

bool mjpeg_streamer_is_enabled(void)
{
    bool enabled;
    portENTER_CRITICAL(&s_lock);
    enabled = s_enabled;
    portEXIT_CRITICAL(&s_lock);
    return enabled;
}

static void mjpeg_send_command_reply(esp_websocket_client_handle_t client,
                                      const char *command)
{
    char cmd[16];
    char reply[192];
    if (!mjpeg_extract_cmd(command, cmd, sizeof(cmd))) {
        snprintf(reply, sizeof(reply), "{\"ok\":false,\"error\":\"bad command\"}");
    } else if (strcmp(cmd, "ping") == 0) {
        snprintf(reply, sizeof(reply), "{\"ok\":true,\"cmd\":\"ping\"}");
    } else if (strcmp(cmd, "video_on") == 0 || strcmp(cmd, "video_off") == 0) {
        const bool enabled = strcmp(cmd, "video_on") == 0;
        mjpeg_streamer_set_enabled(enabled);
        snprintf(reply, sizeof(reply),
                 "{\"ok\":true,\"cmd\":\"%s\",\"video_enabled\":%s}",
                 cmd, enabled ? "true" : "false");
    } else if (strcmp(cmd, "status") == 0) {
        const bool enabled = mjpeg_streamer_is_enabled();
        snprintf(reply, sizeof(reply),
                 "{\"ok\":true,\"cmd\":\"status\",\"mode\":\"mjpeg\","
                 "\"width\":%d,\"height\":%d,\"fps\":30,\"video_enabled\":%s}",
                 VIDEO_STREAM_WIDTH, VIDEO_STREAM_HEIGHT,
                 enabled ? "true" : "false");
    } else {
        snprintf(reply, sizeof(reply),
                 "{\"ok\":false,\"error\":\"unknown cmd\",\"cmd\":\"%s\"}", cmd);
    }
    if (esp_websocket_client_send_text(client, reply, (int)strlen(reply),
                                       pdMS_TO_TICKS(MJPEG_SEND_TIMEOUT_MS)) < 0) {
        ESP_LOGW(TAG, "MJPEG 命令响应发送失败");
    }
}

static void mjpeg_ws_event_handler(void *handler_args, esp_event_base_t base,
                                   int32_t event_id, void *event_data)
{
    (void)handler_args;
    (void)base;
    const esp_websocket_event_data_t *data = event_data;
    switch ((esp_websocket_event_id_t)event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MJPEG WebSocket 已连接：%s", MJPEG_STREAM_WS_URL);
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_CLOSED:
        ESP_LOGW(TAG, "MJPEG WebSocket 断开，等待自动重连");
        break;
    case WEBSOCKET_EVENT_DATA:
        if (data != NULL && data->op_code == 0x01 && data->data_ptr != NULL && data->fin) {
            char command[192];
            const size_t length = (size_t)data->data_len;
            if (length < sizeof(command)) {
                memcpy(command, data->data_ptr, length);
                command[length] = '\0';
                mjpeg_send_command_reply(data->client, command);
            }
        }
        break;
    default:
        break;
    }
}

static void mjpeg_report(int64_t now_us)
{
    static int64_t last_report_us;
    static uint32_t last_sent;
    static uint64_t last_bytes;
    if (last_report_us == 0) {
        last_report_us = now_us;
        last_sent = s_sent;
        last_bytes = s_sent_bytes;
        return;
    }
    if (now_us - last_report_us < MJPEG_REPORT_INTERVAL_US) {
        return;
    }
    const double seconds = (now_us - last_report_us) / 1000000.0;
    ESP_LOGI(TAG, "MJPEG直通：发送=%.1f fps，%.0f kbps，累计提交=%" PRIu32
             " 丢弃=%" PRIu32 " 失败=%" PRIu32,
             (s_sent - last_sent) / seconds,
             (s_sent_bytes - last_bytes) * 8.0 / seconds / 1000.0,
             s_submitted, s_dropped, s_failed);
    last_report_us = now_us;
    last_sent = s_sent;
    last_bytes = s_sent_bytes;
}

static void mjpeg_upload_task(void *arg)
{
    (void)arg;
    const esp_websocket_client_config_t config = {
        .uri = MJPEG_STREAM_WS_URL,
        .buffer_size = 65536,
        .task_stack = MJPEG_TASK_STACK,
        .task_prio = 5,
        .task_core_id_set = true,
        .task_core_id = MJPEG_TASK_CORE,
        .disable_auto_reconnect = false,
        .enable_close_reconnect = true,
        .reconnect_timeout_ms = MJPEG_RECONNECT_MS,
        .network_timeout_ms = MJPEG_READ_TIMEOUT_MS,
    };
    esp_websocket_client_handle_t client = esp_websocket_client_init(&config);
    if (client == NULL || esp_websocket_register_events(client, WEBSOCKET_EVENT_ANY,
                                                         mjpeg_ws_event_handler, NULL) != ESP_OK ||
        esp_websocket_client_start(client) != ESP_OK) {
        ESP_LOGE(TAG, "MJPEG WebSocket 初始化失败");
        vTaskDelete(NULL);
        return;
    }

    while (true) {
        unsigned index;
        if (xQueueReceive(s_ready_queue, &index, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        mjpeg_slot_t *slot = &s_slots[index];
        int sent = -1;
        if (mjpeg_streamer_is_enabled() && network_manager_is_connected() &&
            esp_websocket_client_is_connected(client)) {
            sent = esp_websocket_client_send_bin(
                client, (const char *)slot->data, (int)slot->data_len,
                pdMS_TO_TICKS(MJPEG_SEND_TIMEOUT_MS));
        }
        if (sent == (int)slot->data_len) {
            s_sent++;
            s_sent_bytes += slot->data_len - MJPEG_WS_HEADER_SIZE;
        } else if (sent < 0) {
            s_failed++;
        } else {
            s_failed++;
        }
        portENTER_CRITICAL(&s_lock);
        slot->busy = false;
        portEXIT_CRITICAL(&s_lock);
        mjpeg_report(esp_timer_get_time());
    }
}

esp_err_t mjpeg_streamer_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }
    s_ready_queue = xQueueCreate(MJPEG_SLOT_COUNT, sizeof(unsigned));
    if (s_ready_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }
    for (unsigned i = 0; i < MJPEG_SLOT_COUNT; ++i) {
        s_slots[i].data = heap_caps_malloc(MJPEG_WS_HEADER_SIZE + VIDEO_STREAM_JPEG_MAX_SIZE,
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_slots[i].data == NULL) {
            for (unsigned j = 0; j < i; ++j) {
                heap_caps_free(s_slots[j].data);
            }
            vQueueDelete(s_ready_queue);
            s_ready_queue = NULL;
            return ESP_ERR_NO_MEM;
        }
    }
    if (xTaskCreatePinnedToCore(mjpeg_upload_task, "mjpeg_upload", 8192, NULL,
                                MJPEG_TASK_PRIORITY, NULL, MJPEG_TASK_CORE) != pdPASS) {
        for (unsigned i = 0; i < MJPEG_SLOT_COUNT; ++i) {
            heap_caps_free(s_slots[i].data);
            s_slots[i].data = NULL;
        }
        vQueueDelete(s_ready_queue);
        s_ready_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    s_initialized = true;
    ESP_LOGI(TAG, "MJPEG 直通已就绪：%ux%u@30fps，WebSocket=%s",
             VIDEO_STREAM_WIDTH, VIDEO_STREAM_HEIGHT, MJPEG_STREAM_WS_URL);
    return ESP_OK;
}

bool mjpeg_streamer_submit_jpeg(const uint8_t *data, size_t data_len)
{
    if (!s_initialized || !mjpeg_streamer_is_enabled() ||
        !network_manager_is_connected() || data == NULL || data_len < 4 ||
        data_len > VIDEO_STREAM_JPEG_MAX_SIZE || data[0] != 0xff || data[1] != 0xd8 ||
        data[data_len - 2] != 0xff || data[data_len - 1] != 0xd9) {
        return false;
    }
    const int64_t now_us = esp_timer_get_time();
    const int64_t frame_interval_us = 1000000LL / 30;
    if (s_next_submit_us != 0 && now_us + MJPEG_SUBMIT_EARLY_US < s_next_submit_us) {
        return false;
    }

    unsigned selected = MJPEG_SLOT_COUNT;
    portENTER_CRITICAL(&s_lock);
    for (unsigned i = 0; i < MJPEG_SLOT_COUNT; ++i) {
        if (!s_slots[i].busy) {
            s_slots[i].busy = true;
            selected = i;
            break;
        }
    }
    portEXIT_CRITICAL(&s_lock);
    if (selected == MJPEG_SLOT_COUNT) {
        s_dropped++;
        return false;
    }

    mjpeg_slot_t *slot = &s_slots[selected];
    const uint32_t sequence = ++s_sequence;
    mjpeg_fill_header(slot->data, sequence);
    memcpy(slot->data + MJPEG_WS_HEADER_SIZE, data, data_len);
    slot->data_len = MJPEG_WS_HEADER_SIZE + data_len;
    slot->sequence = sequence;
    if (xQueueSend(s_ready_queue, &selected, 0) != pdTRUE) {
        portENTER_CRITICAL(&s_lock);
        slot->busy = false;
        portEXIT_CRITICAL(&s_lock);
        s_dropped++;
        return false;
    }
    s_submitted++;
    s_next_submit_us = (s_next_submit_us == 0 || now_us - s_next_submit_us >= frame_interval_us) ?
                       now_us + frame_interval_us : s_next_submit_us + frame_interval_us;
    return true;
}
