idf_component_register(
    SRCS "xiaozhi_audio.c"
    INCLUDE_DIRS "."
    REQUIRES
        video_streamer
        esp_audio_codec
        esp_codec_dev
        esp_driver_gpio
        esp_driver_i2c
        esp_driver_i2s
        freertos)
