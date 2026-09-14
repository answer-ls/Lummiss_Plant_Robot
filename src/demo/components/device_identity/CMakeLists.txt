idf_component_register(
    SRCS "device_identity.c"
    INCLUDE_DIRS "."
    REQUIRES nvs_flash freertos)