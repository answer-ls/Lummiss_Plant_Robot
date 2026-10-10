#ifndef LUMMISS_ALERT_CLIP_H
#define LUMMISS_ALERT_CLIP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 预警档位启动一次；编码任务每帧提交 Annex-B，识人任务触发一次事件。 */
esp_err_t alert_clip_init(void);
void alert_clip_push_h264(const uint8_t *data, size_t len, bool idr);
bool alert_clip_trigger(const char *event_id);
void alert_clip_cancel(const char *event_id);

/* MQTT 回调只保存授权；TF 写入和 HTTPS 上传由独立低优先级任务执行。 */
bool alert_clip_set_grant(const char *event_id, const char *alert_id,
                          const char *ticket, const char *upload_path,
                          uint32_t expires_in, size_t max_bytes);

#ifdef __cplusplus
}
#endif

#endif
