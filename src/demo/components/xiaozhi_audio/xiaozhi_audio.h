#ifndef LUMMISS_XIAOZHI_AUDIO_H
#define LUMMISS_XIAOZHI_AUDIO_H

#include <stdbool.h>

#include "esp_err.h"

/* 原始四槽采集测试；仅在独立诊断档位调用。 */
esp_err_t xiaozhi_audio_start_raw_test(void);

/* 启动板载 ES8311 音频和小智会话管理任务。重复调用不会重复创建任务。 */
esp_err_t xiaozhi_audio_start(void);
/* RTC 控制任务同步请求：释放共享 AFE，或恢复双麦 AFE 后回到唤醒待机。 */
esp_err_t xiaozhi_audio_set_rtc_suspended(bool suspend);

/* 档位 9 使用的纯本地硬件自检：只初始化 ES8311/I2S，持续打印麦克风
 * 电平，并周期播放短测试音。不启动 WakeNet、Opus、网络或小智会话。 */
esp_err_t xiaozhi_audio_start_hardware_test(void);

/* 查询小智传输和语音通道状态，供后续 UI 状态机使用。 */
bool xiaozhi_audio_is_connected(void);
bool xiaozhi_audio_is_listening(void);
bool xiaozhi_audio_is_speaking(void);

/* 查询唤醒词检测状态。 */
bool xiaozhi_audio_is_wake_detected(void);
const char *xiaozhi_audio_get_wake_word(void);

/* MCP 音量工具使用的本地音量控制接口，范围为 0~100。 */
esp_err_t xiaozhi_audio_set_volume(int volume);
int xiaozhi_audio_get_volume(void);

#endif /* LUMMISS_XIAOZHI_AUDIO_H */
