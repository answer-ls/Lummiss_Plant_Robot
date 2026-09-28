#include "sd_card.h"
#include "board_pins.h"

#if BOARD_USE_NEW_PCB
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/sdspi_host.h"
#include "esp_vfs_fat.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "ff.h"
#include "diskio_sdmmc.h"
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include "sdmmc_cmd.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "SD_CARD";
static sdmmc_card_t *s_card;
static bool s_bus_owned;
static esp_err_t (*s_transaction)(int, sdmmc_command_t *);
static esp_err_t s_write_error;
static uint32_t s_fsinfo_lba = UINT32_MAX;
static uint32_t s_fsinfo_initial_hash;
static bool s_fsinfo_initial_valid;
static bool s_raw_diag_requested;
static bool s_raw_bad_identity_seen;
extern void sdspi_diag_dump_busy_stats(void);
extern void sdspi_diag_set_raw_mode(bool enabled);
extern void sdspi_diag_reset_busy_stats(void);
extern void sdspi_diag_get_busy_stats(uint32_t *count, int64_t *min_us, int64_t *max_us,
                                      uint64_t *total_us, uint32_t bins[7]);
extern void sdspi_diag_get_last_write(uint8_t *r1, bool *r1_seen, uint8_t *token,
                                      bool *token_seen, int64_t *busy_us, uint8_t *busy_last_rx,
                                      esp_err_t *busy_result, const char **failure);

void sd_card_dump_busy_stats(void) { sdspi_diag_dump_busy_stats(); }

static void raw_log_card_identity(const char *stage, const sdmmc_card_t *card)
{
    const uint64_t bytes = (uint64_t)card->csd.capacity * card->csd.sector_size;
    ESP_LOGI(TAG, "%s type=%s CSD_version=%d sector_size=%u capacity_sectors=%" PRIu32
             " capacity_MiB=%" PRIu64 " capacity_MB=%" PRIu64,
             stage, (card->ocr & SD_OCR_SDHC_CAP) ? "SDHC/SDXC" : "SDSC",
             card->csd.csd_ver + 1, (unsigned)card->csd.sector_size,
             (uint32_t)card->csd.capacity, bytes / (1024U * 1024U), bytes / 1000000U);
}

static void diagnose_after_busy_timeout(void)
{
    if (!s_card) return;
    static const uint32_t delays_ms[] = {10, 100, 500, 1000, 5000};
    const int64_t started = esp_timer_get_time();
    for (size_t i = 0; i < sizeof(delays_ms) / sizeof(delays_ms[0]); ++i) {
        int64_t remaining_us = started + (int64_t)delays_ms[i] * 1000 - esp_timer_get_time();
        if (remaining_us > 0) vTaskDelay(pdMS_TO_TICKS((remaining_us + 999) / 1000));
        int64_t actual_ms = (esp_timer_get_time() - started) / 1000;
        sdmmc_command_t status = {
            .opcode = 13, .arg = 0, .flags = SCF_CMD_AC | SCF_RSP_R1, .timeout_ms = 50,
        };
        esp_err_t status_err = s_transaction(s_card->host.slot, &status);
        ESP_LOGI(TAG, "RECOVERY delay_target=%" PRIu32 "ms actual=%" PRId64
                 "ms CMD13=%s valid=%d R1=0x%02" PRIx32 " R2=0x%02" PRIx32,
                 delays_ms[i], actual_ms, esp_err_to_name(status_err), status_err == ESP_OK,
                 status.response[0] & 0xff, (status.response[0] >> 8) & 0xff);
        uint32_t sector0[128] = {0};
        sdmmc_command_t read = {
            .opcode = 17, .arg = 0, .data = sector0, .datalen = sizeof(sector0),
            .buflen = sizeof(sector0), .blklen = 512,
            .flags = SCF_CMD_ADTC | SCF_CMD_READ | SCF_RSP_R1, .timeout_ms = 50,
        };
        esp_err_t read_err = s_transaction(s_card->host.slot, &read);
        ESP_LOGI(TAG, "RECOVERY delay_target=%" PRIu32 "ms CMD17=%s data_valid=%d signature=0x%02x%02x",
                 delays_ms[i], esp_err_to_name(read_err), read_err == ESP_OK,
                 ((uint8_t *)sector0)[511], ((uint8_t *)sector0)[510]);
    }
    // 不切断 TF 供电，复用 ESP-IDF 的完整 SD SPI 初始化流程。
    sdmmc_host_t host = s_card->host;
    ESP_LOGI(TAG, "RECOVERY_SOFT_INIT begin power_cycle=0");
    esp_err_t init_err = sdmmc_card_init(&host, s_card);
    ESP_LOGI(TAG, "RECOVERY_SOFT_INIT result=%s", esp_err_to_name(init_err));
    uint32_t sector0[128] = {0};
    esp_err_t read_err = init_err == ESP_OK ? sdmmc_read_sectors(s_card, sector0, 0, 1) : init_err;
    ESP_LOGI(TAG, "RECOVERY_SOFT_INIT CMD17=%s signature=0x%02x%02x",
             esp_err_to_name(read_err), ((uint8_t *)sector0)[511], ((uint8_t *)sector0)[510]);
}

