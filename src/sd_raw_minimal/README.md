# 独立 SDSPI Raw CMD24 诊断

默认构建只初始化 GPIO45 供电控制、SPI2 和 SD 卡；不包含 ESP-Hosted、Wi-Fi、LCD、摄像头或 FATFS。复用 `../demo/components/esp_driver_sdspi` 的诊断驱动，便于与主工程档位 10 对照。

在 ESP-IDF v5.5.5 环境中，于本目录执行 `idf.py -B build build`，随后按实际串口烧录和监视。该工程自动只读扫描 FAT32 的所有 FAT 副本，选取一致标为空闲的连续 16 个数据扇区，再执行固定 LBA 重复写和连续 LBA 写；不会改写已有文件或文件系统元数据。写入会覆盖所选空闲扇区原有的残留数据。

若要单独对照 ESP-Hosted，先在同一 ESP-IDF 环境中设置 `$env:IDF_COMPONENT_MANAGER='0'`，然后执行 `idf.py -B build_hosted -D SD_RAW_WITH_HOSTED=ON -D SDKCONFIG=sdkconfig.hosted build`。此构建复用主工程的 ESP-Hosted 2.7.4 / Wi-Fi Remote 组件，`sdkconfig.hosted.defaults` 选择 C6 SDIO 四位总线及同一引脚；`hosted_compat` 补齐 IDF 5.5.5 未命中 Wi-Fi Remote 5.5 Kconfig 入口的问题。两个构建目录必须使用各自独立的 sdkconfig。

两组固件均已在本机编译通过：默认产物为 `build/sd_raw_minimal.bin`，Hosted 产物为 `build_hosted_v2/sd_raw_minimal.bin`。分别冷启动测试，对比 `MINIMAL_BOOT`、`H_API`、`H_SDIO_DRV`、`RAW_WRITE`、`RAW_READBACK`、`BEFORE_CMD24` 和 `CMD24_BUSY_OK`。默认构建必须看不到 `H_API` 与 `H_SDIO_DRV`；Hosted 构建只有实际出现这两类启动日志后，才算 Hosted-on 对照。当前尚未在板上运行，不能据编译结果判断 CMD24 是否恢复。
