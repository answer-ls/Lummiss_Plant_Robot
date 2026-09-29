#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* 工作区由 codec 独占并放 PSRAM；不申请 DMA，不重建像素。 */
typedef struct {
    int32_t first[17], last[17], base[17];
    uint8_t values[256];
    uint16_t lookup[256];
    bool valid;
} jpeg_integrity_table_t;
typedef struct {
    jpeg_integrity_table_t tables[8];
} jpeg_integrity_workspace_t;
typedef enum {
    JPEG_CHECK_OK, JPEG_CHECK_HEADER, JPEG_CHECK_RST, JPEG_CHECK_MCU,
    JPEG_CHECK_EXTRA, JPEG_CHECK_PADDING, JPEG_CHECK_COUNT
} jpeg_integrity_kind_t;
typedef struct {
    const char *reason;
    uint32_t segment, mcus, expected, offset;
    jpeg_integrity_kind_t kind;
    uint32_t padding_segments;
} jpeg_integrity_result_t;
bool jpeg_integrity_check(jpeg_integrity_workspace_t *work, const uint8_t *data,
                          size_t size, bool allow_padding, jpeg_integrity_result_t *result);