esp_err_t sd_card_get_write_error(void) { return s_write_error; }

static uint32_t sector_hash(const void *data)
{
    /* 仅用于同一轮诊断比较扇区内容；不作为文件系统校验码。 */
    const uint8_t *bytes = data;
    uint32_t hash = 2166136261U;
    for (size_t i = 0; i < 512; ++i) {
        hash = (hash ^ bytes[i]) * 16777619U;
    }
    return hash;
}

static void check_timed_out_write_data(const sdmmc_command_t *write_cmd, uint32_t lba)
{
    if (!s_card || !write_cmd->data || write_cmd->datalen != 512 ||
        lba >= s_card->csd.capacity) {
        return;
    }

    /* 只在原写事务结束、CS 已释放后回读；不改变 CMD24 的发送和 Busy 时序。
     * 对比本次实际发出的 512 字节，判断超时的写入是否已经落到卡上。 */
    uint32_t readback[128] = {0};
    sdmmc_command_t read_cmd = {
        .opcode = 17,
        .arg = (s_card->ocr & SD_OCR_SDHC_CAP) ? lba : lba * 512U,
        .data = readback, .datalen = sizeof(readback),
        .buflen = sizeof(readback), .blklen = 512,
        .flags = SCF_CMD_ADTC | SCF_CMD_READ | SCF_RSP_R1,
        .timeout_ms = 1000,
    };
    const esp_err_t read_err = s_transaction(s_card->host.slot, &read_cmd);
    const bool match = read_err == ESP_OK &&
                       memcmp(readback, write_cmd->data, sizeof(readback)) == 0;
    ESP_LOGI(TAG, "WRITE_TIMEOUT_READBACK lba=%" PRIu32
             " read=%s R1=0x%02" PRIx32 " match=%d"
             " expected_hash=0x%08" PRIx32 " actual_hash=0x%08" PRIx32,
             lba, esp_err_to_name(read_err), read_cmd.response[0] & 0xff, match,
             sector_hash(write_cmd->data), read_err == ESP_OK ? sector_hash(readback) : 0);
}

/* Observe the official driver's commands without changing their contents. */
static esp_err_t trace_transaction(int slot, sdmmc_command_t *cmd)
{
    bool write_cmd = cmd->opcode == 24 || cmd->opcode == 25;
    uint32_t lba = s_card && (s_card->ocr & SD_OCR_SDHC_CAP) ? cmd->arg : cmd->arg / 512;
    if (write_cmd && s_write_error != ESP_OK) {
        ESP_LOGE(TAG, "WRITE_BLOCKED after first failure: CMD%" PRIu32 " lba=%" PRIu32,
                 cmd->opcode, lba);
        cmd->error = s_write_error;
        return s_write_error;
    }
    if (write_cmd) {
        ESP_LOGI(TAG, "WRITE_CMD begin: CMD%" PRIu32 " arg=0x%08" PRIx32
                 " bytes=%u block=%u timeout=%u ms",
                 cmd->opcode, cmd->arg, (unsigned)cmd->datalen,
                 (unsigned)cmd->blklen, (unsigned)cmd->timeout_ms);
        if (lba == s_fsinfo_lba) ESP_LOGI(TAG, "WRITE_TARGET=FAT32_FSINFO lba=%" PRIu32, lba);
        if (lba == s_fsinfo_lba && cmd->data && cmd->datalen == 512 &&
            s_fsinfo_initial_valid) {
            /* 只比较内存中已读取的 FSInfo，不增加写前 SPI 事务。 */
            ESP_LOGI(TAG, "FSINFO_WRITE_COMPARE initial_hash=0x%08" PRIx32
                     " outgoing_hash=0x%08" PRIx32 " changed=%d",
                     s_fsinfo_initial_hash, sector_hash(cmd->data),
                     s_fsinfo_initial_hash != sector_hash(cmd->data));
        }
    }
    int64_t started = esp_timer_get_time();
    esp_err_t err = s_transaction(slot, cmd);
    if (write_cmd && (err != ESP_OK || cmd->error != ESP_OK)) {
        s_write_error = err != ESP_OK ? err : cmd->error;
        ESP_LOGE(TAG, "FIRST_WRITE_ERROR lba=%" PRIu32 " kind=%s error=%s",
                 lba, lba == s_fsinfo_lba ? "FAT32_FSINFO" : "OTHER",
                 esp_err_to_name(s_write_error));
        if (s_write_error == ESP_ERR_TIMEOUT && cmd->opcode == 24) {
            check_timed_out_write_data(cmd, lba);
            diagnose_after_busy_timeout();
        }
    }
    if (write_cmd || err != ESP_OK || cmd->error != ESP_OK) {
        ESP_LOGW(TAG, "CMD%" PRIu32 " end: ret=%s cmd_error=%s elapsed=%" PRId64
                 " ms response0=0x%08" PRIx32,
                 cmd->opcode, esp_err_to_name(err), esp_err_to_name(cmd->error),
                 (esp_timer_get_time() - started) / 1000, cmd->response[0]);
    }
    /* Each transaction has released the SPI bus before returning here. */
    vTaskDelay(1);
    return err;
}

