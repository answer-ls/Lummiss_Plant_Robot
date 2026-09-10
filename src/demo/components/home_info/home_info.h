#ifndef LUMMISS_HOME_INFO_H
#define LUMMISS_HOME_INFO_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    bool time_valid;
    bool weather_valid;
    int year;
    int month;
    int day;
    int weekday;
    int hour;
    int minute;
    float temperature_c;
    int weather_code;
    char city[32];
} home_info_snapshot_t;

/* 启动首页信息服务。服务会等待 WiFi，自动定位并定时刷新天气。 */
esp_err_t home_info_start(void);

/* 取得供 UI 显示的快照；函数内部完成并发保护。 */
bool home_info_get_snapshot(home_info_snapshot_t *snapshot);

/* 把 Open-Meteo 的 WMO 天气码转换为简短中文名称。 */
const char *home_info_weather_text(int weather_code);

#endif /* LUMMISS_HOME_INFO_H */
