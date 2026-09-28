idf_component_register(
    SRCS "provisioning_manager.c"
    INCLUDE_DIRS "."
    REQUIRES
        device_identity
        esp_event
        esp_wifi
        espressif__esp_hosted
        nvs_flash
        protocomm
        espressif__network_provisioning)
