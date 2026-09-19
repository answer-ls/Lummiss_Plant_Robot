idf_component_register(
    SRCS "cloud_mqtt.c" "cloud_mcp.c"
    INCLUDE_DIRS "."
    # stepper_motor / ambient_led 是 MCP 工具的实际执行者。
    # mcp_registry 是工具/能力登记处（tools/list、tools/call、
    # capability_manifest 三方同源）。不再依赖 ota_client（清单宏已废止）。
    REQUIRES mqtt json esp_timer mcp_registry stepper_motor ambient_led)