static uint32_t read_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Inspect the mounted volume rather than guessing that partition 1 was used.
 * Open a directory only; do not scan free space or change FSInfo flags. */
static esp_err_t inspect_filesystem(void)
{
    BYTE drive = ff_diskio_get_pdrv_card(s_card);
    if (drive == 0xff) return ESP_ERR_INVALID_STATE;
    char root[12];
    snprintf(root, sizeof(root), "%u:/", (unsigned)drive);
    FF_DIR dir = {0};
    FRESULT fr = f_opendir(&dir, root);
    if (fr != FR_OK) {
        ESP_LOGE(TAG, "FS_LAYOUT open root failed: %d", fr);
        return ESP_FAIL;
    }
    FATFS *fs = dir.obj.fs;
    unsigned type = fs->fs_type;
    uint32_t base = (uint32_t)fs->volbase;
    ESP_LOGI(TAG, "FS_LAYOUT type=%s volume_lba=%" PRIu32
             " fat_lba=%" PRIu32 " data_lba=%" PRIu32 " cluster_sectors=%u fsi_flag=0x%02x",
             type == FS_FAT32 ? "FAT32" : type == FS_FAT16 ? "FAT16" : type == FS_FAT12 ? "FAT12" : "OTHER",
             base, (uint32_t)fs->fatbase, (uint32_t)fs->database,
             (unsigned)fs->csize, (unsigned)fs->fsi_flag);
    fr = f_closedir(&dir);
    if (fr != FR_OK) return ESP_FAIL;
    if (type != FS_FAT32) return ESP_OK;
    uint8_t sector[512];
    esp_err_t err = sdmmc_read_sectors(s_card, sector, base, 1);
    if (err != ESP_OK) return err;
    unsigned offset = (unsigned)sector[48] | ((unsigned)sector[49] << 8);
    unsigned reserved = (unsigned)sector[14] | ((unsigned)sector[15] << 8);
    if (!offset || offset >= reserved || (uint64_t)base + offset >= s_card->csd.capacity) {
        ESP_LOGE(TAG, "FSINFO invalid BPB offset=%u reserved=%u", offset, reserved);
        return ESP_ERR_INVALID_RESPONSE;
    }
    s_fsinfo_lba = base + offset;
    err = sdmmc_read_sectors(s_card, sector, s_fsinfo_lba, 1);
    if (err != ESP_OK) return err;
    s_fsinfo_initial_hash = sector_hash(sector);
    s_fsinfo_initial_valid = true;
    ESP_LOGI(TAG, "FSINFO lba=%" PRIu32 " lead=0x%08" PRIx32
             " structure=0x%08" PRIx32 " trail=0x%08" PRIx32 " hash=0x%08" PRIx32
             " free_clusters=%" PRIu32 " next_free=%" PRIu32,
             s_fsinfo_lba, read_le32(sector), read_le32(sector + 484),
             read_le32(sector + 508), s_fsinfo_initial_hash,
             read_le32(sector + 488), read_le32(sector + 492));
    return ESP_OK;
}

static esp_err_t check_read_stability(void)
{
    uint8_t first[512], next[512];
    if (s_card->csd.sector_size != sizeof(first)) return ESP_ERR_NOT_SUPPORTED;
    esp_err_t err = sdmmc_read_sectors(s_card, first, 0, 1);
    for (unsigned i = 0; err == ESP_OK && i < 8; ++i) {
        err = sdmmc_read_sectors(s_card, next, 0, 1);
        if (err == ESP_OK && memcmp(first, next, sizeof(first)) != 0) {
            ESP_LOGE(TAG, "READ_BASELINE mismatch on repeat %u", i + 1);
            return ESP_ERR_INVALID_RESPONSE;
        }
    }
    ESP_LOGI(TAG, "READ_BASELINE sector0 repeat8: %s", esp_err_to_name(err));
    return err;
}

static esp_err_t release_bus(void)
{
    if (s_bus_owned) {
        esp_err_t err = spi_bus_free(SPI2_HOST);
        if (err != ESP_OK) return err;
        s_bus_owned = false;
    }
    /* Remove host drive before cutting switched TF power. */
    gpio_reset_pin(BOARD_TF_CS);
    gpio_reset_pin(BOARD_TF_CLK);
    gpio_reset_pin(BOARD_TF_MOSI);
    gpio_reset_pin(BOARD_TF_MISO);
    return gpio_set_level(BOARD_TF_POWER, !BOARD_TF_POWER_ON_LEVEL);
}

