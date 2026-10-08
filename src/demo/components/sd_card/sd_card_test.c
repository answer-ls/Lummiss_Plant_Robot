#include "sd_card.h"
#include "board_pins.h"
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/sdspi_host.h"
#include "driver/spi_master.h"
#include "driver/sdmmc_host.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

#define TEST_BYTES 16384
static const char *TAG = "SD_TEST";
static esp_err_t check_write_error(const char *stage, esp_err_t result);

#if BOARD_USE_NEW_PCB
static esp_err_t (*s_reference_transaction)(int, sdmmc_command_t *);

static esp_err_t reference_trace_transaction(int slot, sdmmc_command_t *cmd)
{
    esp_err_t err = s_reference_transaction(slot, cmd);
    if (cmd->opcode == 0 || cmd->opcode == 5 || cmd->opcode == 8 ||
        cmd->opcode == 9 || cmd->opcode == 10 || cmd->opcode == 58 ||
        err != ESP_OK || cmd->error != ESP_OK) {
        ESP_LOGI(TAG, "CMD%lu ret=%s cmd_error=%s response0=0x%08lx",
                 (unsigned long)cmd->opcode, esp_err_to_name(err),
                 esp_err_to_name(cmd->error), (unsigned long)cmd->response[0]);
    }
    return err;
}

static esp_err_t reference_set_power(bool on)
{
    esp_err_t err = gpio_set_level(BOARD_TF_POWER, on ? 0 : 1);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "TF_POWER GPIO45=%d state=%s t_us=%lld CD_GPIO39=%d",
                 on ? 0 : 1, on ? "ON" : "OFF", (long long)esp_timer_get_time(),
                 gpio_get_level(BOARD_TF_CD));
    }
    return err;
}

static esp_err_t reference_float_bus(void)
{
    const gpio_num_t pins[] = {
        BOARD_TF_CLK, BOARD_TF_MOSI, BOARD_TF_MISO,
        BOARD_TF_D1, BOARD_TF_D2, BOARD_TF_D3,
    };
    for (size_t i = 0; i < sizeof(pins) / sizeof(pins[0]); ++i) {
        esp_err_t err = gpio_reset_pin(pins[i]);
        if (err != ESP_OK) return err;
        err = gpio_set_direction(pins[i], GPIO_MODE_INPUT);
        if (err != ESP_OK) return err;
        err = gpio_set_pull_mode(pins[i], GPIO_FLOATING);
        if (err != ESP_OK) return err;
    }
    return ESP_OK;
}

