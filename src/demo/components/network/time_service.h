#ifndef LUMMISS_TIME_SERVICE_H
#define LUMMISS_TIME_SERVICE_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/* 全局唯一的 SNTP 客户端 —— 全工程只有这里能初始化时间同步。
 *
 * 为什么必须唯一（2026-09-18 实机踩到）：
 *   1) `esp_netif_sntp_init()` 第二次调用会直接报
 *      "esp_netif_sntp already initialized"；
 *   2) 更要命的是 `esp_netif_sntp_sync_wait()` 内部是
 *      `xQueueSemaphoreTake(二值信号量)` —— **只能被消费一次**。
 *      第一个调用方把信号量取走后，第二个调用方会白等满整个超时再返回
 *      ESP_ERR_TIMEOUT。现象就是：app_main 明明已经校时成功，home_info
 *      却报"网络校时超时，稍后重试"。
 *
 * 所以同步状态改由**粘性事件位**广播：任何调用方在任何时刻来问都能立刻得到
 * 正确答案；再叠加一层"系统墙钟是否已经合理"的兜底判断，避免回调万一没送到
 * 就永远等下去。
 *
 * 启动顺序（文档 §2）：WiFi 拿到 IP → time_service_start() → 等同步 → OTA。 */

/* 初始化并启动 SNTP。幂等：重复调用直接返回 ESP_OK。
 * 必须在 WiFi 联网之后调用，否则拿不到 DNS/路由。 */
esp_err_t time_service_start(void);

/* 等到时间同步完成。已经同步过就立即返回 true，可被任意多个调用方反复调用。 */
bool time_service_wait_synced(uint32_t timeout_ms);

/* 非阻塞查询当前是否已同步。 */
bool time_service_is_synced(void);

#endif
