idf_component_register(
    SRCS "sd_card.c" "sd_card_spi.c" "sd_card_test.c"
    INCLUDE_DIRS "."
    # sdmmc 走 IOMUX + 片内 LDO 供电；fatfs 提供 esp_vfs_fat_sdmmc_mount。
    REQUIRES board esp_timer esp_driver_gpio esp_driver_spi esp_driver_sdspi esp_driver_sdmmc fatfs sdmmc)
