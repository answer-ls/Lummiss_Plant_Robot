idf_component_register(
    SRCS "ota_client.c"
    INCLUDE_DIRS "."
    REQUIRES esp_http_client mbedtls freertos device_identity)