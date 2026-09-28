#ifndef LUMMISS_CAMERA_PHOTO_H
#define LUMMISS_CAMERA_PHOTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 第一阶段固定抓取当前 UVC 流中的 1280×720 MJPEG。 */
#define CAMERA_PHOTO_WIDTH            1280U
#define CAMERA_PHOTO_HEIGHT           720U
#define CAMERA_PHOTO_TIMEOUT_MS       5000U
#define CAMERA_PHOTO_BUFFER_CAPACITY  (512U * 1024U)
#define CAMERA_PHOTO_TEST_PATH        "/sdcard/photo_test.jpg"

typedef enum {
    PHOTO_IDLE = 0,
    PHOTO_REQUESTED, /* 请求已入队，等待 photo_task 开始接收共享 MJPEG 槽 */
    PHOTO_WAIT_FRAME,
    PHOTO_CAPTURING,  /* 内部状态：正在接管 Camera handoff 槽引用 */
    PHOTO_READY,
    PHOTO_UPLOADING, /* 第一阶段对应写入 TF；后续在这里替换为 HTTPS 上传 */
    PHOTO_DONE,
    PHOTO_FAILED,
} camera_photo_state_t;

typedef enum {
    CAMERA_PHOTO_OK = 0,
    CAMERA_PHOTO_NO_MEMORY,
    CAMERA_PHOTO_TIMEOUT,
    CAMERA_PHOTO_TOO_LARGE,
    CAMERA_PHOTO_STORAGE_UNAVAILABLE,
    CAMERA_PHOTO_SAVE_FAILED,
} camera_photo_status_t;

typedef struct {
    camera_photo_status_t status;
    const char *path;
    size_t jpeg_size;
    uint32_t capture_ms;
    uint32_t total_ms;
} camera_photo_result_t;

typedef void (*camera_photo_complete_cb_t)(const camera_photo_result_t *result,
                                           void *ctx);
typedef void (*camera_photo_frame_release_cb_t)(void *ctx);

/* 只创建队列与任务，不永久申请 JPEG Buffer。 */
esp_err_t camera_photo_init(void);

/* 非阻塞请求拍照。同一时刻只允许一个请求；完成结果由 photo_task 回调。 */
esp_err_t camera_photo_request(camera_photo_complete_cb_t complete_cb, void *ctx);

/* 返回拍照任务与请求队列是否已经创建完成。 */
bool camera_photo_is_initialized(void);

/* 提交 camera handoff 池中已复制完成的 MJPEG 槽。返回 true 表示拍照任务
 * 接管该引用，并会在保存结束后调用 release_cb；返回 false 时由调用方归还。 */
bool camera_photo_submit_mjpeg_owned(
    const uint8_t *data, size_t data_len, uint16_t width, uint16_t height,
    camera_photo_frame_release_cb_t release_cb, void *release_ctx);

camera_photo_state_t camera_photo_get_state(void);
const char *camera_photo_status_name(camera_photo_status_t status);

#ifdef __cplusplus
}
#endif

#endif /* LUMMISS_CAMERA_PHOTO_H */
