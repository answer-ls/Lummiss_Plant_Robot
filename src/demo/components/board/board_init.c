#include "mem_contig.h"
#include "board_init.h"

#include "board_pins.h"
#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "BOARD";

esp_err_t board_init_early(void)
{
#if BOARD_USE_NEW_PCB
    /* 新 PCB 首板屏幕诊断：上电后立即把背光控制脚持续拉高。
     * 本测试期间其它模块不得再把 BOARD_LCD_BL 拉低。 */
    esp_err_t bl_err = gpio_set_level(BOARD_LCD_BL, 1);
    if (bl_err != ESP_OK) {
        ESP_LOGE(TAG, "预置 LCD BL(GPIO%d) 高电平失败：%s",
                 BOARD_LCD_BL, esp_err_to_name(bl_err));
        return bl_err;
    }

    const gpio_config_t bl_config = {
        .pin_bit_mask = 1ULL << BOARD_LCD_BL,
        .mode = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    bl_err = gpio_config(&bl_config);
    if (bl_err != ESP_OK) {
        ESP_LOGE(TAG, "配置 LCD BL(GPIO%d) 输出失败：%s",
                 BOARD_LCD_BL, esp_err_to_name(bl_err));
        return bl_err;
    }
    ESP_ERROR_CHECK(gpio_set_level(BOARD_LCD_BL, 1));
    ESP_LOGI(TAG, "屏幕诊断模式：LCD BL(GPIO%d) 已持续拉高", BOARD_LCD_BL);
#endif

#if BOARD_HAS_PWR_IO
    /* 先写输出锁存值，再打开输出驱动，避免切换方向时产生低电平毛刺。 */
    esp_err_t err = gpio_set_level(BOARD_PWR_IO, 1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "预置 PWR_IO(GPIO%d) 高电平失败：%s",
                 BOARD_PWR_IO, esp_err_to_name(err));
        return err;
    }

    const gpio_config_t config = {
        .pin_bit_mask = 1ULL << BOARD_PWR_IO,
        /* 同时打开输入通路，便于首板测试读取 PWR_IO 引脚的实际物理电平。 */
        .mode = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    err = gpio_config(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "配置 PWR_IO(GPIO%d) 输出失败：%s",
                 BOARD_PWR_IO, esp_err_to_name(err));
        return err;
    }

    err = gpio_set_level(BOARD_PWR_IO, 1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "拉高 PWR_IO(GPIO%d) 失败：%s",
                 BOARD_PWR_IO, esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "新 PCB 引脚方案已启用，PWR_IO(GPIO%d) 已拉高并保持供电",
             BOARD_PWR_IO);
#else
    ESP_LOGI(TAG, "开发板引脚方案已启用，GPIO8 保留给 ES8311 I2C SCL");
#endif
    return ESP_OK;
}

/* Hosted 在 app_main 前初始化；由其弱钩子记录真正的初始化完成时刻。 */
void board_mem_contig_hosted_ready(void)
{
    mem_contig_log("HOSTED_INIT_DONE");
}

