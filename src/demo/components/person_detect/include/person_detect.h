#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* 推理输入始终由本模块转换为 320x320 RGB565，避免 ESP-DL 对 640x480
 * 走内部 RGB565 resize SIMD 路径。主摄像头仍保持 640x480。 */

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
