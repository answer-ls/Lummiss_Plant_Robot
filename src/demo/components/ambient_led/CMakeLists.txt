idf_component_register(
    SRCS "ambient_led.c"
    INCLUDE_DIRS "."
    REQUIRES board esp_driver_gpio esp_timer led_strip)