static esp_err_t reference_run_attempt(const char *name)
{
    ESP_LOGI(TAG, "ATTEMPT %s bus_before_power=1", name);
    esp_err_t err = reference_float_bus();
    ESP_LOGI(TAG, "%s BUS_PINS_FLOAT=%s", name, esp_err_to_name(err));
    if (err != ESP_OK) return err;
    err = reference_set_power(false);
    if (err != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(100));

    const spi_bus_config_t bus = {
        .mosi_io_num = BOARD_TF_MOSI, .miso_io_num = BOARD_TF_MISO,
        .sclk_io_num = BOARD_TF_CLK, .quadwp_io_num = -1,
        .quadhd_io_num = -1, .max_transfer_sz = 4096,
    };
    err = spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO);
    ESP_LOGI(TAG, "%s SPI_BUS_INIT=%s t_us=%lld", name,
             esp_err_to_name(err), (long long)esp_timer_get_time());
    if (err != ESP_OK) return err;
    err = reference_set_power(true);
    if (err == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(200));
        ESP_LOGI(TAG, "%s POWER_SETTLED delay_ms=200 t_us=%lld CD_GPIO39=%d", name,
                 (long long)esp_timer_get_time(), gpio_get_level(BOARD_TF_CD));

        sdmmc_host_t host = SDSPI_HOST_DEFAULT();
        host.slot = SPI2_HOST;
        host.max_freq_khz = 1000;
        s_reference_transaction = host.do_transaction;
        host.do_transaction = reference_trace_transaction;
        sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
        slot.host_id = SPI2_HOST;
        slot.gpio_cs = BOARD_TF_CS;
        const esp_vfs_fat_sdmmc_mount_config_t mount = {
            .format_if_mount_failed = false, .max_files = 2,
            .allocation_unit_size = 16 * 1024,
        };
        sdmmc_card_t *card = NULL;
        int64_t mount_begin = esp_timer_get_time();
        ESP_LOGI(TAG, "%s MOUNT_BEGIN t_us=%lld SPI2 CLK=%d MOSI=%d MISO=%d CS=%d max_khz=1000",
                 name, (long long)mount_begin, BOARD_TF_CLK, BOARD_TF_MOSI,
                 BOARD_TF_MISO, BOARD_TF_CS);
        err = esp_vfs_fat_sdspi_mount(SD_CARD_MOUNT_POINT, &host, &slot, &mount, &card);
        ESP_LOGI(TAG, "%s MOUNT=%s elapsed_us=%lld", name, esp_err_to_name(err),
                 (long long)(esp_timer_get_time() - mount_begin));
        if (err == ESP_OK) {
            sdmmc_card_print_info(stdout, card);
            uint8_t first[512], second[512];
            err = sdmmc_read_sectors(card, first, 0, 1);
            ESP_LOGI(TAG, "%s READ0_FIRST=%s", name, esp_err_to_name(err));
            if (err == ESP_OK) err = sdmmc_read_sectors(card, second, 0, 1);
            ESP_LOGI(TAG, "%s READ0_SECOND=%s identical=%d", name,
                     esp_err_to_name(err), err == ESP_OK &&
                     memcmp(first, second, sizeof(first)) == 0);
            if (err == ESP_OK && memcmp(first, second, sizeof(first)) != 0) {
                err = ESP_ERR_INVALID_RESPONSE;
            }
            esp_err_t unmount_err = esp_vfs_fat_sdcard_unmount(SD_CARD_MOUNT_POINT, card);
            ESP_LOGI(TAG, "%s UNMOUNT=%s", name, esp_err_to_name(unmount_err));
            if (err == ESP_OK) err = unmount_err;
        }
    }

    esp_err_t free_err = spi_bus_free(SPI2_HOST);
    ESP_LOGI(TAG, "%s SPI_BUS_FREE=%s", name, esp_err_to_name(free_err));
    if (err == ESP_OK) err = free_err;
    const gpio_num_t reset_pins[] = {
        BOARD_TF_D3, BOARD_TF_CLK, BOARD_TF_MOSI, BOARD_TF_MISO,
    };
    esp_err_t reset_status = ESP_OK;
    for (size_t i = 0; i < sizeof(reset_pins) / sizeof(reset_pins[0]); ++i) {
        esp_err_t reset_err = gpio_reset_pin(reset_pins[i]);
        if (reset_status == ESP_OK) reset_status = reset_err;
        if (err == ESP_OK) err = reset_err;
    }
    ESP_LOGI(TAG, "%s BUS_PINS_RESET=%s", name, esp_err_to_name(reset_status));
    esp_err_t off_err = reference_set_power(false);
    if (err == ESP_OK) err = off_err;
    ESP_LOGI(TAG, "ATTEMPT %s RESULT=%s", name, esp_err_to_name(err));
    return err;
}
#endif

esp_err_t sd_card_sdspi_reference_test(void)
{
#if !BOARD_USE_NEW_PCB
    return ESP_ERR_NOT_SUPPORTED;
#else
    /* 复现 sdspi_power_order_test 的浮空、断电、SPI2、上电及完整清理顺序。 */
    esp_err_t err = gpio_set_level(BOARD_TF_POWER, 1);
    if (err != ESP_OK) return err;
    gpio_config_t power = {
        .pin_bit_mask = 1ULL << BOARD_TF_POWER,
        .mode = GPIO_MODE_INPUT_OUTPUT,
    };
    err = gpio_config(&power);
    if (err != ESP_OK) return err;
    gpio_config_t cd = {
        .pin_bit_mask = 1ULL << BOARD_TF_CD,
        .mode = GPIO_MODE_INPUT,
    };
    err = gpio_config(&cd);
    if (err != ESP_OK) return err;

    ESP_LOGI(TAG, "BOOT PWR_IO GPIO8=%d TF_POWER GPIO45=%d CD GPIO39=%d",
             gpio_get_level(BOARD_PWR_IO), gpio_get_level(BOARD_TF_POWER),
             gpio_get_level(BOARD_TF_CD));
    esp_err_t first = reference_run_attempt("F1_COLD");
    esp_err_t second = reference_run_attempt("F2_RETRY");
    esp_err_t third = reference_run_attempt("F3_RETRY");
    ESP_LOGI(TAG, "SUMMARY F1=%s F2=%s F3=%s", esp_err_to_name(first),
             esp_err_to_name(second), esp_err_to_name(third));
    return first == ESP_OK || second == ESP_OK || third == ESP_OK
               ? ESP_OK : third;
#endif
}

