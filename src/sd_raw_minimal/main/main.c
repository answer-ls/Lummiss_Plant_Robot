#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/sdspi_host.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdmmc_cmd.h"
#include "sd_protocol_defs.h"
#include "esp_private/sdmmc_common.h"

#define TF_CLK GPIO_NUM_42
#define TF_MOSI GPIO_NUM_43
#define TF_MISO GPIO_NUM_41
#define TF_CS GPIO_NUM_44
#define TF_POWER GPIO_NUM_45
#define TF_D1 GPIO_NUM_40
#define TF_D2 GPIO_NUM_46
#define TF_CD GPIO_NUM_39
#define BOARD_PWR_IO GPIO_NUM_7
#define RAW_SECTORS 16U

static const char *TAG = "SD_RAW_MIN";
static sdmmc_card_t s_card;

extern void sdspi_diag_set_raw_mode(bool enabled);
extern void sdspi_diag_dump_busy_stats(void);

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static esp_err_t raw_read(uint32_t lba, uint32_t *sector)
{
    return sdmmc_read_sectors(&s_card, sector, lba, 1);
}

static esp_err_t find_free_data_run(uint32_t *out_lba)
{
    uint32_t sector[128];
    esp_err_t err = raw_read(0, sector);
    ESP_LOGI(TAG, "SECTOR0 read=%s", esp_err_to_name(err));
    if (err != ESP_OK) return err;
    const uint8_t *p = (const uint8_t *)sector;
    // 只读记录原始扇区，用于区分容量解析错误与数据读取错误。
    ESP_LOG_BUFFER_HEX(TAG, p, 64);
    ESP_LOG_BUFFER_HEX(TAG, p + 496, 16);
    const uint32_t part_lba = le32(p + 454);
    const uint32_t part_sectors = le32(p + 458);
    ESP_LOGI(TAG, "MBR signature=%02x%02x part_type=0x%02x start=%" PRIu32
             " sectors=%" PRIu32 " card_sectors=%" PRIu32,
             p[511], p[510], p[450], part_lba, part_sectors, (uint32_t)s_card.csd.capacity);
    if (p[510] != 0x55 || p[511] != 0xaa) return ESP_ERR_INVALID_RESPONSE;
    if (!part_lba || !part_sectors || (uint64_t)part_lba + part_sectors > s_card.csd.capacity) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    err = raw_read(part_lba, sector);
    if (err != ESP_OK) return err;
    p = (const uint8_t *)sector;
    const uint32_t total = le32(p + 32);
    const uint32_t fat_sectors = le32(p + 36);
    const uint32_t reserved = p[14] | ((uint32_t)p[15] << 8);
    const uint32_t fats = p[16], cluster_sectors = p[13];
    if (p[510] != 0x55 || p[511] != 0xaa || memcmp(p + 82, "FAT", 3) != 0 ||
        p[11] != 0 || p[12] != 2 || !fat_sectors || !reserved || !fats ||
        !cluster_sectors || (cluster_sectors & (cluster_sectors - 1)) ||
        total > part_sectors || total <= reserved + fats * fat_sectors) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    const uint32_t fat_lba = part_lba + reserved;
    const uint32_t data_lba = fat_lba + fats * fat_sectors;
    const uint32_t clusters = (total - reserved - fats * fat_sectors) / cluster_sectors;
    const uint32_t needed = (RAW_SECTORS + cluster_sectors - 1) / cluster_sectors;
    if ((uint64_t)fat_sectors * 128 < (uint64_t)clusters + 2) return ESP_ERR_INVALID_RESPONSE;
    uint32_t cached = UINT32_MAX, run_start = 0, run_size = 0;
    for (uint32_t cluster = 2; cluster < clusters + 2; ++cluster) {
        const uint32_t lba = fat_lba + cluster / 128;
        if (lba != cached) {
            err = raw_read(lba, sector);
            if (err != ESP_OK) return err;
            cached = lba;
        }
        if ((sector[cluster % 128] & 0x0fffffff) != 0) {
            run_size = 0;
            continue;
        }
        if (!run_size) run_start = cluster;
        if (++run_size < needed) continue;

        // 仅选择所有 FAT 副本都确认空闲的扇区，避免覆盖已有文件。
        bool all_free = true;
        for (uint32_t fat = 1; fat < fats && all_free; ++fat) {
            uint32_t copy_cached = UINT32_MAX;
            for (uint32_t c = run_start; c < run_start + needed; ++c) {
                const uint32_t copy_lba = fat_lba + fat * fat_sectors + c / 128;
                if (copy_lba != copy_cached) {
                    err = raw_read(copy_lba, sector);
                    if (err != ESP_OK) return err;
                    copy_cached = copy_lba;
                }
                if ((sector[c % 128] & 0x0fffffff) != 0) {
                    all_free = false;
                    break;
                }
            }
        }
        cached = UINT32_MAX;
        if (!all_free) {
            run_size = 0;
            continue;
        }
        const uint32_t candidate = data_lba + (run_start - 2) * cluster_sectors;
        if ((uint64_t)candidate + RAW_SECTORS > (uint64_t)part_lba + total) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        *out_lba = candidate;
        return ESP_OK;
    }
    return ESP_ERR_NOT_FOUND;
}

