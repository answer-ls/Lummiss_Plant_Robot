cmake_minimum_required(VERSION 3.16)

# esp_wifi_remote 的 Kconfig 按 ESP_IDF_VERSION 加载对应版本配置。
# IDF 5.5.5 使用组件提供的 5.5 配置，否则 C6/SDIO 选项会被跳过。
set(ENV{ESP_IDF_VERSION} "5.5")

# 只加载正式主组件及它声明的依赖，避免引入开发板原大屏 BSP。
set(COMPONENTS main)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(lummiss_main)

# ESP-SR 默认把语音模型写入名为 model 的分区；本板该分区留给视觉模型，
# 唤醒词运行时从 storage 读取。仅修正语音镜像的烧录映射，不改组件源码。
partition_table_get_partition_info(sr_model_offset "--partition-name model" "offset")
partition_table_get_partition_info(sr_storage_offset "--partition-name storage" "offset")
foreach(prop IN ITEMS IMAGES FLASH_FILE FLASH_ENTRY)
    get_target_property(entries flash ${prop})
    set(corrected_entries "")
    foreach(entry IN LISTS entries)
        if(entry MATCHES "srmodels/srmodels.bin")
            string(REPLACE "${sr_model_offset}" "${sr_storage_offset}" entry "${entry}")
            if(prop STREQUAL "FLASH_ENTRY")
                string(REPLACE "\"model\" :" "\"storage\" :" entry "${entry}")
            endif()
        endif()
        list(APPEND corrected_entries "${entry}")
    endforeach()
    set_property(TARGET flash PROPERTY ${prop} "${corrected_entries}")
endforeach()

if(CONFIG_ESP_HOSTED_PKT_STATS)
    # 当前 ESP-Hosted 组件的统计间隔依赖旧 Kconfig 名称，周期定时器常量也使用旧名称。
    # 仅为该组件补齐定义，不改变数据发送路径。
    idf_component_get_property(hosted_lib espressif__esp_hosted COMPONENT_LIB)
    target_compile_definitions(${hosted_lib} PRIVATE
        CONFIG_ESP_PKT_STATS_INTERVAL_SEC=5
        HOSTED_TIMER_PERIODIC=H_TIMER_TYPE_PERIODIC)
endif()
