cmake_minimum_required(VERSION 3.16)

# esp_wifi_remote 的 Kconfig 按 ESP_IDF_VERSION 加载对应版本配置。
# IDF 5.5.5 使用组件提供的 5.5 配置，否则 C6/SDIO 选项会被跳过。
set(ENV{ESP_IDF_VERSION} "5.5")

# 只加载正式主组件及它声明的依赖，避免引入开发板原大屏 BSP。
set(COMPONENTS main)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(lummiss_main)

if(CONFIG_ESP_HOSTED_PKT_STATS)
    # 当前 ESP-Hosted 组件的统计间隔依赖旧 Kconfig 名称，周期定时器常量也使用旧名称。
    # 仅为该组件补齐定义，不改变数据发送路径。
    idf_component_get_property(hosted_lib espressif__esp_hosted COMPONENT_LIB)
    target_compile_definitions(${hosted_lib} PRIVATE
        CONFIG_ESP_PKT_STATS_INTERVAL_SEC=5
        HOSTED_TIMER_PERIODIC=H_TIMER_TYPE_PERIODIC)
endif()
