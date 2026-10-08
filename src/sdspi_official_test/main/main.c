#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "driver/gpio.h"
#include "driver/sdspi_host.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdmmc_cmd.h"

#define PWR_HOLD GPIO_NUM_8
#define TF_POWER GPIO_NUM_45
#define TF_CLK GPIO_NUM_42
#define TF_MOSI GPIO_NUM_43
#define TF_MISO GPIO_NUM_41
#define TF_CS GPIO_NUM_44
#define TF_CD GPIO_NUM_39
#define EXPRESSION_PATH "/sdcard/expressions/exp_08.bin"
#define EXPRESSION_READ_CHUNK 4096U

static const char *TAG = "SDSPI_OFFICIAL";

static esp_err_t read_expression_file(int attempt, uint32_t *hash_out)
{
    struct stat info;
    ESP_LOGI(TAG, "ATTEMPT=%d EXPRESSION_STAT_BEGIN path=%s", attempt, EXPRESSION_PATH);
    if (stat(EXPRESSION_PATH, &info) != 0) {
        ESP_LOGE(TAG, "ATTEMPT=%d EXPRESSION_STAT path=%s errno=%d (%s)",
                 attempt, EXPRESSION_PATH, errno, strerror(errno));
        return ESP_FAIL;
    }
    if (info.st_size <= 0) {
        ESP_LOGE(TAG, "ATTEMPT=%d EXPRESSION_SIZE=%ld", attempt, (long)info.st_size);
        return ESP_ERR_INVALID_SIZE;
    }
    ESP_LOGI(TAG, "ATTEMPT=%d EXPRESSION_STAT_OK size=%u", attempt, (unsigned)info.st_size);

    ESP_LOGI(TAG, "ATTEMPT=%d EXPRESSION_OPEN_BEGIN", attempt);
    FILE *file = fopen(EXPRESSION_PATH, "rb");
    if (file == NULL) {
        ESP_LOGE(TAG, "ATTEMPT=%d EXPRESSION_OPEN errno=%d (%s)",
                 attempt, errno, strerror(errno));
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "ATTEMPT=%d EXPRESSION_OPEN_OK", attempt);
    uint8_t *buffer = malloc(EXPRESSION_READ_CHUNK);
    if (buffer == NULL) {
        fclose(file);
        return ESP_ERR_NO_MEM;
    }

    /* 与主工程一样分块读完整个表情文件；校验长度并记录内容指纹。 */
    size_t total = 0;
    size_t last_progress = 0;
    uint32_t hash = 2166136261U;
    int read_errno = 0;
    while (total < (size_t)info.st_size) {
        const size_t remaining = (size_t)info.st_size - total;
        const size_t chunk = remaining < EXPRESSION_READ_CHUNK ? remaining : EXPRESSION_READ_CHUNK;
        const size_t received = fread(buffer, 1, chunk, file);
        for (size_t i = 0; i < received; ++i) hash = (hash ^ buffer[i]) * 16777619U;
        total += received;
        if (total - last_progress >= 64U * 1024U) {
            ESP_LOGI(TAG, "ATTEMPT=%d EXPRESSION_PROGRESS bytes=%u/%u", attempt,
                     (unsigned)total, (unsigned)info.st_size);
            last_progress = total;
        }
        if (received != chunk) {
            read_errno = errno;
            break;
        }
    }
    const bool read_failed = ferror(file) != 0;
    free(buffer);
    const int close_result = fclose(file);
    const bool complete = total == (size_t)info.st_size && !read_failed && close_result == 0;
    ESP_LOGI(TAG, "ATTEMPT=%d EXPRESSION_READ=%s bytes=%u/%u hash=0x%08lx errno=%d",
             attempt, complete ? "ESP_OK" : "ESP_FAIL", (unsigned)total,
             (unsigned)info.st_size, (unsigned long)hash, read_errno);
    if (complete && hash_out != NULL) *hash_out = hash;
    return complete ? ESP_OK : ESP_FAIL;
}