static void recovery_matrix(void)
{
    static const uint32_t delays_ms[] = {10, 100, 500, 1000, 5000};
    const int64_t start = esp_timer_get_time();
    for (size_t i = 0; i < sizeof(delays_ms) / sizeof(delays_ms[0]); ++i) {
        int64_t remain_us = start + (int64_t)delays_ms[i] * 1000 - esp_timer_get_time();
        if (remain_us > 0) vTaskDelay(pdMS_TO_TICKS((remain_us + 999) / 1000));
        const int64_t actual_ms = (esp_timer_get_time() - start) / 1000;
        sdmmc_command_t status = {
            .opcode = 13, .flags = SCF_CMD_AC | SCF_RSP_R1, .timeout_ms = 50,
        };
        esp_err_t se = s_card.host.do_transaction(s_card.host.slot, &status);
        uint32_t sector0[128] = {0};
        sdmmc_command_t read = {
            .opcode = 17, .data = sector0, .datalen = 512, .buflen = 512,
            .blklen = 512, .flags = SCF_CMD_ADTC | SCF_CMD_READ | SCF_RSP_R1,
            .timeout_ms = 50,
        };
        esp_err_t re = s_card.host.do_transaction(s_card.host.slot, &read);
        ESP_LOGI(TAG, "RECOVERY target=%" PRIu32 "ms actual=%" PRId64
                 "ms CMD13=%s valid=%d R1=0x%02" PRIx32 " R2=0x%02" PRIx32
                 " CMD17=%s valid=%d",
                 delays_ms[i], actual_ms, esp_err_to_name(se), se == ESP_OK,
                 status.response[0] & 0xff, (status.response[0] >> 8) & 0xff,
                 esp_err_to_name(re), re == ESP_OK);
    }
    sdmmc_host_t host = s_card.host;
    ESP_LOGI(TAG, "SOFT_INIT begin power_cycle=0");
    esp_err_t err = sdmmc_card_init(&host, &s_card);
    uint32_t sector0[128] = {0};
    esp_err_t read_err = err == ESP_OK ? raw_read(0, sector0) : err;
    ESP_LOGI(TAG, "SOFT_INIT init=%s CMD17=%s", esp_err_to_name(err), esp_err_to_name(read_err));
}

