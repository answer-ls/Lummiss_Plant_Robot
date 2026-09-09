cmake_minimum_required(VERSION 3.16)

# 只加载正式主组件及它声明的依赖，避免引入开发板原大屏 BSP。
set(COMPONENTS main)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(lummiss_main)
