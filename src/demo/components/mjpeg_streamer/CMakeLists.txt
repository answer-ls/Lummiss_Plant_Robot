idf_component_register(
    SRCS "mjpeg_streamer.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_websocket_client
        esp_timer
        network)
