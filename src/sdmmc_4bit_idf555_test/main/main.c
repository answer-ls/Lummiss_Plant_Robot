#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/sdmmc_host.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdmmc_cmd.h"

#define SD_POWER_PIN   GPIO_NUM_45  // 低电平开启 Q6
#define SD_CLK_PIN     GPIO_NUM_42
#define SD_CMD_PIN     GPIO_NUM_43
#define SD_D0_PIN      GPIO_NUM_41
#define SD_D1_PIN      GPIO_NUM_40
#define SD_D2_PIN      GPIO_NUM_46
#define SD_D3_PIN      GPIO_NUM_44

#define MOUNT_POINT    "/sdcard"
#define TEST_FILE      MOUNT_POINT "/sdtest.txt"

static const char *TAG = "sd_test";

static esp_err_t sd_power_on(void)
{
    // 先将总线设为输入，避免在卡未上电时驱动信号。
    const gpio_num_t bus_pins[] = {
        SD_CLK_PIN, SD_CMD_PIN,
        SD_D0_PIN, SD_D1_PIN, SD_D2_PIN, SD_D3_PIN,
    };

    for (size_t i = 0; i < sizeof(bus_pins) / sizeof(bus_pins[0]); ++i) {
        esp_err_t err = gpio_reset_pin(bus_pins[i]);
        if (err != ESP_OK) {
            return err;
        }

        err = gpio_set_direction(bus_pins[i], GPIO_MODE_INPUT);
        if (err != ESP_OK) {
            return err;
        }

        err = gpio_set_pull_mode(bus_pins[i], GPIO_FLOATING);
        if (err != ESP_OK) {
            return err;
        }
    }

    esp_err_t err = gpio_reset_pin(SD_POWER_PIN);
    if (err != ESP_OK) {
        return err;
    }

    // 预置低电平，再开启输出。R33 为硬件栅源上拉。
    err = gpio_set_level(SD_POWER_PIN, 0);
    if (err != ESP_OK) {
        return err;
    }

    err = gpio_set_direction(SD_POWER_PIN, GPIO_MODE_OUTPUT);
    if (err != ESP_OK) {
        return err;
    }

    vTaskDelay(pdMS_TO_TICKS(100));

    ESP_LOGI(TAG, "SD power enabled: GPIO45 LOW");
    return ESP_OK;
}

static esp_err_t sd_test_file(void)
{
    static const char test_data[] =
        "ESP32-P4 SDMMC test\n"
        "Bus width: 4 bits\n"
        "CLK=42 CMD=43 D0=41 D1=40 D2=46 D3=44\n"
        "0123456789 ABCDEFGHIJKLMNOPQRSTUVWXYZ\n";

    const size_t expected = sizeof(test_data) - 1;
    char read_buffer[sizeof(test_data)] = {0};

    ESP_LOGI(TAG, "Writing %s", TEST_FILE);

    FILE *file = fopen(TEST_FILE, "wb");
    if (file == NULL) {
        ESP_LOGE(TAG, "Open for writing failed: %s", strerror(errno));
        return ESP_FAIL;
    }

    size_t written = fwrite(test_data, 1, expected, file);
    int write_error = ferror(file);
    int close_result = fclose(file);

    if (write_error || written != expected || close_result != 0) {
        ESP_LOGE(TAG, "Write/close failed: expected=%u, written=%u",
                 (unsigned)expected, (unsigned)written);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Reading %s", TEST_FILE);

    file = fopen(TEST_FILE, "rb");
    if (file == NULL) {
        ESP_LOGE(TAG, "Open for reading failed: %s", strerror(errno));
        return ESP_FAIL;
    }

    // 多读一个字节，也检查文件长度是否符合预期。
    size_t count = fread(read_buffer, 1, sizeof(read_buffer), file);
    int read_error = ferror(file);
    close_result = fclose(file);

    if (read_error || close_result != 0) {
        ESP_LOGE(TAG, "Read/close failed");
        return ESP_FAIL;
    }

    if (count != expected) {
        ESP_LOGE(TAG, "Length mismatch: expected=%u, read=%u",
                 (unsigned)expected, (unsigned)count);
        return ESP_FAIL;
    }

    if (memcmp(read_buffer, test_data, expected) != 0) {
        ESP_LOGE(TAG, "Data verification failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "PASS: %u bytes written and verified",
             (unsigned)expected);
    return ESP_OK;
}

void app_main(void)
{
    ESP_LOGI(TAG, "Starting SD card test");

    esp_err_t err = sd_power_on();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Power setup failed: %s", esp_err_to_name(err));
        return;
    }

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_1;
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;  // 20 MHz

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 4;
    slot.clk = SD_CLK_PIN;
    slot.cmd = SD_CMD_PIN;
    slot.d0 = SD_D0_PIN;
    slot.d1 = SD_D1_PIN;
    slot.d2 = SD_D2_PIN;
    slot.d3 = SD_D3_PIN;

    slot.d4 = GPIO_NUM_NC;
    slot.d5 = GPIO_NUM_NC;
    slot.d6 = GPIO_NUM_NC;
    slot.d7 = GPIO_NUM_NC;

    // GPIO39 是插卡检测，不是 CMD。
    // 本测试不使用 CD/WP，先插卡再上电。
    slot.gpio_cd = GPIO_NUM_NC;
    slot.gpio_wp = GPIO_NUM_NC;

    // 使用原理图 RN1/RN2 的外部上拉。
    const esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 2,
        .allocation_unit_size = 16 * 1024,
    };

    sdmmc_card_t *card = NULL;

    ESP_LOGI(TAG, "Mounting: slot=1, width=4, max clock=20 MHz");
    ESP_LOGI(TAG, "CLK=42 CMD=43 D0=41 D1=40 D2=46 D3=44");

    err = esp_vfs_fat_sdmmc_mount(
        MOUNT_POINT, &host, &slot, &mount_config, &card);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Mount failed: %s (0x%x)",
                 esp_err_to_name(err), (unsigned)err);
        ESP_LOGE(TAG,
                 "Check card power, wiring, external pull-ups, "
                 "SD IO voltage and slot availability.");
        ESP_LOGW(TAG, "Card was NOT formatted.");
        return;
    }

    ESP_LOGI(TAG, "SD card mounted");
    sdmmc_card_print_info(stdout, card);

    esp_err_t test_result = sd_test_file();

    err = esp_vfs_fat_sdcard_unmount(MOUNT_POINT, card);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Unmount failed: %s", esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG, "SD card unmounted");

    if (test_result == ESP_OK) {
        ESP_LOGI(TAG, "SD CARD TEST PASSED");
    } else {
        ESP_LOGE(TAG, "SD CARD TEST FAILED");
    }

    // 保持卡电源开启，避免未处理总线状态就切断电源。
    ESP_LOGI(TAG, "GPIO45 remains LOW; card power remains ON");
}