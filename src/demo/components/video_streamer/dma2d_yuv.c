#include "dma2d_yuv.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_cache.h"
#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "esp_private/dma2d.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "hal/dma2d_types.h"
#include "soc/dma2d_channel.h"

#define DMA2D_YUV_POOL_ID              0U
#define DMA2D_YUV_CACHE_ALIGN          64U
#define DMA2D_YUV_BLOCK_WIDTH          32U
#define DMA2D_YUV_BLOCK_HEIGHT         8U
#define DMA2D_YUV_DMA_TIMEOUT_MS       100U

/*
 * This is deliberately kept as one static context.  The video codec task is
 * the only caller, while the two DMA2D callbacks only touch the semaphore.
 * Keeping the callback context in internal RAM also satisfies
 * CONFIG_DMA2D_ISR_IRAM_SAFE builds.
 */
typedef struct {
    dma2d_pool_handle_t pool;
    dma2d_descriptor_t *tx_desc;
    dma2d_descriptor_t *rx_desc;
    dma2d_trans_t *trans_placeholder;
    dma2d_transfer_ability_t transfer_ability;
    dma2d_csc_config_t rx_csc;
    dma2d_trans_config_t trans_config;
    StaticSemaphore_t done_storage;
    SemaphoreHandle_t done;
    bool initialized;
    volatile bool in_flight;
} dma2d_yuv_context_t;

static dma2d_yuv_context_t s_dma2d_yuv;

static void dma2d_yuv_desc_init(dma2d_descriptor_t *desc,
                                uint8_t *buffer,
                                uint32_t width,
                                uint32_t height,
                                uint32_t eof,
                                uint32_t pbyte)
{
    memset(desc, 0, sizeof(*desc));
    desc->dma2d_en = 1;
    desc->mode = DMA2D_DESCRIPTOR_BLOCK_RW_MODE_MULTIPLE;
    desc->vb_size = DMA2D_YUV_BLOCK_HEIGHT;
    desc->hb_length = DMA2D_YUV_BLOCK_WIDTH;
    desc->suc_eof = eof;
    desc->owner = DMA2D_DESCRIPTOR_BUFFER_OWNER_DMA;
    desc->va_size = height;
    desc->ha_length = width;
    desc->pbyte = pbyte;
    desc->x = 0;
    desc->y = 0;
    desc->buffer = buffer;
    desc->next = NULL;
}

static bool IRAM_ATTR dma2d_yuv_done_cb(dma2d_channel_handle_t dma2d_chan,
                                        dma2d_event_data_t *event_data,
                                        void *user_data)
{
    (void)dma2d_chan;
    (void)event_data;
    dma2d_yuv_context_t *ctx = (dma2d_yuv_context_t *)user_data;
    BaseType_t higher_priority_task_woken = pdFALSE;
    ctx->in_flight = false;
    xSemaphoreGiveFromISR(ctx->done, &higher_priority_task_woken);
    return higher_priority_task_woken == pdTRUE;
}