static esp_err_t write_verify(uint32_t lba, unsigned pass)
{
    uint32_t tx[128], rx[128];
    for (unsigned i = 0; i < 512; ++i) ((uint8_t *)tx)[i] = (uint8_t)(lba + pass * 31 + i * 7);
    sdmmc_command_t cmd = {
        .opcode = 24,
        .arg = (s_card.ocr & SD_OCR_SDHC_CAP) ? lba : lba * 512U,
        .data = tx, .datalen = 512, .buflen = 512, .blklen = 512,
        .flags = SCF_CMD_ADTC | SCF_RSP_R1, .timeout_ms = 5000,
    };
    esp_err_t err = s_card.host.do_transaction(s_card.host.slot, &cmd);
    ESP_LOGI(TAG, "RAW_WRITE lba=%" PRIu32 " pass=%u result=%s R1=0x%02" PRIx32,
             lba, pass, esp_err_to_name(err), cmd.response[0] & 0xff);
    if (err != ESP_OK) {
        if (err == ESP_ERR_TIMEOUT) recovery_matrix();
        return err;
    }
    err = raw_read(lba, rx);
    const bool match = err == ESP_OK && memcmp(tx, rx, sizeof(tx)) == 0;
    ESP_LOGI(TAG, "RAW_READBACK lba=%" PRIu32 " pass=%u result=%s match=%d",
             lba, pass, esp_err_to_name(err), match);
    return err != ESP_OK ? err : match ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

void app_main(void)
{
    ESP_LOGI(TAG, "MINIMAL_BOOT hosted=%d wifi_app=absent fatfs=absent lcd=absent",
             SD_RAW_WITH_HOSTED);
    // 与主工程保持相同的板级 GPIO7 锁存状态，排除最小工程遗漏初始化的影响。
    esp_err_t err = gpio_set_level(BOARD_PWR_IO, 1);
    if (err != ESP_OK) goto done;
    const gpio_config_t board_power = {
        .pin_bit_mask = 1ULL << BOARD_PWR_IO,
        .mode = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    err = gpio_config(&board_power);
    if (err != ESP_OK) goto done;
    err = gpio_set_level(BOARD_PWR_IO, 1);
    if (err != ESP_OK) goto done;
    ESP_LOGI(TAG, "BOARD_PWR_IO gpio=%d level=%d", BOARD_PWR_IO, gpio_get_level(BOARD_PWR_IO));
    gpio_set_level(TF_POWER, 1);
    const gpio_config_t power = {.pin_bit_mask = 1ULL << TF_POWER, .mode = GPIO_MODE_OUTPUT};
    err = gpio_config(&power);
    if (err != ESP_OK) goto done;
    // 与主工程保持相同的未用 TF 引脚输入状态，不改变 SPI 引脚分配。
    const gpio_config_t unused = {
        .pin_bit_mask = (1ULL << TF_D1) | (1ULL << TF_D2) | (1ULL << TF_CD),
        .mode = GPIO_MODE_INPUT,
    };
    err = gpio_config(&unused);
    if (err != ESP_OK) goto done;
    vTaskDelay(pdMS_TO_TICKS(100));
    err = gpio_set_level(TF_POWER, 0);
    if (err != ESP_OK) goto done;
    vTaskDelay(pdMS_TO_TICKS(200));

    const spi_bus_config_t bus = {
        .mosi_io_num = TF_MOSI, .miso_io_num = TF_MISO, .sclk_io_num = TF_CLK,
        .quadwp_io_num = -1, .quadhd_io_num = -1, .max_transfer_sz = 4096,
    };
    err = spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) goto done;
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI2_HOST;
    host.max_freq_khz = 1000;
    err = host.init();
    if (err != ESP_OK) goto done;
    sdspi_device_config_t dev = SDSPI_DEVICE_CONFIG_DEFAULT();
    dev.host_id = SPI2_HOST;
    dev.gpio_cs = TF_CS;
    sdspi_dev_handle_t handle;
    err = sdspi_host_init_device(&dev, &handle);
    if (err != ESP_OK) goto done;
    host.slot = handle;
    err = sdmmc_card_init(&host, &s_card);
    if (err != ESP_OK) goto done;
    ESP_LOGI(TAG, "CARD type=%s sectors=%" PRIu32 " sector_size=%" PRIu32,
             (s_card.ocr & SD_OCR_SDHC_CAP) ? "SDHC" : "SDSC",
             s_card.csd.capacity, s_card.csd.sector_size);
    // CSD 容量与主工程不一致时，只重读寄存器定位原因，不修改卡的容量或写入范围。
    for (unsigned i = 0; i < 3; ++i) {
        sdmmc_csd_t csd = {0};
        esp_err_t csd_err = sdmmc_send_cmd_send_csd(&s_card, &csd);
        ESP_LOGI(TAG, "CSD_REPEAT pass=%u result=%s ver=%d sectors=%d sector_size=%d read_bl_len=%d",
                 i, esp_err_to_name(csd_err), csd.csd_ver, csd.capacity,
                 csd.sector_size, csd.read_block_len);
    }
    if (s_card.csd.sector_size != 512) { err = ESP_ERR_NOT_SUPPORTED; goto done; }
    uint32_t base_lba = 0;
    err = find_free_data_run(&base_lba);
    if (err != ESP_OK) goto done;
    ESP_LOGI(TAG, "RAW_SCRATCH_FREE lba=%" PRIu32 " sectors=%u", base_lba, RAW_SECTORS);
    sdspi_diag_set_raw_mode(true);
    for (unsigned pass = 0; pass < 4 && err == ESP_OK; ++pass) err = write_verify(base_lba, pass);
    for (unsigned i = 0; i < RAW_SECTORS && err == ESP_OK; ++i) err = write_verify(base_lba + i, i + 4);
    sdspi_diag_set_raw_mode(false);
    sdspi_diag_dump_busy_stats();
done:
    ESP_LOGI(TAG, "MINIMAL_RESULT=%s", esp_err_to_name(err));
}