esp_err_t sd_card_sdmmc_4bit_test(void)
{
#if !BOARD_USE_NEW_PCB
    return ESP_ERR_NOT_SUPPORTED;
#else
    const gpio_num_t bus_pins[] = {
        BOARD_TF_CLK, BOARD_TF_MOSI, BOARD_TF_MISO,
        BOARD_TF_D1, BOARD_TF_D2, BOARD_TF_D3,
    };
    for (size_t i = 0; i < sizeof(bus_pins) / sizeof(bus_pins[0]); ++i) {
        esp_err_t err = gpio_reset_pin(bus_pins[i]);
        if (err != ESP_OK) return err;
        err = gpio_set_direction(bus_pins[i], GPIO_MODE_INPUT);
        if (err != ESP_OK) return err;
        err = gpio_set_pull_mode(bus_pins[i], GPIO_FLOATING);
        if (err != ESP_OK) return err;
    }
    esp_err_t err = gpio_reset_pin(BOARD_TF_POWER);
    if (err != ESP_OK) return err;
    err = gpio_set_level(BOARD_TF_POWER, BOARD_TF_POWER_ON_LEVEL);
    if (err != ESP_OK) return err;
    err = gpio_set_direction(BOARD_TF_POWER, GPIO_MODE_OUTPUT);
    if (err != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(100));

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_1;
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 4;
    slot.clk = BOARD_TF_CLK;
    slot.cmd = BOARD_TF_MOSI;
    slot.d0 = BOARD_TF_MISO;
    slot.d1 = BOARD_TF_D1;
    slot.d2 = BOARD_TF_D2;
    slot.d3 = BOARD_TF_D3;
    slot.d4 = slot.d5 = slot.d6 = slot.d7 = GPIO_NUM_NC;
    slot.gpio_cd = GPIO_NUM_NC;
    slot.gpio_wp = GPIO_NUM_NC;
    const esp_vfs_fat_sdmmc_mount_config_t mount = {
        .format_if_mount_failed = false,
        .max_files = 2,
        .allocation_unit_size = 16 * 1024,
    };
    sdmmc_card_t *card = NULL;
    ESP_LOGI(TAG, "SDMMC_4BIT begin slot=1 CLK=%d CMD=%d D0=%d D1=%d D2=%d D3=%d power=%d clock_khz=%d",
             BOARD_TF_CLK, BOARD_TF_MOSI, BOARD_TF_MISO, BOARD_TF_D1,
             BOARD_TF_D2, BOARD_TF_D3, BOARD_TF_POWER, host.max_freq_khz);
    err = esp_vfs_fat_sdmmc_mount(SD_CARD_MOUNT_POINT, &host, &slot, &mount, &card);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SDMMC_4BIT mount=%s", esp_err_to_name(err));
        return err;
    }
    sdmmc_card_print_info(stdout, card);

    /* 使用独立文件名，避免覆盖卡上原有数据；测试文件保留供电脑检查。 */
    static const char payload[] = "ESP32-P4 SDMMC 4-bit test\n";
    char path[64];
    struct stat st;
    bool found = false;
    for (unsigned i = 0; i < 1000; ++i) {
        snprintf(path, sizeof(path), SD_CARD_MOUNT_POINT "/SD4%03u.TXT", i);
        errno = 0;
        if (stat(path, &st) != 0 && errno == ENOENT) {
            found = true;
            break;
        }
    }
    err = found ? ESP_OK : ESP_ERR_NOT_FOUND;
    FILE *file = NULL;
    if (err == ESP_OK) {
        file = fopen(path, "wb");
        if (!file) err = ESP_FAIL;
    }
    if (err == ESP_OK) {
        size_t written = fwrite(payload, 1, sizeof(payload) - 1, file);
        int close_result = fclose(file);
        file = NULL;
        if (written != sizeof(payload) - 1 || close_result != 0) err = ESP_FAIL;
    }
    if (err == ESP_OK) {
        file = fopen(path, "rb");
        if (!file) err = ESP_FAIL;
    }
    if (err == ESP_OK) {
        char readback[sizeof(payload)] = {0};
        size_t count = fread(readback, 1, sizeof(readback), file);
        int read_error = ferror(file);
        int close_result = fclose(file);
        file = NULL;
        if (read_error || close_result != 0 || count != sizeof(payload) - 1 ||
            memcmp(readback, payload, sizeof(payload) - 1) != 0) {
            err = ESP_ERR_INVALID_RESPONSE;
        }
    }
    if (file) fclose(file);
    ESP_LOGI(TAG, "SDMMC_4BIT file=%s bytes=%u result=%s",
             found ? path : "NONE", (unsigned)(sizeof(payload) - 1), esp_err_to_name(err));
    esp_err_t unmount_err = esp_vfs_fat_sdcard_unmount(SD_CARD_MOUNT_POINT, card);
    if (err == ESP_OK) err = unmount_err;
    ESP_LOGI(TAG, "SDMMC_4BIT unmount=%s final=%s",
             esp_err_to_name(unmount_err), esp_err_to_name(err));
    return err;