static void diagnose_mount_failure_readonly(const sdspi_device_config_t *slot_config)
{
    /* 官方挂载失败后已移除设备；在释放 SPI 总线前重新初始化，只读检查原始扇区。 */
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.max_freq_khz = 1000;
    esp_err_t err = host.init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "MOUNT_READONLY host_init=%s", esp_err_to_name(err));
        return;
    }
    sdspi_dev_handle_t handle;
    err = sdspi_host_init_device(slot_config, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "MOUNT_READONLY device_init=%s", esp_err_to_name(err));
        return;
    }
    host.slot = handle;
    sdmmc_card_t *card = calloc(1, sizeof(*card));
    if (!card) {
        ESP_LOGE(TAG, "MOUNT_READONLY alloc failed");
        sdspi_host_remove_device(handle);
        return;
    }
    err = sdmmc_card_init(&host, card);
    ESP_LOGI(TAG, "MOUNT_READONLY card_init=%s sectors=%" PRIu32 " sector_size=%u",
             esp_err_to_name(err), (uint32_t)card->csd.capacity,
             (unsigned)card->csd.sector_size);
    if (err == ESP_OK) {
        raw_log_card_identity("MOUNT_READONLY_CARD", card);
        if (s_raw_diag_requested &&
            (card->csd.sector_size != 512 || card->csd.capacity != 245760)) {
            s_raw_bad_identity_seen = true;
            ESP_LOGE(TAG, "RAW_IDENTITY_REJECT: 异常容量，停止本次压力测试及挂载重试");
        }
        ESP_LOGI(TAG, "MOUNT_READONLY identity ocr=0x%08" PRIx32
                 " mfg=0x%02x oem=0x%04x name=%.8s serial=0x%08x",
                 card->ocr, card->cid.mfg_id, card->cid.oem_id,
                 card->cid.name, card->cid.serial);
    }
    if (err == ESP_OK && card->csd.sector_size == 512) {
        const uint32_t lbas[] = {0, 32, 33};
        uint32_t sector[128];
        for (size_t i = 0; i < sizeof(lbas) / sizeof(lbas[0]); ++i) {
            if (lbas[i] >= card->csd.capacity) continue;
            err = sdmmc_read_sectors(card, sector, lbas[i], 1);
            const uint8_t *p = (const uint8_t *)sector;
            ESP_LOGI(TAG, "MOUNT_READONLY lba=%" PRIu32 " read=%s hash=0x%08" PRIx32
                     " first=%02x%02x%02x%02x sig=%02x%02x",
                     lbas[i], esp_err_to_name(err), err == ESP_OK ? sector_hash(sector) : 0,
                     err == ESP_OK ? p[0] : 0, err == ESP_OK ? p[1] : 0,
                     err == ESP_OK ? p[2] : 0, err == ESP_OK ? p[3] : 0,
                     err == ESP_OK ? p[510] : 0, err == ESP_OK ? p[511] : 0);
            if (err != ESP_OK) continue;
            if (lbas[i] == 0) {
                ESP_LOGI(TAG, "MOUNT_READONLY MBR part_type=%02x start=%" PRIu32
                         " sectors=%" PRIu32,
                         p[450], read_le32(p + 454), read_le32(p + 458));
            } else if (lbas[i] == 32) {
                ESP_LOGI(TAG, "MOUNT_READONLY VBR bps=%u spc=%u reserved=%u fats=%u"
                         " total=%" PRIu32 " fat_size=%" PRIu32 " fsinfo=%u",
                         (unsigned)(p[11] | p[12] << 8), p[13],
                         (unsigned)(p[14] | p[15] << 8), p[16],
                         read_le32(p + 32), read_le32(p + 36),
                         (unsigned)(p[48] | p[49] << 8));
            } else {
                ESP_LOGI(TAG, "MOUNT_READONLY FSINFO lead=0x%08" PRIx32
                         " structure=0x%08" PRIx32 " trail=0x%08" PRIx32,
                         read_le32(p), read_le32(p + 484), read_le32(p + 508));
            }
        }
    }
    free(card);
    esp_err_t cleanup = sdspi_host_remove_device(handle);
    ESP_LOGI(TAG, "MOUNT_READONLY cleanup=%s", esp_err_to_name(cleanup));
}

