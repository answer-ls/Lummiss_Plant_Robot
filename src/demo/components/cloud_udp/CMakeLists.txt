idf_component_register(
    SRCS "cloud_udp.c"
    INCLUDE_DIRS "."
    # lwip 提供 socket / inet_pton；mbedtls 提供 AES-128-CTR；
    # cloud_mqtt 提供 cloud_mqtt_session_t（UDP 参数就是 MQTT hello 协商出来的）。
    REQUIRES lwip mbedtls esp_timer cloud_mqtt)
