idf_component_register(
    SRCS "xiaozhi_audio.c" "wake_word.c"
    INCLUDE_DIRS "."
    REQUIRES
        board
        video_streamer
        # V3 协议下音频的控制面在 MQTT、传输面在 UDP（旧协议下不引用，
        # 但 REQUIRES 是静态的，一起链上无副作用）。
        cloud_mqtt
        cloud_udp
        ambient_led
        esp_audio_codec
        esp_codec_dev
        esp_driver_gpio
        esp_driver_i2c
        esp_driver_i2s
        esp-sr
        freertos)