static esp_err_t run_attempt(int attempt, bool *delayed_read_done)
{
    /* 每轮都让卡断电；本轮对照仅调整 SPI2 初始化与上电的先后。 */
    ESP_ERROR_CHECK(gpio_set_level(TF_POWER, 1));
    vTaskDelay(pdMS_TO_TICKS(100));

    /* 驱动与挂载 API 仍使用 ESP-IDF 官方示例的 SDSPI 路径。 */
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI2_HOST;
    host.max_freq_khz = 1000;
    spi_bus_config_t bus = {
        .mosi_io_num = TF_MOSI, .miso_io_num = TF_MISO, .sclk_io_num = TF_CLK,
        .quadwp_io_num = -1, .quadhd_io_num = -1, .max_transfer_sz = 4096,
    };
    esp_err_t err = spi_bus_initialize(host.slot, &bus, SDSPI_DEFAULT_DMA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPI_BUS_INIT=%s", esp_err_to_name(err));
        goto power_off;
    }
    ESP_LOGI(TAG, "ATTEMPT=%d SPI_BUS_INIT=ESP_OK power=OFF", attempt);
    ESP_ERROR_CHECK(gpio_set_level(TF_POWER, 0));
    vTaskDelay(pdMS_TO_TICKS(200));
    ESP_LOGI(TAG, "ATTEMPT=%d OFFICIAL_IDF_DRIVER power=ON GPIO45=%d CD_GPIO39=%d",
             attempt, gpio_get_level(TF_POWER), gpio_get_level(TF_CD));

    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.host_id = host.slot;
    slot.gpio_cs = TF_CS;
    const esp_vfs_fat_sdmmc_mount_config_t mount = {
        .format_if_mount_failed = false, .max_files = 2,
        .allocation_unit_size = 16 * 1024,
    };
    sdmmc_card_t *card = NULL;
    ESP_LOGI(TAG, "ATTEMPT=%d MOUNT_BEGIN SPI2 CLK=42 MOSI=43 MISO=41 CS=44 max_khz=1000", attempt);
    err = esp_vfs_fat_sdspi_mount("/sdcard", &host, &slot, &mount, &card);
    ESP_LOGI(TAG, "ATTEMPT=%d MOUNT=%s", attempt, esp_err_to_name(err));
    if (err == ESP_OK) {
        sdmmc_card_print_info(stdout, card);
        uint8_t first[512], second[512];
        err = sdmmc_read_sectors(card, first, 0, 1);
        if (err == ESP_OK) err = sdmmc_read_sectors(card, second, 0, 1);
        if (err == ESP_OK && memcmp(first, second, sizeof(first)) != 0) {
            err = ESP_ERR_INVALID_RESPONSE;
        }
        ESP_LOGI(TAG, "READ_SECTOR0_TWICE=%s", esp_err_to_name(err));
        uint32_t first_hash = 0;
        if (err == ESP_OK) err = read_expression_file(attempt, &first_hash);
        if (err == ESP_OK && !*delayed_read_done) {
            *delayed_read_done = true;
            /* 保持卡供电和文件系统挂载，空闲约 90 秒后重新完整读取。 */
            ESP_LOGI(TAG, "ATTEMPT=%d HOLD_MOUNT_BEGIN wait_ms=90000 GPIO45=%d",
                     attempt, gpio_get_level(TF_POWER));
            vTaskDelay(pdMS_TO_TICKS(90000));
            ESP_LOGI(TAG, "ATTEMPT=%d HOLD_MOUNT_END GPIO45=%d CD_GPIO39=%d",
                     attempt, gpio_get_level(TF_POWER), gpio_get_level(TF_CD));
            uint32_t delayed_hash = 0;
            err = read_expression_file(attempt, &delayed_hash);
            if (err == ESP_OK && delayed_hash != first_hash) {
                err = ESP_ERR_INVALID_RESPONSE;
            }
            ESP_LOGI(TAG, "ATTEMPT=%d DELAYED_READ_RESULT=%s first_hash=0x%08lx delayed_hash=0x%08lx",
                     attempt, esp_err_to_name(err), (unsigned long)first_hash,
                     (unsigned long)delayed_hash);
        }
        esp_err_t unmount_err = esp_vfs_fat_sdcard_unmount("/sdcard", card);
        ESP_LOGI(TAG, "UNMOUNT=%s", esp_err_to_name(unmount_err));
        if (err == ESP_OK) err = unmount_err;
    }
    esp_err_t free_err = spi_bus_free(host.slot);
    ESP_LOGI(TAG, "SPI_BUS_FREE=%s", esp_err_to_name(free_err));
    if (err == ESP_OK) err = free_err;

power_off:
    ESP_ERROR_CHECK(gpio_set_level(TF_POWER, 1));
    ESP_LOGI(TAG, "ATTEMPT=%d RESULT=%s TF_POWER=OFF", attempt, esp_err_to_name(err));
    return err;
}

void app_main(void)
{
    /* 仅保留本 PCB 必需的保持供电、TF 上电和卡检测引脚配置。 */
    ESP_ERROR_CHECK(gpio_set_level(PWR_HOLD, 1));
    gpio_config_t hold = {.pin_bit_mask = 1ULL << PWR_HOLD, .mode = GPIO_MODE_INPUT_OUTPUT};
    ESP_ERROR_CHECK(gpio_config(&hold));
    ESP_ERROR_CHECK(gpio_set_level(PWR_HOLD, 1));

    ESP_ERROR_CHECK(gpio_set_level(TF_POWER, 1));
    gpio_config_t power = {.pin_bit_mask = 1ULL << TF_POWER, .mode = GPIO_MODE_INPUT_OUTPUT};
    ESP_ERROR_CHECK(gpio_config(&power));
    gpio_config_t cd = {.pin_bit_mask = 1ULL << TF_CD, .mode = GPIO_MODE_INPUT};
    ESP_ERROR_CHECK(gpio_config(&cd));

    esp_err_t results[3];
    bool delayed_read_done = false;
    for (int i = 0; i < 3; ++i) results[i] = run_attempt(i + 1, &delayed_read_done);
    ESP_LOGI(TAG, "SUMMARY F1=%s F2=%s F3=%s",
             esp_err_to_name(results[0]), esp_err_to_name(results[1]),
             esp_err_to_name(results[2]));
}
