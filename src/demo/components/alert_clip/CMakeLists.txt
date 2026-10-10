idf_component_register(
    SRCS "alert_clip.c"
    INCLUDE_DIRS "."
    REQUIRES esp_http_client esp_timer json mbedtls sd_card)
