cmake_minimum_required(VERSION 3.16)
# WiFi Remote 1.3 使用主次版本选择 Kconfig，5.5.5 对应其 5.5 接口。
set(ENV{ESP_IDF_VERSION} "5.5")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
project(xiaozhi_dual_mic_reference_test)
spiffs_create_partition_image(storage spiffs FLASH_IN_PROJECT)