esp_err_t sd_card_mount(void)
{
    if (s_card) return ESP_OK;
    if (s_bus_owned) return ESP_ERR_INVALID_STATE;
    s_write_error = ESP_OK;
    s_fsinfo_lba = UINT32_MAX;
    s_fsinfo_initial_valid = false;
    gpio_config_t power = {
        .pin_bit_mask = 1ULL << BOARD_TF_POWER,
        .mode = GPIO_MODE_OUTPUT,
    };
    esp_err_t err = gpio_set_level(BOARD_TF_POWER, !BOARD_TF_POWER_ON_LEVEL);
    if (err != ESP_OK) return err;
    err = gpio_config(&power);
    if (err != ESP_OK) return err;
    gpio_config_t unused = {
        .pin_bit_mask = (1ULL << BOARD_TF_D1) | (1ULL << BOARD_TF_D2) | (1ULL << BOARD_TF_CD),
        .mode = GPIO_MODE_INPUT,
    };
    err = gpio_config(&unused);
    if (err != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(100));
    err = gpio_set_level(BOARD_TF_POWER, BOARD_TF_POWER_ON_LEVEL);
    if (err != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(200));
    ESP_LOGI(TAG, "SDSPI SPI2: CLK=%d CMD/MOSI=%d D0/MISO=%d D3/CS=%d POWER=%d(active low) CD=%d level=%d",
             BOARD_TF_CLK, BOARD_TF_MOSI, BOARD_TF_MISO, BOARD_TF_CS,
             BOARD_TF_POWER, BOARD_TF_CD, gpio_get_level(BOARD_TF_CD));
    /* CD polarity awaits physical verification: log only, do not gate mount. */
    spi_bus_config_t bus = {
        .mosi_io_num = BOARD_TF_MOSI, .miso_io_num = BOARD_TF_MISO,
        .sclk_io_num = BOARD_TF_CLK, .quadwp_io_num = -1, .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };
    err = spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        gpio_set_level(BOARD_TF_POWER, !BOARD_TF_POWER_ON_LEVEL);
        ESP_LOGE(TAG, "SPI2 init failed: %s", esp_err_to_name(err));
        return err;
    }
    s_bus_owned = true;
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI2_HOST;
    /* Keep the 1 MHz diagnostic clock, but use SDSPI_HOST_DEFAULT's timeout
     * policy as in the bundled SDSPI examples. sdmmc_cmd.c assigns 5000 ms
     * to CMD24 when command_timeout_ms remains zero. */
    host.max_freq_khz = 1000;
    s_transaction = host.do_transaction;
    host.do_transaction = trace_transaction;
    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.host_id = SPI2_HOST;
    slot.gpio_cs = BOARD_TF_CS;
    esp_vfs_fat_sdmmc_mount_config_t mount = {
        .format_if_mount_failed = false, .max_files = 2,
        .allocation_unit_size = 16 * 1024,
    };
    err = esp_vfs_fat_sdspi_mount(SD_CARD_MOUNT_POINT, &host, &slot, &mount, &s_card);
    if (err != ESP_OK) {
        s_card = NULL;
        ESP_LOGE(TAG, "SDSPI mount failed: %s (no formatting)", esp_err_to_name(err));
        diagnose_mount_failure_readonly(&slot);
        esp_err_t cleanup = release_bus();
        if (cleanup != ESP_OK) ESP_LOGE(TAG, "SPI cleanup failed: %s", esp_err_to_name(cleanup));
        return err;
    }
    sdmmc_card_print_info(stdout, s_card);
    raw_log_card_identity("MOUNTED_CARD", s_card);
    err = check_read_stability();
    if (err == ESP_OK && !s_raw_diag_requested) err = inspect_filesystem();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Read stability failed; skip file writes");
        esp_err_t cleanup = sd_card_unmount();
        if (cleanup != ESP_OK) ESP_LOGE(TAG, "Cleanup failed: %s", esp_err_to_name(cleanup));
        return err;
    }
    ESP_LOGI(TAG, "SDSPI mounted %s, max clock=%d kHz, host timeout override=%d ms (CMD24 default=5000 ms)",
             SD_CARD_MOUNT_POINT, host.max_freq_khz, host.command_timeout_ms);
    return ESP_OK;
}

esp_err_t sd_card_unmount(void)
{
    if (s_card) {
        esp_err_t err = esp_vfs_fat_sdcard_unmount(SD_CARD_MOUNT_POINT, s_card);
        if (err != ESP_OK) return err;
        s_card = NULL;
    } else if (!s_bus_owned) {
        return ESP_OK;
    }
    return release_bus();
}

bool sd_card_is_mounted(void) { return s_card != NULL; }

static void raw_log_edge_bytes(const char *stage, const uint8_t *data)
{
    char first[32 * 3], last[32 * 3];
    for (size_t i = 0; i < 32; ++i) {
        snprintf(first + i * 3, sizeof(first) - i * 3, "%02x%s", data[i], i == 31 ? "" : " ");
        snprintf(last + i * 3, sizeof(last) - i * 3, "%02x%s", data[512 - 32 + i], i == 31 ? "" : " ");
    }
    ESP_LOGI(TAG, "%s first32=%s", stage, first);
    ESP_LOGI(TAG, "%s last32=%s", stage, last);
}

typedef struct {
    uint32_t total, write_ok, read_ok, verify_ok;
    uint32_t write_fail, busy_timeout, cmd17_timeout, cmd13_error, memcmp_fail;
} raw_stress_stats_t;

