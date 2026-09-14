#ifndef LUMMISS_ANIM_BIN_PLAYER_H
#define LUMMISS_ANIM_BIN_PLAYER_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "lvgl.h"

/* 在 LVGL 上下文或持有 lvgl_port_lock() 时调用一次。parent 应为表情页面。 */
esp_err_t anim_bin_player_init(lv_obj_t *parent);

/* 异步打开并播放 LUM1 文件，不在调用者线程执行 SD 读取。 */
esp_err_t anim_bin_player_play(const char *path);

/* 异步停止。两个 PSRAM 帧缓冲会保留给下一次播放复用。 */
void anim_bin_player_stop(void);

bool anim_bin_player_is_playing(void);
bool anim_bin_player_is_loading(void);
esp_err_t anim_bin_player_get_last_error(void);
uint16_t anim_bin_player_get_frame_index(void);

#endif /* LUMMISS_ANIM_BIN_PLAYER_H */
