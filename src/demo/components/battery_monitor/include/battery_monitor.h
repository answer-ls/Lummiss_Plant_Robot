#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    bool available;
    uint8_t percent;
    uint16_t voltage_mv;
} battery_monitor_snapshot_t;

/* 初始化 CW2015 并启动低频轮询；读取失败不会阻止设备其它功能启动。 */
esp_err_t battery_monitor_init(void);

/* 读取最近一次采样缓存，不执行 I2C 操作。 */
bool battery_monitor_get_snapshot(battery_monitor_snapshot_t *snapshot);
