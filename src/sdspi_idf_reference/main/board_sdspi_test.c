#include <inttypes.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/sdspi_host.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdmmc_cmd.h"

#define BOARD_PWR_IO GPIO_NUM_7
#define TF_POWER GPIO_NUM_45
#define TF_D1 GPIO_NUM_40
#define TF_D2 GPIO_NUM_46
#define TF_CD GPIO_NUM_39

static const char *TAG = "IDF_SDSPI_REF";

#define TEST_BYTES 16384U
#define TEST_CHUNK 512U

static esp_err_t (*s_native_transaction)(int, sdmmc_command_t *);

static esp_err_t trace_card_probe(int slot, sdmmc_command_t *cmd)
{
    // 只记录识别阶段的命令结果；不更改参数、响应或底层 SPI 时序。
    esp_err_t err = s_native_transaction(slot, cmd);
    if (cmd->opcode == 0 || cmd->opcode == 5 || cmd->opcode == 8 ||
        cmd->opcode == 9 || cmd->opcode == 52 || cmd->opcode == 58) {
        ESP_LOGI(TAG, "PROBE CMD%d arg=0x%08" PRIx32 " result=%s response0=0x%08" PRIx32,
                 cmd->opcode, cmd->arg, esp_err_to_name(err), cmd->response[0]);
    }
    // 时序 A/B：与主程序 trace_transaction() 一致，在每条命令结束后等待 1 tick。
    // 此轮不覆盖 CMD5 返回值，以便观察等待本身是否改变 CMD5/CSD/扇区0。
    vTaskDelay(1);
    return err;
}

static void diagnose_mount_failure(const sdmmc_host_t *base_host,
                                   const sdspi_device_config_t *slot)
{
    // 官方挂载接口在 FATFS 失败时释放 card，无法向调用者返回 CSD。
    // 因此失败后用同一原生 SDSPI 驱动重新识别一次，只读取寄存器和扇区。
    sdmmc_host_t host = *base_host;
    sdspi_dev_handle_t handle = 0;
    esp_err_t err = host.init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "RAW_DIAG host_init=%s", esp_err_to_name(err));
        return;
    }
    err = sdspi_host_init_device(slot, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "RAW_DIAG device_init=%s", esp_err_to_name(err));
        return;
    }
    host.slot = handle;
    // main 任务默认只有 3584 字节栈；卡结构体和扇区缓冲放堆上，避免诊断时栈溢出。
    sdmmc_card_t *card = calloc(1, sizeof(*card));
    uint32_t *sector = calloc(128, sizeof(*sector));
    if (!card || !sector) {
        ESP_LOGE(TAG, "RAW_DIAG alloc=ESP_ERR_NO_MEM");
        goto cleanup;
    }
    err = sdmmc_card_init(&host, card);
    ESP_LOGI(TAG, "RAW_DIAG card_init=%s", esp_err_to_name(err));
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "RAW_DIAG is_mem=%d is_sdio=%d ocr=0x%08" PRIx32
                 " csd_ver=%d sectors=%" PRIu32
                 " sector_size=%" PRIu32 " read_bl_len=%d",
                 card->is_mem, card->is_sdio, card->ocr, card->csd.csd_ver, card->csd.capacity,
                 card->csd.sector_size, card->csd.read_block_len);
        sdmmc_card_print_info(stdout, card);
        err = sdmmc_read_sectors(card, sector, 0, 1);
        ESP_LOGI(TAG, "RAW_DIAG sector0=%s", esp_err_to_name(err));
        if (err == ESP_OK) {
            const uint8_t *p = (const uint8_t *)sector;
            ESP_LOG_BUFFER_HEX(TAG, p, 64);
            ESP_LOG_BUFFER_HEX(TAG, p + 496, 16);
            ESP_LOGI(TAG, "RAW_DIAG mbr_signature=%02x%02x part_type=0x%02x",
                     p[511], p[510], p[450]);
        }
    }
