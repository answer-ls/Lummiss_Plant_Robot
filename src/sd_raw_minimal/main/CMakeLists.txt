set(extra_requires "")
if(SD_RAW_WITH_HOSTED)
    list(APPEND extra_requires espressif__esp_hosted espressif__esp_wifi_remote)
endif()
idf_component_register(SRCS "main.c"
    REQUIRES esp_driver_gpio esp_driver_spi esp_driver_sdspi sdmmc esp_timer ${extra_requires})
target_compile_definitions(${COMPONENT_LIB} PRIVATE SD_RAW_WITH_HOSTED=$<BOOL:${SD_RAW_WITH_HOSTED}>)