#endif
}

esp_err_t sd_card_official_style_test(void)
{
    // 与 ESP-IDF SDSPI 示例保持相同的文件 API 顺序：挂载→打开写→关闭→重开读。
    // 使用唯一的 8.3 文件名，避免示例中 hello.txt/foo.txt 覆盖已有文件。
    static const char payload[] = "ESP-IDF SDSPI file write check\n";
    esp_err_t result = sd_card_mount();
    if (result != ESP_OK) return result;

    char path[64] = {0};
    struct stat st;
    bool found = false;
    for (unsigned i = 0; i < 1000; ++i) {
        snprintf(path, sizeof(path), SD_CARD_MOUNT_POINT "/SDF%03u.TXT", i);
        errno = 0;
        if (stat(path, &st) != 0 && errno == ENOENT) {
            found = true;
            break;
        }
    }
    if (!found) {
        ESP_LOGE(TAG, "OFFICIAL_STYLE no free test filename");
        result = ESP_ERR_NOT_FOUND;
        goto done;
    }

    ESP_LOGI(TAG, "OFFICIAL_STYLE WRITE_OPEN path=%s bytes=%u", path,
             (unsigned)(sizeof(payload) - 1));
    FILE *file = fopen(path, "wb");
    if (!file) {
        ESP_LOGE(TAG, "OFFICIAL_STYLE WRITE_OPEN failed errno=%d", errno);
        result = ESP_FAIL;
        goto done;
    }
    size_t written = fwrite(payload, 1, sizeof(payload) - 1, file);
    int close_result = fclose(file);
    ESP_LOGI(TAG, "OFFICIAL_STYLE WRITE_CLOSE written=%u close=%d low_level=%s",
             (unsigned)written, close_result, esp_err_to_name(sd_card_get_write_error()));
    if (written != sizeof(payload) - 1 || close_result != 0) result = ESP_FAIL;
    result = check_write_error("OFFICIAL_STYLE_WRITE_CLOSE", result);
    if (result != ESP_OK) goto done;

    file = fopen(path, "rb");
    if (!file) {
        ESP_LOGE(TAG, "OFFICIAL_STYLE READ_OPEN failed errno=%d", errno);
        result = ESP_FAIL;
        goto done;
    }
    char actual[sizeof(payload)] = {0};
    size_t read_len = fread(actual, 1, sizeof(payload) - 1, file);
    int trailing = fgetc(file);
    bool read_error = ferror(file);
    close_result = fclose(file);
    bool match = read_len == sizeof(payload) - 1 && trailing == EOF &&
                 !read_error && close_result == 0 &&
                 memcmp(actual, payload, sizeof(payload) - 1) == 0;
    ESP_LOGI(TAG, "OFFICIAL_STYLE READBACK read=%u match=%d close=%d",
             (unsigned)read_len, match, close_result);
    if (!match) result = ESP_ERR_INVALID_RESPONSE;

done:
    // 成功后保留测试文件供电脑核对；失败时也不删除或格式化卡上数据。
    esp_err_t cleanup = sd_card_unmount();
    if (result == ESP_OK) result = cleanup;
    ESP_LOGI(TAG, "OFFICIAL_STYLE RESULT=%s path=%s", esp_err_to_name(result), path);
    return result;
}

