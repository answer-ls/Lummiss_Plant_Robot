#ifndef LUMMISS_SD_CARD_H
#define LUMMISS_SD_CARD_H

#include <stdbool.h>

#include "esp_err.h"

/*
 * TF 卡（SDMMC Slot 0）挂载。
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

/* 卸载 TF 卡。会保留 SDMMC 主控（Slot 1 的 ESP-Hosted 仍在使用）。 */
esp_err_t sd_card_unmount(void);

bool sd_card_is_mounted(void);

#endif /* LUMMISS_SD_CARD_H */
