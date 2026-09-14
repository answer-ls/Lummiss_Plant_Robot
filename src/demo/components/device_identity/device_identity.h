#ifndef LUMMISS_DEVICE_IDENTITY_H
#define LUMMISS_DEVICE_IDENTITY_H

#include <stdbool.h>
#include "esp_err.h"

#define DEVICE_IDENTITY_MAX_LEN 64

typedef struct {
    char device_id[DEVICE_IDENTITY_MAX_LEN];
    char client_id[DEVICE_IDENTITY_MAX_LEN];
} device_identity_t;

esp_err_t device_identity_init(void);

const device_identity_t *device_identity_get(void);

#endif