static esp_err_t check_write_error(const char *stage, esp_err_t result)
{
    esp_err_t low_level = sd_card_get_write_error();
    if (low_level != ESP_OK) {
        ESP_LOGE(TAG, "%s: low-level write failed: %s (even if filesystem API succeeded)",
                 stage, esp_err_to_name(low_level));
        return low_level;
    }
    return result;
}

static esp_err_t verify_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return ESP_FAIL;
    unsigned char data[256];
    bool ok = true;
    for (unsigned offset = 0; offset < TEST_BYTES && ok; offset += sizeof(data)) {
        if (fread(data, 1, sizeof(data), f) != sizeof(data)) { ok = false; break; }
        for (unsigned i = 0; i < sizeof(data); ++i) {
            if (data[i] != (unsigned char)(((offset + i) * 37U) ^ ((offset + i) >> 8))) {
                ESP_LOGE(TAG, "Mismatch at byte %u", offset + i);
                ok = false;
                break;
            }
        }
    }
    if (ok && (fgetc(f) != EOF || ferror(f))) ok = false;
    if (fclose(f) != 0) ok = false;
    return ok ? ESP_OK : ESP_FAIL;
}

esp_err_t sd_card_self_test(void)
{
    esp_err_t err = sd_card_mount();
    if (err != ESP_OK) return err;
    char path[64];
    int fd = -1;
    /* Exclusive creation preserves all pre-existing files, including old tests. */
    for (unsigned i = 0; i < 1000; ++i) {
        snprintf(path, sizeof(path), SD_CARD_MOUNT_POINT "/SDT%03u.BIN", i);
        fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (fd >= 0 || errno != EEXIST) break;
        vTaskDelay(1);
    }
    if (fd < 0) {
        ESP_LOGE(TAG, "Create test file failed, errno=%d", errno);
        sd_card_unmount();
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Write %d bytes: %s", TEST_BYTES, path);
    unsigned char data[256];
    for (unsigned offset = 0; offset < TEST_BYTES; offset += sizeof(data)) {
        for (unsigned i = 0; i < sizeof(data); ++i)
            data[i] = (unsigned char)(((offset + i) * 37U) ^ ((offset + i) >> 8));
        ssize_t written = write(fd, data, sizeof(data));
        if (written != sizeof(data) || sd_card_get_write_error() != ESP_OK) {
            ESP_LOGE(TAG, "WRITE failed: offset=%u returned=%d errno=%d",
                     offset, (int)written, errno);
            err = ESP_FAIL;
            break;
        }
        vTaskDelay(1);
    }
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "FSYNC begin");
        if (fsync(fd) != 0) {
            ESP_LOGE(TAG, "FSYNC failed: errno=%d", errno);
            err = ESP_FAIL;
        }
        err = check_write_error("FSYNC", err);
    }
    vTaskDelay(1);
    /* close is required even after failure; FatFs may still attempt a flush. */
    if (close(fd) != 0) {
        ESP_LOGE(TAG, "CLOSE failed: errno=%d", errno);
        err = ESP_FAIL;
    }
    err = check_write_error("CLOSE", err);
    vTaskDelay(1);
    if (err == ESP_OK) err = verify_file(path);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Initial readback PASS; unmount/remount verification");
        err = sd_card_unmount();
        if (err == ESP_OK) err = sd_card_mount();
        if (err == ESP_OK) err = verify_file(path);
    }
    if (sd_card_is_mounted()) {
        if (err == ESP_OK) {
            if (unlink(path) != 0) err = ESP_FAIL;
            err = check_write_error("CLEANUP", err);
        } else {
            ESP_LOGW(TAG, "Skip deletion after IO failure; test file may remain: %s", path);
        }
        esp_err_t cleanup = sd_card_unmount();
        if (err == ESP_OK) err = cleanup;
    } else {
        ESP_LOGW(TAG, "Card unavailable; test file may remain: %s", path);
    }
    ESP_LOGI(TAG, "write/read/remount/read/cleanup: %s", err == ESP_OK ? "PASS" : "FAIL");
    return err;
}
