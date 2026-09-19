#ifndef LUMMISS_CAMERA_DRIVER_H
#define LUMMISS_CAMERA_DRIVER_H

#include <stdbool.h>
#include <stdint.h>

#include "test_profile.h"
#include "esp_err.h"

/* 初始化 USB Host/UVC 并持续管理摄像头视频流。
 * 该函数为阻塞循环，应在独立的 Camera Task 中调用。 */
void camera_driver_run(void);

/* 在创建 Camera Task 前准备 CAMERA_READY 事件；调用方随后可以等待
 * UVC stream open 成功且收到第一帧有效 MJPEG。 */
esp_err_t camera_driver_prepare_ready_event(void);

/* 等待真实摄像头就绪。返回 true 的前提是 stream 已打开并收到首帧，
 * 超时或摄像头异常时返回 false。 */
bool camera_driver_wait_ready(uint32_t timeout_ms);

#endif /* LUMMISS_CAMERA_DRIVER_H */
