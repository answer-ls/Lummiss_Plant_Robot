idf_component_register(
    SRCS "mcp_registry.c"
    INCLUDE_DIRS "."
    # cJSON 只用于 handler 签名里的参数对象类型；清单生成是纯字符串拼接。
    REQUIRES json)
