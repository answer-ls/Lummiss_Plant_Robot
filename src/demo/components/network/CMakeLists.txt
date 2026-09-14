idf_component_register(
    SRCS
        "network_manager.c"
        "wifi_manager.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_event
        esp_netif
        esp_timer
        esp_wifi
        esp_wifi_remote
        esp_hosted
        provisioning
        nvs_flash)