static esp_err_t raw_stress_write_verify(uint32_t lba, unsigned pass,
                                         raw_stress_stats_t *stats)
{
    uint32_t before[128], expected[128], readback[128];
    memset(before, 0xa5, sizeof(before));
    memset(expected, 0, sizeof(expected));
    memset(readback, 0xa5, sizeof(readback));
    ++stats->total;

    /* 每轮先读当前内容；读失败时不向该扇区写入，也不自动重试。 */
    esp_err_t before_err = sdmmc_read_sectors(s_card, before, lba, 1);
    esp_err_t write_err = ESP_ERR_INVALID_STATE;
    esp_err_t status_err = ESP_ERR_INVALID_STATE;
    esp_err_t read_err = ESP_ERR_INVALID_STATE;
    uint32_t before_hash = before_err == ESP_OK ? sector_hash(before) : 0;
    uint32_t expected_hash = 0;
    uint32_t readback_hash = 0;
    uint32_t arg = (s_card->ocr & SD_OCR_SDHC_CAP) ? lba : lba * 512U;
    uint8_t r1 = 0, token = 0, busy_last_rx = 0;
    bool r1_seen = false, token_seen = false;
    int64_t busy_us = 0;
    esp_err_t busy_result = ESP_ERR_INVALID_STATE;
    const char *driver_failure = NULL;
    const char *failure = NULL;
    uint32_t cmd13_response = 0;
    bool match = false;

    if (before_err != ESP_OK) {
        if (before_err == ESP_ERR_TIMEOUT) ++stats->cmd17_timeout;
        failure = "before_cmd17";
        goto failed;
    }

    /* pass 每轮变化；若碰巧与旧内容一致，则轮换偏移，直到字节与哈希均不同。 */
    bool pattern_selected = false;
    for (unsigned attempt = 0; attempt < 4; ++attempt) {
        for (size_t i = 0; i < sizeof(expected); ++i) {
            ((uint8_t *)expected)[i] = (uint8_t)(i + pass * 37U + attempt * 53U);
        }
        expected_hash = sector_hash(expected);
        if (memcmp(before, expected, sizeof(expected)) != 0 &&
            before_hash != expected_hash) {
            pattern_selected = true;
            break;
        }
    }
    if (!pattern_selected) {
        failure = "pattern_not_distinct";
        goto failed;
    }

    sdmmc_command_t cmd = {
        .opcode = 24, .arg = arg,
        .data = expected, .datalen = sizeof(expected),
        .buflen = sizeof(expected), .blklen = 512,
        .flags = SCF_CMD_ADTC | SCF_RSP_R1, .timeout_ms = 5000,
    };
    write_err = s_transaction(s_card->host.slot, &cmd);
    /* 下一条命令会复用驱动的诊断字段，必须在此刻取走 CMD24 现场。 */
    sdspi_diag_get_last_write(&r1, &r1_seen, &token, &token_seen,
                              &busy_us, &busy_last_rx, &busy_result, &driver_failure);
    if (write_err == ESP_OK && (cmd.response[0] & 0xff) == 0 &&
        r1_seen && r1 == 0 && token_seen && (token & 0x1f) == 0x05 &&
        busy_result == ESP_OK) {
        ++stats->write_ok;
    } else {
        ++stats->write_fail;
    }
    if (busy_result == ESP_ERR_TIMEOUT) ++stats->busy_timeout;

    /* CMD24 即使失败，也只做状态和回读诊断，不再发下一笔写命令。 */
    sdmmc_command_t status = {
        .opcode = 13, .arg = 0,
        .flags = SCF_CMD_AC | SCF_RSP_R1, .timeout_ms = 1000,
    };
    status_err = s_transaction(s_card->host.slot, &status);
    cmd13_response = status.response[0];
    if (status_err != ESP_OK || cmd13_response != 0) ++stats->cmd13_error;

    /* 哨兵值用于判断 CMD17 是否覆盖了完整的 512B 缓冲区。 */
    read_err = sdmmc_read_sectors(s_card, readback, lba, 1);
    if (read_err == ESP_OK) {
        ++stats->read_ok;
        readback_hash = sector_hash(readback);
        match = memcmp(expected, readback, sizeof(expected)) == 0;
        if (!match) ++stats->memcmp_fail;
    } else if (read_err == ESP_ERR_TIMEOUT) {
        ++stats->cmd17_timeout;
    }

    if (stats->write_ok + stats->write_fail == stats->total &&
        write_err == ESP_OK && (cmd.response[0] & 0xff) == 0 &&
        r1_seen && r1 == 0 && token_seen && (token & 0x1f) == 0x05 &&
        busy_result == ESP_OK && status_err == ESP_OK && cmd13_response == 0 &&
        read_err == ESP_OK && match) {
        ++stats->verify_ok;
        return ESP_OK;
    }
    failure = write_err != ESP_OK ? (driver_failure ? driver_failure : "cmd24_error") :
              !r1_seen || r1 != 0 ? "cmd24_r1" :
              !token_seen || (token & 0x1f) != 0x05 ? "data_response" :
              busy_result != ESP_OK ? "busy_error" :
              status_err != ESP_OK || cmd13_response != 0 ? "cmd13_error" :
              read_err != ESP_OK ? "after_cmd17" : "memcmp_fail";

failed:
    if (write_err != ESP_OK && write_err != ESP_ERR_INVALID_STATE) s_write_error = write_err;
    unsigned count_00 = 0, count_ff = 0, count_a5 = 0, count_other = 0;
    const uint8_t *bytes = (const uint8_t *)readback;
    for (size_t i = 0; i < sizeof(readback); ++i) {
        if (bytes[i] == 0x00) ++count_00;
        else if (bytes[i] == 0xff) ++count_ff;
        else if (bytes[i] == 0xa5) ++count_a5;
        else ++count_other;
    }
    ESP_LOGE(TAG, "RAW_STRESS_FAIL pass=%u/1000 LBA=%" PRIu32 " failure=%s",
             pass + 1, lba, failure ? failure : "unknown");
    ESP_LOGE(TAG, "RAW_STRESS_CMD24 arg=0x%08" PRIx32 " ret=%s R1=%s0x%02x"
             " token=%s0x%02x masked=0x%02x busy=%" PRId64
             "us busy_result=%s busy_last_rx=0x%02x",
             arg, esp_err_to_name(write_err), r1_seen ? "" : "UNSEEN:", r1,
             token_seen ? "" : "UNSEEN:", token, token & 0x1f, busy_us,
             esp_err_to_name(busy_result), busy_last_rx);
    ESP_LOGE(TAG, "RAW_STRESS_CMD13 ret=%s R1=0x%02" PRIx32 " R2=0x%02" PRIx32
             " BEFORE_CMD17=%s AFTER_CMD17=%s",
             esp_err_to_name(status_err), cmd13_response & 0xff,
             (cmd13_response >> 8) & 0xff, esp_err_to_name(before_err),
             esp_err_to_name(read_err));
    ESP_LOGE(TAG, "RAW_STRESS_HASH before=0x%08" PRIx32 " expected=0x%08" PRIx32
             " readback=0x%08" PRIx32 " before_expected_match=%d memcmp=%d",
             before_hash, expected_hash, readback_hash,
             before_err == ESP_OK && memcmp(before, expected, sizeof(expected)) == 0,
             read_err == ESP_OK && match);
    raw_log_edge_bytes("RAW_STRESS_EXPECTED", (const uint8_t *)expected);
    raw_log_edge_bytes("RAW_STRESS_READBACK", (const uint8_t *)readback);
    ESP_LOGE(TAG, "RAW_STRESS_BYTE_STATS FF=%u A5=%u 00=%u OTHER=%u",
             count_ff, count_a5, count_00, count_other);
    return before_err != ESP_OK ? before_err :
           write_err != ESP_OK ? write_err :
           status_err != ESP_OK ? status_err :
           read_err != ESP_OK ? read_err : ESP_ERR_INVALID_RESPONSE;
}

