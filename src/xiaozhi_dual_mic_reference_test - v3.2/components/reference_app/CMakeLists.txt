idf_component_register(SRCS "XiaozhiApp.cpp" "XiaozhiAudioProcessor.cpp" "XiaozhiClient.cpp"
 "XiaozhiActivationClient.cpp" "XiaozhiActivationPrompt.cpp" "XiaozhiDeveloperTools.cpp"
 "XiaozhiLocalization.cpp" "third_party/xiaozhi_esp32/ogg_demuxer.cc"
 INCLUDE_DIRS "." REQUIRES board_audio esp_audio_codec esp_audio_effects esp-sr esp_xiaozhi
 esp_event esp_netif nvs_flash esp_timer mcp-c-sdk esp_http_client cjson
 esp_app_format esp_hw_support efuse esp_partition app_update spi_flash mbedtls)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-missing-field-initializers)
