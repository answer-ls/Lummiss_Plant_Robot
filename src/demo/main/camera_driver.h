#ifndef LUMMISS_CAMERA_DRIVER_H
#define LUMMISS_CAMERA_DRIVER_H

/* 第一阶段 UVC-only 隔离测试：只启动 USB Host/UVC 和统计，跳过
 * WiFi/ESP-Hosted、H.264/WebSocket、LVGL、天气 HTTPS 及 GIF/SD 任务。
 * 测试完成后改为 0，即可恢复完整应用。 */
#define CAMERA_UVC_ONLY_TEST 1

/* 初始化 USB Host/UVC 并持续管理摄像头视频流。
 * 该函数为阻塞循环，应在独立的 Camera Task 中调用。 */
void camera_driver_run(void);

#endif /* LUMMISS_CAMERA_DRIVER_H */
