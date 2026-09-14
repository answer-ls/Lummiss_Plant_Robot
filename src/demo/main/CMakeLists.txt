idf_component_register(
    SRCS
        "main.c"
        "display_driver.c"
        "lv_font_lummiss_weather_16.c"
        "camera_driver.c"
        "screen_carousel.c"
    INCLUDE_DIRS "."
    # 基础工程同时启用屏幕和 USB 摄像头，后续模块继续在此依赖集合上扩展。
    # sd_card 供 screen_carousel 从 TF 卡读取 LUM1 BIN，anim_bin_player 负责双缓冲播放。
    REQUIRES video_streamer network home_info sd_card anim_bin_player usb esp_timer usb_host_uvc esp_lcd esp_driver_spi esp_lvgl_port lvgl__lvgl device_identity ota_client)
