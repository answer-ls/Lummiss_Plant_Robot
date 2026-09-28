idf_component_register(
    SRCS "webrtc_whip.c" "rtc_heap_diag.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_peer esp_http_client esp-tls json
    PRIV_REQUIRES board esp_timer esp_netif esp_hw_support network)
