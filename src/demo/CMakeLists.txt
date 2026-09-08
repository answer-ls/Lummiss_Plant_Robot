cmake_minimum_required(VERSION 3.16)

# 允许通过 -DLUMMISS_TEST=screen/camera 选择硬件测试入口。
set(LUMMISS_TEST "screen" CACHE STRING "Lummiss hardware test: screen or camera")
set_property(CACHE LUMMISS_TEST PROPERTY STRINGS screen camera)

if(NOT LUMMISS_TEST STREQUAL "screen" AND NOT LUMMISS_TEST STREQUAL "camera")
    message(FATAL_ERROR "LUMMISS_TEST must be 'screen' or 'camera'")
endif()

# 仅加载主程序及它声明的依赖，避免引入开发板原大屏 BSP。
set(COMPONENTS main)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(lummiss_hardware_test)
