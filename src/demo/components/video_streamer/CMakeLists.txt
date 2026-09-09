idf_component_register(
    SRCS "video_streamer.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_http_client
        esp_driver_jpeg
        esp_timer
        esp_h264
        network)
