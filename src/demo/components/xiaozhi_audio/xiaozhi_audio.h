#ifndef LUMMISS_XIAOZHI_AUDIO_H
#define LUMMISS_XIAOZHI_AUDIO_H

#include <stdbool.h>

#include "esp_err.h"

/* 启动板载 ES8311 音频和小智会话管理任务。重复调用不会重复创建任务。 */
esp_err_t xiaozhi_audio_start(void);

/* 查询小智传输和语音通道状态，供后续 UI 状态机使用。 */
bool xiaozhi_audio_is_connected(void);
bool xiaozhi_audio_is_listening(void);
bool xiaozhi_audio_is_speaking(void);

#endif /* LUMMISS_XIAOZHI_AUDIO_H */
