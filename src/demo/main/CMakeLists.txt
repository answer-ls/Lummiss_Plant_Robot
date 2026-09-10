idf_component_register(
    SRCS
        "main.c"
        "display_driver.c"
        "camera_driver.c"
        "gif_assets.c"
    INCLUDE_DIRS "."
    # 基础工程同时启用屏幕和 USB 摄像头，后续模块继续在此依赖集合上扩展。
    REQUIRES video_streamer network home_info usb esp_timer usb_host_uvc esp_lcd esp_driver_spi esp_lvgl_port lvgl__lvgl)
