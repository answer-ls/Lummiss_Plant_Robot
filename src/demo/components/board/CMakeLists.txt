idf_component_register(
    SRCS "board_init.c" "mem_fragment.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_gpio esp_timer)
