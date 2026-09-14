#ifndef LUMMISS_CAMERA_DRIVER_H
#define LUMMISS_CAMERA_DRIVER_H

#include "test_profile.h"

/* 初始化 USB Host/UVC 并持续管理摄像头视频流。
 * 该函数为阻塞循环，应在独立的 Camera Task 中调用。 */
void camera_driver_run(void);

#endif /* LUMMISS_CAMERA_DRIVER_H */
