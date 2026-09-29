#include "battery_monitor.h"

#include "board_pins.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define CW2015_I2C_ADDR       0x62
#define CW2015_REG_VCELL      0x02
#define CW2015_REG_SAMPLE_LEN 4
#define CW2015_REG_MODE       0x0A
#define CW2015_MODE_MASK      0xC0
#define CW2015_MODE_SLEEP     0xC0
#define BATTERY_POLL_MS       5000

static const char *TAG = "BATTERY";
static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_device;
static portMUX_TYPE s_snapshot_lock = portMUX_INITIALIZER_UNLOCKED;
static battery_monitor_snapshot_t s_snapshot;
static bool s_initialized;

static esp_err_t read_register(uint8_t reg, uint8_t *data, size_t size)
{
    return i2c_master_transmit_receive(s_device, &reg, 1, data, size, 100);
}

static void wake_if_sleeping(void)
{
    uint8_t mode = 0xFF;
    esp_err_t err = read_register(CW2015_REG_MODE, &mode, 1);
    if (err == ESP_OK && (mode & CW2015_MODE_MASK) == CW2015_MODE_SLEEP) {
        const uint8_t normal_mode[2] = {CW2015_REG_MODE, 0x00};
        err = i2c_master_transmit(s_device, normal_mode, sizeof(normal_mode), 100);
        ESP_LOGI(TAG, "CW2015 从休眠唤醒：%s", esp_err_to_name(err));
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static esp_err_t read_battery_sample(battery_monitor_snapshot_t *sample)
{
    uint8_t raw[CW2015_REG_SAMPLE_LEN];
    esp_err_t err = read_register(CW2015_REG_VCELL, raw, sizeof(raw));
    if (err != ESP_OK) return err;

    const uint16_t vcell = ((uint16_t)raw[0] << 8) | raw[1];
    const uint16_t soc = ((uint16_t)raw[2] << 8) | raw[3];
    const uint8_t percent = (uint8_t)(soc >> 8);
    /* CW2015 电压 ADC 每码约 305uV；312/1024 是误差约0.1%的整数换算。 */
    const uint16_t voltage_mv = (uint16_t)(((uint32_t)vcell * 312U) / 1024U);
    if (percent > 100 || voltage_mv < 2000 || voltage_mv > 6000) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    *sample = (battery_monitor_snapshot_t){
        .available = true,
        .percent = percent,
        .voltage_mv = voltage_mv,
    };
    return ESP_OK;
}

static void battery_poll_task(void *arg)
{
    (void)arg;
    bool had_error = false;
    battery_monitor_snapshot_t last_logged = {0};
    for (;;) {
        battery_monitor_snapshot_t sample;
        esp_err_t err = read_battery_sample(&sample);
        if (err == ESP_OK) {
            portENTER_CRITICAL(&s_snapshot_lock);
            s_snapshot = sample;
            portEXIT_CRITICAL(&s_snapshot_lock);
            if (had_error || !last_logged.available ||
                sample.percent != last_logged.percent ||
                sample.voltage_mv / 100 != last_logged.voltage_mv / 100) {
                const uint16_t decivolts = (uint16_t)((sample.voltage_mv + 50U) / 100U);
                ESP_LOGI(TAG, "电池电量=%u%% 电压=%u.%uV",
                         sample.percent, decivolts / 10, decivolts % 10);
            }
            last_logged = sample;
            had_error = false;
        } else {
            portENTER_CRITICAL(&s_snapshot_lock);
            s_snapshot.available = false;
            portEXIT_CRITICAL(&s_snapshot_lock);
            if (!had_error) {
                ESP_LOGW(TAG, "CW2015 采样失败：%s", esp_err_to_name(err));
            }
            had_error = true;
        }
        vTaskDelay(pdMS_TO_TICKS(BATTERY_POLL_MS));
    }
}

esp_err_t battery_monitor_init(void)
{
#if !BOARD_HAS_BATTERY_GAUGE
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (s_initialized) return ESP_OK;

    const i2c_master_bus_config_t bus_config = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = BOARD_BATTERY_I2C_SDA,
        .scl_io_num = BOARD_BATTERY_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        /* 原理图已装4.7k外部上拉，避免并联启用内部弱上拉。 */
        .flags.enable_internal_pullup = false,
    };
    esp_err_t err = i2c_new_master_bus(&bus_config, &s_bus);
    if (err != ESP_OK) return err;

    const i2c_device_config_t device_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = CW2015_I2C_ADDR,
        .scl_speed_hz = 100000,
    };
    err = i2c_master_bus_add_device(s_bus, &device_config, &s_device);
    if (err != ESP_OK) goto fail;

    err = i2c_master_probe(s_bus, CW2015_I2C_ADDR, 100);
    if (err != ESP_OK) goto fail;

    wake_if_sleeping();

    s_initialized = true;
    ESP_LOGI(TAG, "CW2015 已连接：addr=0x%02X SDA=GPIO%d SCL=GPIO%d",
             CW2015_I2C_ADDR, BOARD_BATTERY_I2C_SDA, BOARD_BATTERY_I2C_SCL);
    if (xTaskCreate(battery_poll_task, "battery_poll", 2048, NULL, 2, NULL) != pdPASS) {
        s_initialized = false;
        err = ESP_ERR_NO_MEM;
        goto fail;
    }
    return ESP_OK;

fail:
    if (s_device != NULL) {
        i2c_master_bus_rm_device(s_device);
        s_device = NULL;
    }
    if (s_bus != NULL) {
        i2c_del_master_bus(s_bus);
        s_bus = NULL;
    }
    return err;
#endif
}

bool battery_monitor_get_snapshot(battery_monitor_snapshot_t *snapshot)
{
    if (snapshot == NULL) return false;
    portENTER_CRITICAL(&s_snapshot_lock);
    *snapshot = s_snapshot;
    portEXIT_CRITICAL(&s_snapshot_lock);
    return snapshot->available;
}
