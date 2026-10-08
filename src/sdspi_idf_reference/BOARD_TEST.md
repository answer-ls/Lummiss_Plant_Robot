# ESP32-P4 新 PCB：官方 SDSPI 示例文件读写对照

本目录复制自 ESP-IDF v5.5.5 的 `examples/storage/sd_card/sdspi`。原版入口保留在 `main/sd_card_example_main.c`，实际构建入口是 `main/board_sdspi_test.c`：使用官方 `SDSPI_HOST_DEFAULT()`、原生 ESP-IDF SDSPI 驱动与 `esp_vfs_fat_sdspi_mount()`，只改为本板 GPIO42/43/41/44、GPIO7 保持高、GPIO45 控制 TF，并限制为 1 MHz。不会自动格式化。成功挂载后创建一个未占用的 `codex_spi_XX.bin`，写入 16 KB、关闭文件、重新打开并逐字节比较；成功后保留测试文件供电脑核对。

在乐鑫组件库下载的 P4 `display_sdcard` 原始示例保存在 `../esp32p4_registry_sdcard_example/display_sdcard`。该示例默认使用 SDMMC、官方开发板 BSP 和显示屏，不适合作为本定制 PCB 的直接 SPI 对照；这里选用与本机 ESP-IDF 版本完全一致的官方 SDSPI 示例。

本机已配置 16 MB 程序 Flash、ESP32-P4 目标和本板引脚。进入下载模式后，在本目录构建及烧录；如自动复位不能进入下载模式，可在 `build` 目录用现有的 `--before no_reset` 烧录命令及 `@flash_args`。监视时确保 ELF 指向本工程的 `build/sd_card.elf`。

预期日志：`SOURCE=...`、`PWR_IO`、`SPI2`、`MOUNT`、卡信息、`READ_REPEAT`、`WRITE_START`、`WRITE_CLOSE`、`READBACK` 和 `RESULT`。只有 `MOUNT=ESP_OK`、`WRITE_CLOSE result=0`、`READBACK result=ESP_OK`，才算完整文件写入与读回通过。若卡仍被识别为无效 FAT 卷，测试会在挂载失败后停止，绝不格式化。

挂载失败时会额外执行一次只读 `RAW_DIAG`：使用同一套原生 SDSPI 驱动重新读取卡的 CSD 容量及 LBA0 前 64 字节、后 16 字节，不执行写入。重点将 `RAW_DIAG csd_ver/sectors` 与主程序成功挂载时的 `CSD: ver=1, capacity=245760` 对比；如果仍是 16384 扇区并且 LBA0 不是有效分区表，问题发生在 FATFS 挂载之前。

2026-09-23 对照日志确认原生驱动识别为 16384 扇区，LBA0 以 `Chipsbank CBM3082T` 开头，无 MBR 签名；主程序同一卡识别为 245760 扇区并读取 FAT32。新版只读日志增加 `PROBE CMD52/CMD0/CMD8/CMD5/CMD58/CMD9` 结果和 `is_mem/is_sdio`，用于定位识别分叉。此时尚不能判定容量差异的原因，不能格式化卡。

后续日志发现原生驱动将 `CMD5 response0=0xffffffff` 返回为 `ESP_OK`，并识别为 `is_mem=1 is_sdio=1`；主程序的 CMD5 路径则返回不支持。当前 A/B 固件打印 `cmd5_ff_ab=1`，只在 CMD5 返回全 FF 时覆盖为 `ESP_ERR_NOT_SUPPORTED`，用于验证误识别是否导致 8 MiB 和挂载失败，不改变 SPI 事务。若仍识别为 8 MiB，则继续排查初始化时序，不能把这个覆盖当作最终修复。

CMD5 全 FF 覆盖实验结果：`is_sdio` 从 1 变 0，但仍识别 16384 扇区、LBA0 仍为 Chipsbank 字串，挂载失败。因此它不是容量差异的充分解释。下一轮固件打印 `cmd5_ff_ab=0 tx_delay_1tick_ab=1`，取消覆盖，仅复现主程序每条 SD 命令结束后的 `vTaskDelay(1)`；如果容量恢复，继续缩小到具体初始化命令之间的等待点。

第一次 1 tick 实验仍失败，但主程序 `CONFIG_FREERTOS_HZ=1000`，对照工程当时为 100，即实际等待分别约 1 ms 与 10 ms。现将对照工程改为 1000 Hz，并在启动行打印 `tick_hz`，重新验证相同的 1 tick 时序。