static bool IRAM_ATTR dma2d_yuv_on_picked(uint32_t channel_num,
                                          const dma2d_trans_channel_info_t *channels,
                                          void *user_config)
{
    if (channel_num != 2 || channels == NULL || user_config == NULL) {
        return false;
    }

    dma2d_yuv_context_t *ctx = (dma2d_yuv_context_t *)user_config;
    uint32_t tx_index = 0;
    uint32_t rx_index = 1;
    if (channels[0].dir == DMA2D_CHANNEL_DIRECTION_RX) {
        tx_index = 1;
        rx_index = 0;
    }

    dma2d_channel_handle_t tx_chan = channels[tx_index].chan;
    dma2d_channel_handle_t rx_chan = channels[rx_index].chan;
    dma2d_trigger_t trigger = {
        .periph = DMA2D_TRIG_PERIPH_M2M,
        .periph_sel_id = SOC_DMA2D_TRIG_PERIPH_M2M_TX,
    };
    dma2d_connect(tx_chan, &trigger);
    trigger.periph_sel_id = SOC_DMA2D_TRIG_PERIPH_M2M_RX;
    dma2d_connect(rx_chan, &trigger);

    dma2d_set_transfer_ability(tx_chan, &ctx->transfer_ability);
    dma2d_set_transfer_ability(rx_chan, &ctx->transfer_ability);
    dma2d_configure_color_space_conversion(rx_chan, &ctx->rx_csc);

    dma2d_rx_event_callbacks_t callbacks = {
        .on_recv_eof = dma2d_yuv_done_cb,
    };
    dma2d_register_rx_event_callbacks(rx_chan, &callbacks, ctx);
    dma2d_set_desc_addr(tx_chan, (intptr_t)ctx->tx_desc);
    dma2d_set_desc_addr(rx_chan, (intptr_t)ctx->rx_desc);
    dma2d_start(tx_chan);
    dma2d_start(rx_chan);
    return false;
}