cleanup:
    free(sector);
    free(card);
    esp_err_t cleanup = sdspi_host_remove_device(handle);
    ESP_LOGI(TAG, "RAW_DIAG cleanup=%s", esp_err_to_name(cleanup));
}

static void fill_pattern(uint8_t *buf, unsigned chunk)
{
    for (unsigned i = 0; i < TEST_CHUNK; ++i) {
        buf[i] = (uint8_t)(((chunk * TEST_CHUNK + i) * 7U + 0x5aU) & 0xffU);
    }
}

static esp_err_t test_file_roundtrip(void)
{
    char path[64] = {0};
    struct stat st = {0};
    bool found = false;
    // 选用尚不存在的文件名，不覆盖卡上原有文件；成功后保留文件供电脑核对。
    for (unsigned n = 0; n < 100; ++n) {
        snprintf(path, sizeof(path), "/sdcard/codex_spi_%02u.bin", n);
        errno = 0;
        if (stat(path, &st) != 0 && errno == ENOENT) {
            found = true;
            break;
        }
    }
    if (!found) {
        ESP_LOGE(TAG, "没有找到空闲的测试文件名");
        return ESP_ERR_NOT_FOUND;
    }

    uint8_t expected[TEST_CHUNK], actual[TEST_CHUNK];
    ESP_LOGI(TAG, "WRITE_START path=%s bytes=%u", path, TEST_BYTES);
    FILE *fw = fopen(path, "wb");
    if (!fw) {
        ESP_LOGE(TAG, "CREATE_FAIL errno=%d", errno);
        return ESP_FAIL;
    }
    int64_t started = esp_timer_get_time();
    esp_err_t result = ESP_OK;
    for (unsigned chunk = 0; chunk < TEST_BYTES / TEST_CHUNK; ++chunk) {
        fill_pattern(expected, chunk);
        if (fwrite(expected, 1, sizeof(expected), fw) != sizeof(expected)) {
            ESP_LOGE(TAG, "WRITE_FAIL chunk=%u errno=%d", chunk, errno);
            result = ESP_FAIL;
            break;
        }
    }
    // fclose 会提交数据和 FAT 元数据；必须检查它的返回值。
    int close_result = fclose(fw);
    ESP_LOGI(TAG, "WRITE_CLOSE result=%d elapsed_ms=%" PRId64,
             close_result, (esp_timer_get_time() - started) / 1000);
    if (close_result != 0 || result != ESP_OK) return ESP_FAIL;

    errno = 0;
    if (stat(path, &st) != 0 || st.st_size != TEST_BYTES) {
        ESP_LOGE(TAG, "FILE_SIZE_FAIL errno=%d size=%" PRId64,
                 errno, (int64_t)st.st_size);
        return ESP_FAIL;
    }
    FILE *fr = fopen(path, "rb");
    if (!fr) {
        ESP_LOGE(TAG, "REOPEN_FAIL errno=%d", errno);
        return ESP_FAIL;
    }
    for (unsigned chunk = 0; chunk < TEST_BYTES / TEST_CHUNK; ++chunk) {
        fill_pattern(expected, chunk);
        if (fread(actual, 1, sizeof(actual), fr) != sizeof(actual) ||
            memcmp(expected, actual, sizeof(actual)) != 0) {
            ESP_LOGE(TAG, "READBACK_FAIL chunk=%u errno=%d", chunk, errno);
            result = ESP_ERR_INVALID_RESPONSE;
            break;
        }
    }
    if (fclose(fr) != 0) result = ESP_FAIL;
    ESP_LOGI(TAG, "READBACK result=%s bytes=%u path=%s",
             esp_err_to_name(result), TEST_BYTES, path);
    return result;
}

