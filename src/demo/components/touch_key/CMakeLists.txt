idf_component_register(
    SRCS "touch_key.c"
    INCLUDE_DIRS "."
    REQUIRES board esp_driver_gpio esp_timer)
