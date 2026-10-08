#pragma once
#include "esp_log.h"
// 参考库仅使用这些日志宏；适配成IDF日志，不改变控制流程。
#define ESP_UTILS_LOGI(...) ESP_LOGI(ESP_UTILS_LOG_TAG, __VA_ARGS__)
#define ESP_UTILS_LOGW(...) ESP_LOGW(ESP_UTILS_LOG_TAG, __VA_ARGS__)
#define ESP_UTILS_LOGE(...) ESP_LOGE(ESP_UTILS_LOG_TAG, __VA_ARGS__)
#define ESP_UTILS_LOGD(...) ESP_LOGD(ESP_UTILS_LOG_TAG, __VA_ARGS__)
