#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/sdspi_host.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdmmc_cmd.h"

#define PWR_HOLD GPIO_NUM_8
#define TF_POWER GPIO_NUM_45
#define TF_CLK GPIO_NUM_42
#define TF_CMD GPIO_NUM_43
#define TF_D0 GPIO_NUM_41
#define TF_D1 GPIO_NUM_40
#define TF_D2 GPIO_NUM_46
#define TF_D3 GPIO_NUM_44
#define TF_CD GPIO_NUM_39
#define TF_POWER_SETTLE_MS 200

static const char *TAG = "SDSPI_POWER";
static esp_err_t (*s_native_transaction)(int, sdmmc_command_t *);

static esp_err_t trace_transaction(int slot, sdmmc_command_t *cmd)
{
    esp_err_t err = s_native_transaction(slot, cmd);
    if (cmd->opcode == 0 || cmd->opcode == 5 || cmd->opcode == 8 ||
        cmd->opcode == 9 || cmd->opcode == 10 || cmd->opcode == 58 ||
        err != ESP_OK || cmd->error != ESP_OK) {
        ESP_LOGI(TAG, "CMD%lu ret=%s cmd_error=%s response0=0x%08lx",
                 (unsigned long)cmd->opcode, esp_err_to_name(err),
                 esp_err_to_name(cmd->error), (unsigned long)cmd->response[0]);
    }
    return err;
}

static esp_err_t set_power(bool on)
{
    esp_err_t err = gpio_set_level(TF_POWER, on ? 0 : 1);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "TF_POWER GPIO45=%d state=%s t_us=%lld CD_GPIO39=%d",
                 on ? 0 : 1, on ? "ON" : "OFF", (long long)esp_timer_get_time(),
                 gpio_get_level(TF_CD));
    }
    return err;
}

static esp_err_t check_readonly(sdmmc_card_t *card, const char *name)
{
    uint8_t first[512], second[512];
    esp_err_t err = sdmmc_read_sectors(card, first, 0, 1);
    ESP_LOGI(TAG, "%s READ0_FIRST=%s", name, esp_err_to_name(err));
    if (err != ESP_OK) return err;
    err = sdmmc_read_sectors(card, second, 0, 1);
    ESP_LOGI(TAG, "%s READ0_SECOND=%s identical=%d", name, esp_err_to_name(err),
             err == ESP_OK && memcmp(first, second, sizeof(first)) == 0);
    if (err == ESP_OK && memcmp(first, second, sizeof(first)) != 0) {
        err = ESP_ERR_INVALID_RESPONSE;
    }
    return err;
}

static esp_err_t run_attempt(const char *name, bool bus_before_power)
{
    ESP_LOGI(TAG, "ATTEMPT %s bus_before_power=%d", name, bus_before_power);
    /* 对照主工程：上电前将 TF 的六根总线线脚设为浮空输入。 */
    const gpio_num_t float_pins[] = {TF_CLK, TF_CMD, TF_D0, TF_D1, TF_D2, TF_D3};
    for (size_t i = 0; i < sizeof(float_pins) / sizeof(float_pins[0]); ++i) {
        esp_err_t pin_err = gpio_reset_pin(float_pins[i]);
        if (pin_err != ESP_OK) return pin_err;
        pin_err = gpio_set_direction(float_pins[i], GPIO_MODE_INPUT);
        if (pin_err != ESP_OK) return pin_err;
        pin_err = gpio_set_pull_mode(float_pins[i], GPIO_FLOATING);
        if (pin_err != ESP_OK) return pin_err;
    }           
    ESP_LOGI(TAG, "%s BUS_PINS_FLOAT=ESP_OK", name);
    esp_err_t err = set_power(false);
    if (err != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(100));

    const spi_bus_config_t bus = {
        .mosi_io_num = TF_CMD, .miso_io_num = TF_D0, .sclk_io_num = TF_CLK,
        .quadwp_io_num = -1, .quadhd_io_num = -1, .max_transfer_sz = 4096,
    };
    bool bus_ready = false;
    if (bus_before_power) {
        err = spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO);
        ESP_LOGI(TAG, "%s SPI_BUS_INIT=%s t_us=%lld", name,
                 esp_err_to_name(err), (long long)esp_timer_get_time());
        if (err != ESP_OK) return err;
        bus_ready = true;
    }
    err = set_power(true);
    if (err != ESP_OK) goto cleanup;
    vTaskDelay(pdMS_TO_TICKS(TF_POWER_SETTLE_MS));
    ESP_LOGI(TAG, "%s POWER_SETTLED delay_ms=%d t_us=%lld CD_GPIO39=%d", name,
             TF_POWER_SETTLE_MS, (long long)esp_timer_get_time(), gpio_get_level(TF_CD));
    if (!bus_ready) {
        err = spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO);
        ESP_LOGI(TAG, "%s SPI_BUS_INIT=%s t_us=%lld", name,
                 esp_err_to_name(err), (long long)esp_timer_get_time());
        if (err != ESP_OK) goto cleanup;
        bus_ready = true;
    }

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI2_HOST;
    host.max_freq_khz = 1000;
    s_native_transaction = host.do_transaction;
    host.do_transaction = trace_transaction;
    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.host_id = SPI2_HOST;
    slot.gpio_cs = TF_D3;
    const esp_vfs_fat_sdmmc_mount_config_t mount = {
        .format_if_mount_failed = false, .max_files = 2,
        .allocation_unit_size = 16 * 1024,
    };
    sdmmc_card_t *card = NULL;
    int64_t mount_begin = esp_timer_get_time();
    ESP_LOGI(TAG, "%s MOUNT_BEGIN t_us=%lld SPI2 CLK=42 MOSI=43 MISO=41 CS=44 max_khz=1000",
             name, (long long)mount_begin);
    err = esp_vfs_fat_sdspi_mount("/sdcard", &host, &slot, &mount, &card);
    ESP_LOGI(TAG, "%s MOUNT=%s elapsed_us=%lld", name, esp_err_to_name(err),
             (long long)(esp_timer_get_time() - mount_begin));
    if (err == ESP_OK) {
        sdmmc_card_print_info(stdout, card);
        ESP_LOGI(TAG, "%s CARD name=%.5s sectors=%lu sector_size=%lu cid_serial=0x%08lx",
                 name, card->cid.name, (unsigned long)card->csd.capacity,
                 (unsigned long)card->csd.sector_size, (unsigned long)card->cid.serial);
        err = check_readonly(card, name);
        esp_err_t unmount_err = esp_vfs_fat_sdcard_unmount("/sdcard", card);
        ESP_LOGI(TAG, "%s UNMOUNT=%s", name, esp_err_to_name(unmount_err));
        if (err == ESP_OK) err = unmount_err;
    }

