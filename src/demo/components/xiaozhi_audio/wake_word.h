#ifndef LUMMISS_WAKE_WORD_H
#define LUMMISS_WAKE_WORD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*wake_word_callback_t)(const char *wake_word, void *user_data);

/* AFE 生产/消费诊断计数，计数器为累计值。 */
typedef struct {
    uint32_t feed_count;
    uint32_t fetch_count;
    uint32_t fetch_null_count;
    uint32_t feed_fail;
    uint32_t wake_detect_count;
    uint32_t output_samples;
    uint32_t output_drop;
    uint32_t fetch_errors;
    uint32_t processed_samples;
    uint32_t processed_peak;
    uint32_t speech_frames;
    uint32_t silence_frames;
    uint32_t speech_begin_count;
    uint32_t speech_end_count;
    bool voice_detected;
    uint32_t feed_samples;  /* 成功送入 AFE 的每通道累计样本数 */
    uint32_t accumulator_remaining_samples; /* 每通道余量，非字节 */
    uint32_t ringbuffer_underflow; /* fetch 返回空/ESP_FAIL 的计数 */
} wake_word_stats_t;

/**
 * @brief 初始化 AFE 唤醒词引擎。
 *
 * 调用前需确保 SPIFFS 已挂载，"storage" 分区中存在 Wakenet 模型文件。
 * 该函数会加载模型、创建 AFE 实例并启动内部检测任务。
 *
 * @param channels      1 为单麦 M；3 为双麦加播放参考 MMR。
 * @return ESP_OK 成功，否则失败
 */
esp_err_t wake_word_init(int channels);

/**
 * @brief 注册唤醒词检测回调。
 *
 * 当唤醒词被检测到时，回调会在 AFE 检测任务上下文中调用。
 *
 * @param callback  回调函数指针
 * @param user_data 用户数据，回调时透传
 */
void wake_word_set_callback(wake_word_callback_t callback, void *user_data);

/**
 * @brief 启动唤醒词检测。
 *
 * 启用 WakeNet；AFE 持续处理，由检测任务更新控制状态。
 */
void wake_word_start(void);

/**
 * @brief 停止唤醒词检测。
 *
 * 只停止 WakeNet，保留双麦 AFE 供对话上行使用。
 */
void wake_word_stop(void);

/**
 * @brief 获取单次喂入音频的采样点数。
 *
 * 调用方应按此大小从麦克风读取数据后调用 feed()。
 *
 * @return 每次喂入要求的 16-bit 样本数
 */
size_t wake_word_get_feed_size(void);

/**
 * @brief 喂入音频数据到唤醒词检测器。
 *
 * 内部会缓存不足一个 chunk 的剩余数据，凑满后送入 AFE。
 *
 * @param data   16 kHz/16-bit PCM，按初始化的 M 或 MMR 顺序交错。
 * @param count  样本数量
 */
void wake_word_feed(const int16_t *data, size_t count);

/* 与 WakeNet 独立：只控制增强 PCM 输出，不按 VAD 筛选数据。 */
void wake_word_enable_voice_processing(bool enabled);
bool wake_word_voice_detected(void);
/* 旧协议兼容入口。 */
void wake_word_set_uplink(bool enabled);
/* 非阻塞读取完整的单声道 AFE 输出帧，不足 samples 时返回 0。 */
size_t wake_word_read_pcm(int16_t *output, size_t samples);

/** @brief 读取 AFE feed/fetch/WakeNet 诊断计数。 */
void wake_word_get_stats(wake_word_stats_t *stats);

/**
 * @brief 查询是否已检测到唤醒词。
 *
 * 检测到后此标志保持置位，直到 start() 或 stop() 被调用。
 *
 * @return true  已检测到唤醒词
 * @return false 未检测到
 */
bool wake_word_is_detected(void);

/**
 * @brief 获取最近一次检测到的唤醒词字符串。
 *
 * @return 唤醒词字符串（如 "你好小智"），未检测到时返回 "unknown"
 */
const char *wake_word_get_last(void);

/**
 * @brief 复制唤醒前保留的 PCM 音频。
 *
 * 参考开发板示例，唤醒检测期间保留最近约 2 秒的 AFE 输出。检测成功后，
 * 上层可将这些音频编码并先发给服务端，避免吞掉唤醒词和紧随其后的语音。
 *
 * @param output       输出缓冲；传 NULL 时只返回当前可复制的样本数
 * @param max_samples  输出缓冲最多可容纳的样本数
 * @return 实际可用或已复制的 16 kHz 单声道样本数
 */
size_t wake_word_copy_preroll(int16_t *output, size_t max_samples);

/**
 * @brief 反初始化并释放所有 AFE 资源。
 * 调用方必须停止 feed/输出读取并串行化 init/deinit；本函数等待 INTERNAL
 * worker 完成 fetch 退出、AFE 销毁和模型 munmap 后才返回；B 组还同步回收其栈/TCB。
 */
esp_err_t wake_word_deinit(void);

#ifdef __cplusplus
}
#endif

#endif /* LUMMISS_WAKE_WORD_H */
