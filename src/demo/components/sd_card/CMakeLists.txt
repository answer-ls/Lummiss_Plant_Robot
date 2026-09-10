idf_component_register(
    SRCS "sd_card.c"
    INCLUDE_DIRS "."
    # sdmmc 走 IOMUX + 片内 LDO 供电；fatfs 提供 esp_vfs_fat_sdmmc_mount。
    REQUIRES esp_driver_sdmmc fatfs sdmmc)
