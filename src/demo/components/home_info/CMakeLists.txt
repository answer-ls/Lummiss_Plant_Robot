idf_component_register(
    SRCS "home_info.c"
    INCLUDE_DIRS "."
    REQUIRES network esp_http_client esp_netif mbedtls json freertos)
