idf_component_register(
    SRCS
        "main.c"
        "display_driver.c"
        "lv_font_lummiss_weather_16.c"
        "camera_driver.c"
        "screen_carousel.c"
        "expression_manager.c"
    INCLUDE_DIRS "."
    # 基础工程同时启用屏幕和 USB 摄像头，后续模块继续在此依赖集合上扩展。
    # sd_card 供 screen_carousel 读取 TF 卡表情资源；xiaozhi_audio 管理板载音频和小智会话。
    REQUIRES board stepper_motor video_streamer network home_info sd_card anim_bin_player xiaozhi_audio usb esp_timer usb_host_uvc esp_lcd esp_driver_spi esp_lvgl_port lvgl__lvgl device_identity ota_client)
