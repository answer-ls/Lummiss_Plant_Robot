cmake_minimum_required(VERSION 3.16)

# esp_wifi_remote 的 Kconfig 按 ESP_IDF_VERSION 加载对应版本配置。
# IDF 5.5.5 使用组件提供的 5.5 配置，否则 C6/SDIO 选项会被跳过。
set(ENV{ESP_IDF_VERSION} "5.5")

# 只加载正式主组件及它声明的依赖，避免引入开发板原大屏 BSP。
set(COMPONENTS main)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(lummiss_main)
