idf_component_register(
    SRCS "video_streamer.c" "dma2d_yuv.c" "jpeg_failure_probe.c" "jpeg_integrity.c"
    INCLUDE_DIRS "."
    REQUIRES
        board
        esp_websocket_client
        espressif__usb_host_uvc
        esp_driver_jpeg
        esp_hw_support
        esp_timer
        esp_h264
        network
        webrtc_whip
        alert_clip
        json)

# YUV422 转 YUV420 是当前流水线中耗时最大的逐像素循环。仅对本组件启用
# 性能优化，保留工程其余组件的调试构建和断言设置。
target_compile_options(${COMPONENT_LIB} PRIVATE -O3)
