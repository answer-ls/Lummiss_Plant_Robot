idf_component_register(
    SRCS "ota_client.c"
    INCLUDE_DIRS "."
    # esp_app_format 提供 esp_app_desc.h（esp_app_get_elf_sha256），
    # 它不在 IDF 的公共依赖集合里，必须显式列出，否则报头文件找不到。
    # mcp_registry 提供 capability_manifest 的运行时生成接口。
    REQUIRES json esp_http_client mbedtls freertos device_identity esp_app_format mcp_registry)