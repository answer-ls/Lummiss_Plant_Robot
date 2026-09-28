#ifndef LUMMISS_SD_CARD_H
#define LUMMISS_SD_CARD_H

#include <stdbool.h>

#include "esp_err.h"

/*
 * 按板型挂载TF卡。新PCB使用SPI2：CLK42、CMD/MOSI43、D0/MISO41、D3/CS44；
 * GPIO45低有效供电；GPIO39仅记录卡检测电平，极性待实测。
 * 以下SDMMC Slot 0 / LDO说明仅适用于旧开发板。
 *
 * 开发板 JC1060P470C 的 TF 卡座直接接 ESP32-P4 的 SDMMC Slot 0，4 位总线：
 *   CLK=GPIO43  CMD=GPIO44  D0=GPIO39  D1=GPIO40  D2=GPIO41  D3=GPIO42
 * 这 6 个脚在 Slot 0 上走 IOMUX，软件不能改（见 soc/esp32p4/include/soc/sdmmc_pins.h）。
 *
 * 卡座供电 TF_VCC 由 P4 片内 LDO 第 4 通道提供，所以挂载前必须先注册
 * sd_pwr_ctrl_new_on_chip_ldo()，否则会以 INIT 失败告终。
 *
 * 板载 ESP32-C6 走 SDMMC Slot 1（ESP-Hosted），与 Slot 0 互不冲突。
 */

/* 挂载点固定为 /sdcard，与厂商 BSP 的 BSP_SD_MOUNT_POINT 保持一致。 */
#define SD_CARD_MOUNT_POINT "/sdcard"

/* 挂载 TF 卡。可重复调用：已挂载时直接返回 ESP_OK。
 * 失败时返回底层错误码（无需格式化，本函数不会写卡）。 */
esp_err_t sd_card_mount(void);

/* 卸载TF卡。新PCB释放SPI2并关电；旧板保留仍被C6使用的SDMMC主控。 */
esp_err_t sd_card_unmount(void);

bool sd_card_is_mounted(void);
/* First low-level write error since mounting; retained across unmount for tests. */
esp_err_t sd_card_get_write_error(void);

/* 独立测试：仅创建唯一临时文件，读回校验、卸载重挂后再次校验并删除。 */
esp_err_t sd_card_self_test(void);
/* 对照 ESP-IDF SDSPI 示例：挂载后写短文件、关闭、重新打开并逐字节读回。 */
esp_err_t sd_card_official_style_test(void);
/* 仅用于 SDSPI 诊断：选取空闲数据扇区，直接执行 CMD24/CMD17 测试。 */
esp_err_t sd_card_raw_diagnostic(void);

#endif /* LUMMISS_SD_CARD_H */