static esp_err_t find_raw_scratch_lba(uint32_t *out_lba)
{
    uint32_t sector[128];
    esp_err_t err = sdmmc_read_sectors(s_card, sector, 0, 1);
    if (err != ESP_OK) return err;
    const uint8_t *p = (const uint8_t *)sector;
    if (p[510] != 0x55 || p[511] != 0xaa) return ESP_ERR_INVALID_RESPONSE;
    uint32_t part_lba = read_le32(p + 446 + 8);
    uint32_t part_sectors = read_le32(p + 446 + 12);
    if (!part_lba || !part_sectors || (uint64_t)part_lba + part_sectors > s_card->csd.capacity) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    err = sdmmc_read_sectors(s_card, sector, part_lba, 1);
    if (err != ESP_OK) return err;
    p = (const uint8_t *)sector;
    uint32_t total = read_le32(p + 32);
    uint32_t fat_sectors = read_le32(p + 36);
    uint32_t reserved = p[14] | ((uint32_t)p[15] << 8);
    uint32_t fats = p[16], cluster_sectors = p[13];
    if (p[510] != 0x55 || p[511] != 0xaa || (p[82] != 'F' || p[83] != 'A' || p[84] != 'T') ||
        p[11] != 0 || p[12] != 2 || !fat_sectors || !reserved || !fats ||
        !cluster_sectors || (cluster_sectors & (cluster_sectors - 1)) ||
        total > part_sectors || total <= reserved + fats * fat_sectors) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    uint32_t fat_lba = part_lba + reserved;
    uint32_t data_lba = fat_lba + fats * fat_sectors;
    uint32_t cluster_count = (total - reserved - fats * fat_sectors) / cluster_sectors;
    const uint32_t needed = (16 + cluster_sectors - 1) / cluster_sectors;
    if ((uint64_t)fat_sectors * 128 < (uint64_t)cluster_count + 2) return ESP_ERR_INVALID_RESPONSE;
    uint32_t cached_fat_lba = UINT32_MAX, run_start = 0, run_count = 0;
    for (uint32_t cluster = 2; cluster < cluster_count + 2; ++cluster) {
        uint32_t entry_lba = fat_lba + cluster / 128;
        if (entry_lba != cached_fat_lba) {
            err = sdmmc_read_sectors(s_card, sector, entry_lba, 1);
            if (err != ESP_OK) return err;
            cached_fat_lba = entry_lba;
        }
        uint32_t entry = sector[cluster % 128] & 0x0fffffff;
        if (entry == 0) {
            if (!run_count) run_start = cluster;
            if (++run_count >= needed) {
                uint32_t candidate = data_lba + (run_start - 2) * cluster_sectors;
                if ((uint64_t)candidate + 16 > (uint64_t)part_lba + total) {
                    return ESP_ERR_INVALID_RESPONSE;
                }
                // 所有 FAT 副本都标记为空闲时，才允许使用这段测试区域。
                bool all_copies_free = true;
                for (uint32_t fat = 1; fat < fats && all_copies_free; ++fat) {
                    uint32_t check_lba = UINT32_MAX;
                    for (uint32_t c = run_start; c < run_start + needed; ++c) {
                        uint32_t next_lba = fat_lba + fat * fat_sectors + c / 128;
                        if (next_lba != check_lba) {
                            err = sdmmc_read_sectors(s_card, sector, next_lba, 1);
                            if (err != ESP_OK) return err;
                            check_lba = next_lba;
                        }
                        if ((sector[c % 128] & 0x0fffffff) != 0) {
                            all_copies_free = false;
                            break;
                        }
                    }
                }
                cached_fat_lba = UINT32_MAX;
                if (!all_copies_free) {
                    run_count = 0;
                    continue;
                }
                *out_lba = candidate;
                ESP_LOGI(TAG, "RAW_SCRATCH_FREE lba=%" PRIu32 " sectors=16 clusters=%" PRIu32,
                         candidate, needed);
                return ESP_OK;
            }
        } else {
            run_count = 0;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t sd_card_raw_diagnostic(void)
{
    s_raw_diag_requested = true;
    s_raw_bad_identity_seen = false;
    sdspi_diag_set_raw_mode(false);
    sdspi_diag_reset_busy_stats();
    raw_stress_stats_t stats = {0};
    esp_err_t err = ESP_FAIL;
    uint32_t base_lba = 0;
    /* 仅实验档位重试挂载；每次失败都会释放 SPI 总线并停止 TF 供电。 */
    for (unsigned attempt = 1; attempt <= 5; ++attempt) {
        ESP_LOGI(TAG, "RAW_MOUNT attempt=%u/5", attempt);
        err = sd_card_mount();
        if (err == ESP_OK) break;
        ESP_LOGW(TAG, "RAW_MOUNT attempt=%u result=%s", attempt, esp_err_to_name(err));
        if (s_raw_bad_identity_seen) break;
        if (attempt < 5) vTaskDelay(pdMS_TO_TICKS(500));
    }
    if (err != ESP_OK) {
        goto done;
    }
    /* 只允许对先前确认的 120 MiB FAT32 卡执行裸写，拒绝 8 MiB 异常身份。 */
    if (s_card->csd.sector_size != 512 || s_card->csd.capacity != 245760) {
        ESP_LOGE(TAG, "RAW_IDENTITY_REJECT sectors=%" PRIu32 " sector_size=%d",
                 (uint32_t)s_card->csd.capacity, s_card->csd.sector_size);
        err = ESP_ERR_INVALID_RESPONSE;
    }
    if (err == ESP_OK) err = find_raw_scratch_lba(&base_lba);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "RAW_SCRATCH select failed: %s; raw CMD24 skipped", esp_err_to_name(err));
        goto done;
    }
    ESP_LOGI(TAG, "RAW_STRESS begin same_lba=%" PRIu32 " passes=500 rotate_lba=%" PRIu32
             "..%" PRIu32 " passes=500 SPI_khz=1000 no_cmd24_retry=1",
             base_lba, base_lba, base_lba + 15);
    for (unsigned pass = 0; pass < 1000 && err == ESP_OK; ++pass) {
        /* 前 500 轮固定一个扇区，后 500 轮仅轮换已确认空闲的 16 个扇区。 */
        const uint32_t lba = pass < 500 ? base_lba : base_lba + (pass - 500) % 16;
        err = raw_stress_write_verify(lba, pass, &stats);
        if (err == ESP_OK && (pass + 1) % 100 == 0) {
            ESP_LOGI(TAG, "RAW_STRESS progress=%u/1000", pass + 1);
        }
    }
    if (err == ESP_OK && (stats.total != 1000 || stats.write_ok != stats.total ||
                          stats.read_ok != stats.total || stats.verify_ok != stats.total ||
                          stats.write_fail || stats.busy_timeout || stats.cmd17_timeout ||
                          stats.cmd13_error || stats.memcmp_fail)) {
        err = ESP_ERR_INVALID_RESPONSE;
    }
done:;
    uint32_t busy_count = 0, busy_bins[7] = {0};
    int64_t busy_min_us = 0, busy_max_us = 0;
    uint64_t busy_total_us = 0;
    sdspi_diag_get_busy_stats(&busy_count, &busy_min_us, &busy_max_us,
                              &busy_total_us, busy_bins);
    ESP_LOGI(TAG, "RAW_STRESS SUMMARY total=%" PRIu32 " write_ok=%" PRIu32
             " read_ok=%" PRIu32 " verify_ok=%" PRIu32 " write_fail=%" PRIu32
             " busy_timeout=%" PRIu32 " cmd17_timeout=%" PRIu32
             " cmd13_error=%" PRIu32 " memcmp_fail=%" PRIu32 " result=%s",
             stats.total, stats.write_ok, stats.read_ok, stats.verify_ok,
             stats.write_fail, stats.busy_timeout, stats.cmd17_timeout,
             stats.cmd13_error, stats.memcmp_fail, esp_err_to_name(err));
    ESP_LOGI(TAG, "RAW_STRESS BUSY count=%" PRIu32 " min=%" PRId64 "us max=%" PRId64
             "us avg=%" PRIu64 "us bins_<1ms/1-5/5-20/20-100/100-500/500-1000/>=1000ms="
             "%" PRIu32 "/%" PRIu32 "/%" PRIu32 "/%" PRIu32 "/%" PRIu32 "/%" PRIu32 "/%" PRIu32,
             busy_count, busy_min_us, busy_max_us,
             busy_count ? busy_total_us / busy_count : 0,
             busy_bins[0], busy_bins[1], busy_bins[2], busy_bins[3],
             busy_bins[4], busy_bins[5], busy_bins[6]);
    esp_err_t unmount_err = sd_card_unmount();
    s_raw_diag_requested = false;
    if (err == ESP_OK) err = unmount_err;
    ESP_LOGI(TAG, "RAW_DIAGNOSTIC result=%s", esp_err_to_name(err));
    return err;
}
#endif
