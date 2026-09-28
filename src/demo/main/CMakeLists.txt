idf_component_register(
    SRCS
        "main.c"
        "display_driver.c"
        "lv_font_lummiss_weather_16.c"
        "camera_driver.c"
        "screen_carousel.c"
        "expression_manager.c"
        "peripheral_test.c"
    INCLUDE_DIRS "."
    # 基础工程同时启用屏幕和 USB 摄像头，后续模块继续在此依赖集合上扩展。
    # sd_card 供 screen_carousel 读取 TF 卡表情资源；xiaozhi_audio 管理板载音频和小智会话。
    # mech_button/touch_key/ambient_led 是三组人机交互外设驱动，只在档位 9 由 peripheral_test 使用。
    # 注意：机械按键组件必须叫 mech_button，不能叫 button —— espressif__esp_lvgl_port
    # 会用 "button" 这个名字去探测 espressif/button(iot_button)，重名会把它的
    # esp_lvgl_port_button.c 拉进编译并链到我们这条库上，导致整片未定义符号。
    REQUIRES board stepper_motor video_streamer webrtc_whip person_detect camera_photo network home_info sd_card anim_bin_player xiaozhi_audio usb esp_timer usb_host_uvc esp_lcd esp_driver_spi esp_lvgl_port lvgl__lvgl device_identity ota_client mech_button touch_key ambient_led cloud_mqtt cloud_udp esp_netif json mcp_registry)
