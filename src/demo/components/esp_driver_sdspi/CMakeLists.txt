# Temporary, project-local diagnostic overlay for ESP-IDF v5.5.5 SDSPI.
# Only sdspi_host.c is copied; headers and the other two translation units
# still come from the exact IDF tree selected by IDF_PATH.
set(sdspi_upstream "${IDF_PATH}/components/esp_driver_sdspi")
idf_component_register(
    SRCS "src/sdspi_host.c"
         "${sdspi_upstream}/src/sdspi_crc.c"
         "${sdspi_upstream}/src/sdspi_transaction.c"
    INCLUDE_DIRS "${sdspi_upstream}/include"
    PRIV_INCLUDE_DIRS "${sdspi_upstream}/src"
    REQUIRES sdmmc esp_driver_spi esp_driver_gpio
    PRIV_REQUIRES esp_timer esp_mm)
