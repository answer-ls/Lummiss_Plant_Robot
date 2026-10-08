set(srcs "board_sdspi_test.c")

idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS "."
                       REQUIRES fatfs sdmmc esp_driver_gpio esp_driver_spi esp_driver_sdspi esp_timer esp_system)
