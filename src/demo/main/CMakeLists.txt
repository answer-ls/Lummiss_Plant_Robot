if(LUMMISS_TEST STREQUAL "camera")
    set(LUMMISS_TEST_SOURCE "camera_test.c")
    message(STATUS "Lummiss test mode: USB camera")
else()
    set(LUMMISS_TEST_SOURCE "screen_test.c")
    message(STATUS "Lummiss test mode: ST7789 screen")
endif()

idf_component_register(
    SRCS "${LUMMISS_TEST_SOURCE}"
    INCLUDE_DIRS "."
    # ESP-IDF 会在依赖展开阶段多次读取该文件，此时顶层缓存变量可能尚未稳定。
    # 两种测试统一声明依赖，可确保首次扫描后仍能找到当前测试所需的头文件。
    REQUIRES usb esp_timer usb_host_uvc esp_lcd esp_driver_spi esp_lvgl_port lvgl__lvgl)
