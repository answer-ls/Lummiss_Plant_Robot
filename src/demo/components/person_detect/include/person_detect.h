#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* 推理输入始终由本模块转换为 320x320 RGB565，支持当前 1280x720
 * 和 640x480 摄像头配置，避免 ESP-DL 对大图走内部 resize SIMD 路径。 */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 初始化本地人体检测任务。
 *
 * 函数只创建单槽队列和任务，模型在任务上下文中加载，避免阻塞 app_main。
 * 模型文件不存在时会记录明确日志并禁用检测，不影响其它业务启动。
 */
esp_err_t person_detect_init(void);

/** 等待模型加载及 JPEG 解码器初始化结束；超时返回 ESP_ERR_TIMEOUT。 */
esp_err_t person_detect_wait_startup(uint32_t timeout_ms);

/* 仅事件上报测试档位开启；正式档位默认不向云端发送预警。 */
void person_detect_set_alert_report_enabled(bool enabled);

/**
 * 从现有 camera handoff 任务提交一张 MJPEG 快照。
 *
 * 这是非阻塞接口：每秒最多接受一张，AI 忙或队列已有帧时直接返回 false。
 * 调用者可以立即继续把原始帧交给视频编码链路。
 */
bool person_detect_submit_mjpeg(const uint8_t *data, size_t size);

/** 停止任务并释放检测器、JPEG 解码器及缓冲区。 */
void person_detect_deinit(void);

#ifdef __cplusplus
}
#endif