esp_err_t dma2d_yuv_converter_init(void)
{
    if (s_dma2d_yuv.initialized) {
        return ESP_OK;
    }

    /* ESP-IDF disables this CSC combination on P4 silicon before v3.0.
     * Do not enqueue a private DMA2D transaction on those revisions: the
     * hardware does not generate RX EOF and the caller would wait to timeout
     * on every frame. */
    esp_chip_info_t chip_info = { 0 };
    esp_chip_info(&chip_info);
    if (chip_info.model == CHIP_ESP32P4 && chip_info.revision < 300) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    memset(&s_dma2d_yuv, 0, sizeof(s_dma2d_yuv));
    s_dma2d_yuv.done = xSemaphoreCreateBinaryStatic(&s_dma2d_yuv.done_storage);
    if (s_dma2d_yuv.done == NULL) {
        return ESP_ERR_NO_MEM;
    }

    const dma2d_pool_config_t pool_config = {
        .pool_id = DMA2D_YUV_POOL_ID,
    };
    esp_err_t ret = dma2d_acquire_pool(&pool_config, &s_dma2d_yuv.pool);
    if (ret != ESP_OK) {
        goto fail;
    }

    s_dma2d_yuv.tx_desc = heap_caps_aligned_calloc(
        DMA2D_YUV_CACHE_ALIGN, 1, DMA2D_YUV_CACHE_ALIGN,
        MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    s_dma2d_yuv.rx_desc = heap_caps_aligned_calloc(
        DMA2D_YUV_CACHE_ALIGN, 1, DMA2D_YUV_CACHE_ALIGN,
        MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    s_dma2d_yuv.trans_placeholder = heap_caps_calloc(
        1, SIZEOF_DMA2D_TRANS_T, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (s_dma2d_yuv.tx_desc == NULL || s_dma2d_yuv.rx_desc == NULL ||
        s_dma2d_yuv.trans_placeholder == NULL) {
        ret = ESP_ERR_NO_MEM;
        goto fail;
    }

    s_dma2d_yuv.transfer_ability = (dma2d_transfer_ability_t) {
        .access_ext_mem = true,
        .data_burst_length = 128,
        .desc_burst_en = true,
        .mb_size = DMA2D_MACRO_BLOCK_SIZE_8_16,
    };
    s_dma2d_yuv.rx_csc = (dma2d_csc_config_t) {
        .rx_csc_option = DMA2D_CSC_RX_YUV422_TO_YUV420,
        .pre_scramble = DMA2D_SCRAMBLE_ORDER_BYTE2_1_0,
        .post_scramble = DMA2D_SCRAMBLE_ORDER_BYTE2_1_0,
    };
    s_dma2d_yuv.trans_config = (dma2d_trans_config_t) {
        .tx_channel_num = 1,
        .rx_channel_num = 1,
        .channel_flags = DMA2D_CHANNEL_FUNCTION_FLAG_SIBLING |
                         DMA2D_CHANNEL_FUNCTION_FLAG_RX_CSC,
        .specified_tx_channel_mask = 0,
        .specified_rx_channel_mask = 0,
        .on_job_picked = dma2d_yuv_on_picked,
        .user_config = &s_dma2d_yuv,
    };
    s_dma2d_yuv.initialized = true;
    return ESP_OK;

fail:
    dma2d_yuv_converter_deinit();
    return ret;
}

esp_err_t dma2d_yuv422_to_h264_yuv420(const uint8_t *src,
                                      size_t src_size,
                                      uint8_t *dst,
                                      size_t dst_size,
                                      uint32_t width,
                                      uint32_t height,
                                      uint32_t timeout_ms)
{
    if (!s_dma2d_yuv.initialized || src == NULL || dst == NULL ||
        width == 0 || height == 0 || (width % DMA2D_YUV_BLOCK_WIDTH) != 0 ||
        (height % DMA2D_YUV_BLOCK_HEIGHT) != 0 ||
        src_size < (size_t)width * height * 2 ||
        dst_size < (size_t)width * height * 3 / 2 ||
        s_dma2d_yuv.in_flight) {
        return ESP_ERR_INVALID_ARG;
    }

    dma2d_yuv_desc_init(s_dma2d_yuv.tx_desc, (uint8_t *)src, width, height,
                        1, DMA2D_DESCRIPTOR_PBYTE_2B0_PER_PIXEL);
    dma2d_yuv_desc_init(s_dma2d_yuv.rx_desc, dst, width, height,
                        1, DMA2D_DESCRIPTOR_PBYTE_1B5_PER_PIXEL);
    esp_err_t ret = esp_cache_msync((void *)src, src_size,
                                    ESP_CACHE_MSYNC_FLAG_DIR_C2M |
                                    ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = esp_cache_msync(dst, dst_size,
                          ESP_CACHE_MSYNC_FLAG_DIR_C2M |
                          ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = esp_cache_msync(s_dma2d_yuv.tx_desc, DMA2D_YUV_CACHE_ALIGN,
                          ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = esp_cache_msync(s_dma2d_yuv.rx_desc, DMA2D_YUV_CACHE_ALIGN,
                          ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    if (ret != ESP_OK) {
        return ret;
    }

    while (xSemaphoreTake(s_dma2d_yuv.done, 0) == pdTRUE) {
    }
    s_dma2d_yuv.in_flight = true;
    ret = dma2d_enqueue(s_dma2d_yuv.pool, &s_dma2d_yuv.trans_config,
                        s_dma2d_yuv.trans_placeholder);
    if (ret != ESP_OK) {
        s_dma2d_yuv.in_flight = false;
        return ret;
    }

    const uint32_t wait_ms = timeout_ms == 0 ? DMA2D_YUV_DMA_TIMEOUT_MS : timeout_ms;
    if (xSemaphoreTake(s_dma2d_yuv.done, pdMS_TO_TICKS(wait_ms)) != pdTRUE) {
        bool need_yield = false;
        dma2d_force_end(s_dma2d_yuv.trans_placeholder, &need_yield);
        s_dma2d_yuv.in_flight = false;
        return ESP_ERR_TIMEOUT;
    }

    return esp_cache_msync(dst, dst_size, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
}

void dma2d_yuv_converter_deinit(void)
{
    if (s_dma2d_yuv.in_flight) {
        return;
    }
    if (s_dma2d_yuv.pool != NULL) {
        dma2d_release_pool(s_dma2d_yuv.pool);
    }
    free(s_dma2d_yuv.tx_desc);
    free(s_dma2d_yuv.rx_desc);
    free(s_dma2d_yuv.trans_placeholder);
    memset(&s_dma2d_yuv, 0, sizeof(s_dma2d_yuv));
}
