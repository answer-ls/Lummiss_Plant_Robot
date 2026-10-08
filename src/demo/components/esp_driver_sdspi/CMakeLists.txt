# 使用当前 IDF 安装目录中的官方 SDSPI 源码，保留本地目录仅用于组件发现。
set(sdspi_upstream "${IDF_PATH}/components/esp_driver_sdspi")
idf_component_register(
    SRCS "${sdspi_upstream}/src/sdspi_crc.c"
         "${sdspi_upstream}/src/sdspi_host.c"
         "${sdspi_upstream}/src/sdspi_transaction.c"
    INCLUDE_DIRS "${sdspi_upstream}/include"
    PRIV_INCLUDE_DIRS "${sdspi_upstream}/src"
    REQUIRES sdmmc esp_driver_spi esp_driver_gpio
    PRIV_REQUIRES esp_timer esp_mm)
