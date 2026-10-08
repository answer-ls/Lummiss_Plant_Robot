cmake_minimum_required(VERSION 3.16)

# 复用当前工程的 SDSPI 诊断覆盖层，确保 A/B 使用同一底层驱动。
set(EXTRA_COMPONENT_DIRS "../demo/components/esp_driver_sdspi")
option(SD_RAW_WITH_HOSTED "在最小工程中启用 ESP-Hosted，用于单变量 A/B" OFF)
if(SD_RAW_WITH_HOSTED)
    list(APPEND EXTRA_COMPONENT_DIRS "../demo/managed_components/espressif__esp_hosted")
    list(APPEND EXTRA_COMPONENT_DIRS "../demo/managed_components/espressif__esp_wifi_remote")
    list(APPEND EXTRA_COMPONENT_DIRS "hosted_compat")
    set(SDKCONFIG_DEFAULTS "sdkconfig.defaults;sdkconfig.hosted.defaults")
    # IDF 最小构建的快速依赖扫描不会展开 main/CMakeLists.txt 中的条件分支。
    set(COMPONENTS main espressif__esp_hosted espressif__esp_wifi_remote hosted_compat)
endif()
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
project(sd_raw_minimal)
