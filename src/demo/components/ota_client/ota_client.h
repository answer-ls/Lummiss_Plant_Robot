#ifndef LUMMISS_OTA_CLIENT_H
#define LUMMISS_OTA_CLIENT_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define OTA_WS_URL_MAX_LEN    256
#define OTA_TOKEN_MAX_LEN     512
#define OTA_FW_URL_MAX_LEN    512
#define OTA_ACT_MSG_MAX_LEN   256

typedef struct {
    char server_time[48];
    char timezone[48];
    int32_t timezone_offset;

    char websocket_url[OTA_WS_URL_MAX_LEN];
    char websocket_token[OTA_TOKEN_MAX_LEN];

    char firmware_url[OTA_FW_URL_MAX_LEN];
    char firmware_version[32];

    char activation_code[16];
    char activation_message[OTA_ACT_MSG_MAX_LEN];
    bool has_activation;
    bool has_firmware;

    char error[128];
    bool has_error;
} ota_result_t;

esp_err_t ota_client_init(void);

esp_err_t ota_client_check(ota_result_t *result);

#endif