cleanup:
    if (bus_ready) {
        esp_err_t free_err = spi_bus_free(SPI2_HOST);
        ESP_LOGI(TAG, "%s SPI_BUS_FREE=%s", name, esp_err_to_name(free_err));
        if (err == ESP_OK) err = free_err;
    }
    /* 对照主工程：释放 SPI2 后复位四根 SPI 线，再进行下一轮。 */
    const gpio_num_t reset_pins[] = {TF_D3, TF_CLK, TF_CMD, TF_D0};
    esp_err_t reset_status = ESP_OK;
    for (size_t i = 0; i < sizeof(reset_pins) / sizeof(reset_pins[0]); ++i) {
        esp_err_t reset_err = gpio_reset_pin(reset_pins[i]);
        if (reset_status == ESP_OK) reset_status = reset_err;
        if (err == ESP_OK) err = reset_err;
    }
    ESP_LOGI(TAG, "%s BUS_PINS_RESET=%s", name, esp_err_to_name(reset_status));
    esp_err_t off_err = set_power(false);
    if (err == ESP_OK) err = off_err;
    ESP_LOGI(TAG, "ATTEMPT %s RESULT=%s", name, esp_err_to_name(err));
    return err;
}

void app_main(void)
{
    /* 独立固件只初始化供电保持脚、TF 供电脚和 SPI2；不链接 Hosted 或业务组件。 */
    ESP_LOGI(TAG, "DRIVER_VARIANT=MAIN_SDSPI_OVERLAY");
    esp_err_t err = gpio_set_level(PWR_HOLD, 1);
    if (err != ESP_OK) goto done;
    const gpio_config_t hold_config = {.pin_bit_mask = 1ULL << PWR_HOLD,
                                       .mode = GPIO_MODE_INPUT_OUTPUT};
    err = gpio_config(&hold_config);
    if (err != ESP_OK) goto done;
    err = gpio_set_level(PWR_HOLD, 1);
    if (err != ESP_OK) goto done;
    const gpio_config_t tf_config = {.pin_bit_mask = 1ULL << TF_POWER,
                                     .mode = GPIO_MODE_INPUT_OUTPUT};
    err = gpio_set_level(TF_POWER, 1);
    if (err != ESP_OK) goto done;
    err = gpio_config(&tf_config);
    if (err != ESP_OK) goto done;
    const gpio_config_t cd_config = {.pin_bit_mask = 1ULL << TF_CD,
                                     .mode = GPIO_MODE_INPUT};
    err = gpio_config(&cd_config);
    if (err != ESP_OK) goto done;
    ESP_LOGI(TAG, "BOOT PWR_IO GPIO8=%d TF_POWER GPIO45=%d CD GPIO39=%d",
             gpio_get_level(PWR_HOLD), gpio_get_level(TF_POWER), gpio_get_level(TF_CD));
    esp_err_t first = run_attempt("F1_COLD", true);
    esp_err_t second = run_attempt("F2_RETRY", true);
    esp_err_t third = run_attempt("F3_RETRY", true);
    ESP_LOGI(TAG, "SUMMARY F1=%s F2=%s F3=%s", esp_err_to_name(first),
             esp_err_to_name(second), esp_err_to_name(third));
    return;
done:
    ESP_LOGE(TAG, "GPIO_INIT=%s", esp_err_to_name(err));
}
