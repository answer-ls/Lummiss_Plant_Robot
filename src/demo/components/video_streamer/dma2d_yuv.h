#ifndef LUMMISS_DMA2D_YUV_H
#define LUMMISS_DMA2D_YUV_H

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Initialize the P4 DMA2D M2M converter.  The converter uses the private
 * ESP-IDF DMA2D API because the public JPEG decoder API rejects YUV422 to
 * YUV420 on P4 revisions below 3.0, even though the hardware CSC exists.
 */
esp_err_t dma2d_yuv_converter_init(void);

/* Convert packed JPEG YUV422 (U Y0 V Y1) to H.264 O_UYY_E_VYY YUV420. */
esp_err_t dma2d_yuv422_to_h264_yuv420(const uint8_t *src,
                                      size_t src_size,
                                      uint8_t *dst,
                                      size_t dst_size,
                                      uint32_t width,
                                      uint32_t height,
                                      uint32_t timeout_ms);

void dma2d_yuv_converter_deinit(void);

#ifdef __cplusplus
}
#endif

#endif /* LUMMISS_DMA2D_YUV_H */
