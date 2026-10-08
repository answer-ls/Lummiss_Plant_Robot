idf_component_register(SRCS "main.c" "es7210.c" INCLUDE_DIRS "."
    REQUIRES esp_driver_i2c esp_driver_i2s esp_driver_gpio esp_driver_uart esp_psram esp_timer mbedtls)