void app_main(void)
{
    // 保持官方示例的 SDSPI_HOST_DEFAULT + esp_vfs_fat_sdspi_mount 路径。
    // 仅补充本板启动 GPIO 和引脚；禁止自动格式化。
    ESP_LOGI(TAG, "SOURCE=ESP-IDF-v5.5.5/examples/storage/sd_card/sdspi file_roundtrip=1 cmd5_ff_ab=0 tx_delay_1tick_ab=1 tick_hz=%d",
             configTICK_RATE_HZ);
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
    ESP_LOGI(TAG, "PWR_IO gpio=%d level=%d", BOARD_PWR_IO, gpio_get_level(BOARD_PWR_IO));

    err = gpio_set_level(TF_POWER, 1);
    if (err != ESP_OK) goto done;
    const gpio_config_t power = {.pin_bit_mask = 1ULL << TF_POWER, .mode = GPIO_MODE_OUTPUT};
    err = gpio_config(&power);
    if (err != ESP_OK) goto done;
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

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.max_freq_khz = 1000;
    host.unaligned_multi_block_rw_max_chunk_size = 8;
    s_native_transaction = host.do_transaction;
    host.do_transaction = trace_card_probe;
    const spi_bus_config_t bus = {
        .mosi_io_num = CONFIG_EXAMPLE_PIN_MOSI,
        .miso_io_num = CONFIG_EXAMPLE_PIN_MISO,
        .sclk_io_num = CONFIG_EXAMPLE_PIN_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4000,
    };
    ESP_LOGI(TAG, "SPI2 clk=%d mosi=%d miso=%d cs=%d max_khz=%d",
             CONFIG_EXAMPLE_PIN_CLK, CONFIG_EXAMPLE_PIN_MOSI,
             CONFIG_EXAMPLE_PIN_MISO, CONFIG_EXAMPLE_PIN_CS, host.max_freq_khz);
    err = spi_bus_initialize(host.slot, &bus, SDSPI_DEFAULT_DMA);
    if (err != ESP_OK) goto done;
    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.gpio_cs = CONFIG_EXAMPLE_PIN_CS;
    slot.host_id = host.slot;
    const esp_vfs_fat_sdmmc_mount_config_t mount = {
        .format_if_mount_failed = false,
        .max_files = 2,
        .allocation_unit_size = 16 * 1024,
    };
    sdmmc_card_t *card = NULL;
    err = esp_vfs_fat_sdspi_mount("/sdcard", &host, &slot, &mount, &card);
    ESP_LOGI(TAG, "MOUNT=%s", esp_err_to_name(err));
    if (err != ESP_OK) {
        diagnose_mount_failure(&host, &slot);
        goto done;
    }
    sdmmc_card_print_info(stdout, card);

    uint32_t first[128], next[128];
    err = sdmmc_read_sectors(card, first, 0, 1);
    if (err != ESP_OK) goto done;
    for (unsigned i = 0; i < 8; ++i) {
        err = sdmmc_read_sectors(card, next, 0, 1);
        if (err != ESP_OK || memcmp(first, next, sizeof(first)) != 0) {
            if (err == ESP_OK) err = ESP_ERR_INVALID_RESPONSE;
            ESP_LOGE(TAG, "READ_REPEAT pass=%u result=%s", i, esp_err_to_name(err));
            goto done;
        }
    }
    ESP_LOGI(TAG, "READ_REPEAT count=8 result=ESP_OK sector0_tail=%02x%02x",
             ((uint8_t *)first)[511], ((uint8_t *)first)[510]);
    // 仅测试期间延长看门狗，不改变 SDSPI Busy 轮询时序。
    const esp_task_wdt_config_t test_wdt = {
        .timeout_ms = 15000,
        .idle_core_mask = (1U << 0) | (1U << 1),
        .trigger_panic = false,
    };
    esp_err_t wdt_err = esp_task_wdt_reconfigure(&test_wdt);
    ESP_LOGI(TAG, "TEST_WDT_15S=%s", esp_err_to_name(wdt_err));
    err = test_file_roundtrip();
    const esp_task_wdt_config_t normal_wdt = {
        .timeout_ms = 5000,
        .idle_core_mask = (1U << 0) | (1U << 1),
        .trigger_panic = false,
    };
    if (wdt_err == ESP_OK) esp_task_wdt_reconfigure(&normal_wdt);
done:
    ESP_LOGI(TAG, "RESULT=%s", esp_err_to_name(err));
}
