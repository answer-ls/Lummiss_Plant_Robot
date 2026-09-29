#include "mem_contig.h"
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_lcd_io_spi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "display_driver.h"
#include "battery_monitor.h"
#include "board_pins.h"
#include "home_info.h"
#include "test_profile.h"

LV_FONT_DECLARE(lv_font_lummiss_weather_16);

static const char *TAG = "DISPLAY";

static bool s_binding_required;
static char s_binding_code[7];

void display_driver_set_binding_code(const char *code)
{
    /* 只接受后台返回的六位数字，保留前导零，不截断或生成假码。 */
    s_binding_required = code != NULL;
    s_binding_code[0] = '\0';
    if (!code || strlen(code) != 6) return;
    for (size_t i = 0; i < 6; ++i) {
        if (code[i] < '0' || code[i] > '9') return;
    }
    memcpy(s_binding_code, code, sizeof(s_binding_code));
}


/* GMT020-02-8P（ST7789）原生为 240×320，本项目交换 X/Y 后横屏使用。 */
#define LCD_PANEL_H_RES         240
#define LCD_PANEL_V_RES         320
#define LCD_H_RES               320
#define LCD_V_RES               240
/* RTC/DTLS 与 720p H.264 reference frame 都需要连续 INTERNAL/DMA 内存。
 * A/B 测试把 partial draw buffer 从 5 行降到 3 行，仍保持双缓冲：
 * 320 * 3 * 2 = 1920 bytes/块，两块比 5 行版本再释放 2560 bytes。 */
#define LCD_DRAW_LINES          3
/* RTC 预览期间暂停 LCD 刷新，避免 720p 编码占满 DMA 时 SPI 临时分配失败。 */
/* 档位 9 用于新 PCB 首板验证，先把 SPI 降到 10 MHz，排除走线、焊接和
 * 信号完整性造成的 40 MHz 黑屏。完整系统仍保持已经验证过的 40 MHz。 */
#if CAMERA_TEST_PROFILE == CAMERA_TEST_PERIPH_ONLY
#define LCD_PIXEL_CLOCK_HZ      (10 * 1000 * 1000)
#else
#define LCD_PIXEL_CLOCK_HZ      (40 * 1000 * 1000)
#endif

/* 屏幕 GPIO 统一从板级引脚表读取。 */
#define LCD_PIN_SCLK            BOARD_LCD_SCLK
#define LCD_PIN_MOSI            BOARD_LCD_MOSI
#define LCD_PIN_RST             BOARD_LCD_RST
#define LCD_PIN_DC              BOARD_LCD_DC
#define LCD_PIN_CS              BOARD_LCD_CS
#define LCD_PIN_BL              BOARD_LCD_BL
/* ESP-Hosted 的 SPI 全双工链路固定占用 SPI2_HOST（控制器 1）。
 * LCD 使用另一条总线，避免网络初始化后再次初始化 SPI2 时得到
 * ESP_ERR_INVALID_STATE。ESP32-P4 的 SPI3_HOST 可通过 GPIO Matrix 复用到
 * 当前 LCD 接线。 */
#define LCD_SPI_HOST            SPI3_HOST

static lv_obj_t *s_date_label;
static lv_obj_t *s_weather_label;
static lv_obj_t *s_temperature_label;
static lv_obj_t *s_time_label;
static lv_obj_t *s_weather_dot;
static lv_obj_t *s_battery_label;
static lv_obj_t *s_battery_fill;
static bool s_lvgl_ready;
static bool s_rtc_hold_lcd;
/* 仅 LVGL 线程访问：内容变更事件允许一次刷新，其余 RTC 刷新跳过。 */
static bool s_rtc_event_flush;
static bool s_home_dirty;
static int64_t s_last_rtc_refresh_us;
static void (*s_panel_flush)(lv_disp_drv_t *, const lv_area_t *, lv_color_t *);

/* 对象失效会自动恢复 LVGL 刷新定时器，因此必须在实际提交点拦截。
 * 跳过的区域不发送 SPI，立即结束本次 flush；退出 RTC 后整屏重绘。 */
static void rtc_lcd_flush(lv_disp_drv_t *drv, const lv_area_t *area,
                          lv_color_t *pixels)
{
    if (__atomic_load_n(&s_rtc_hold_lcd, __ATOMIC_ACQUIRE) && !s_rtc_event_flush) {
        lv_disp_flush_ready(drv);
        return;
    }
    s_panel_flush(drv, area, pixels);
}

#if CAMERA_TEST_PROFILE == CAMERA_TEST_PERIPH_ONLY
/* 仅用于新 PCB 首板隔离诊断的软件 SPI。它完全绕过 SPI3、DMA、esp_lcd 和
 * LVGL，直接验证 GPIO1~5、FPC 连通性以及 ST7789 是否能响应命令。 */
static inline void lcd_gpio_spi_write_byte(uint8_t value)
{
    for (int bit = 7; bit >= 0; --bit) {
        gpio_set_level(LCD_PIN_MOSI, (value >> bit) & 1U);
        gpio_set_level(LCD_PIN_SCLK, 1);
        gpio_set_level(LCD_PIN_SCLK, 0);
    }
}

static void lcd_gpio_spi_command(uint8_t command,
                                 const uint8_t *parameters,
                                 size_t parameter_count)
{
    gpio_set_level(LCD_PIN_CS, 0);
    gpio_set_level(LCD_PIN_DC, 0);
    lcd_gpio_spi_write_byte(command);
    if (parameter_count > 0U) {
        gpio_set_level(LCD_PIN_DC, 1);
        for (size_t i = 0; i < parameter_count; ++i) {
            lcd_gpio_spi_write_byte(parameters[i]);
        }
    }
    gpio_set_level(LCD_PIN_CS, 1);
}

static void lcd_gpio_spi_fill_test(void)
{
    const gpio_config_t output_config = {
        .pin_bit_mask = (1ULL << LCD_PIN_RST) |
                        (1ULL << LCD_PIN_DC) |
                        (1ULL << LCD_PIN_CS) |
                        (1ULL << LCD_PIN_SCLK) |
                        (1ULL << LCD_PIN_MOSI),
        /* 打开输入通路，日志可以读取引脚上的实际物理电平。 */
        .mode = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&output_config));

    gpio_set_level(LCD_PIN_CS, 1);
    gpio_set_level(LCD_PIN_DC, 1);
    gpio_set_level(LCD_PIN_SCLK, 0);
    gpio_set_level(LCD_PIN_MOSI, 0);

    /* 硬件复位，RESX 低有效。 */
    gpio_set_level(LCD_PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(LCD_PIN_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(LCD_PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(120));

    lcd_gpio_spi_command(0x01, NULL, 0); /* Software Reset */
    vTaskDelay(pdMS_TO_TICKS(150));
    lcd_gpio_spi_command(0x11, NULL, 0); /* Sleep Out */
    vTaskDelay(pdMS_TO_TICKS(120));

    const uint8_t pixel_format = 0x55;   /* RGB565 */
    lcd_gpio_spi_command(0x3A, &pixel_format, 1);
    const uint8_t madctl = 0x00;
    lcd_gpio_spi_command(0x36, &madctl, 1);
    lcd_gpio_spi_command(0x21, NULL, 0); /* Display Inversion On */
    lcd_gpio_spi_command(0x13, NULL, 0); /* Normal Display Mode On */
    lcd_gpio_spi_command(0x29, NULL, 0); /* Display On */
    vTaskDelay(pdMS_TO_TICKS(20));

    const uint8_t column_range[] = {0x00, 0x00, 0x00, 0xEF}; /* 0..239 */
    const uint8_t row_range[] = {0x00, 0x00, 0x01, 0x3F};    /* 0..319 */
    lcd_gpio_spi_command(0x2A, column_range, sizeof(column_range));
    lcd_gpio_spi_command(0x2B, row_range, sizeof(row_range));

    ESP_LOGI(TAG,
             "GPIO 软件 SPI 测试开始：绕过 SPI3/LVGL，直接写 ST7789 色块");
    gpio_set_level(LCD_PIN_CS, 0);
    gpio_set_level(LCD_PIN_DC, 0);
    lcd_gpio_spi_write_byte(0x2C);       /* Memory Write */
    gpio_set_level(LCD_PIN_DC, 1);

    for (uint32_t pixel = 0; pixel < LCD_PANEL_H_RES * LCD_PANEL_V_RES; ++pixel) {
        /* 上半屏洋红，下半屏青色，两个颜色的高低字节都明显不相同。 */
        const uint16_t color = pixel < (LCD_PANEL_H_RES * LCD_PANEL_V_RES / 2U)
                                   ? 0xF81FU
                                   : 0x07FFU;
        lcd_gpio_spi_write_byte((uint8_t)(color >> 8));
        lcd_gpio_spi_write_byte((uint8_t)color);
    }
    gpio_set_level(LCD_PIN_CS, 1);

    ESP_LOGI(TAG,
             "GPIO 软件 SPI 测试完成：应显示上半洋红、下半青色，保持 3 秒");
    ESP_LOGI(TAG, "GPIO 软件 SPI 电平：RST=%d DC=%d CS=%d SCLK=%d MOSI=%d BL=%d",
             gpio_get_level(LCD_PIN_RST), gpio_get_level(LCD_PIN_DC),
             gpio_get_level(LCD_PIN_CS), gpio_get_level(LCD_PIN_SCLK),
             gpio_get_level(LCD_PIN_MOSI), gpio_get_level(LCD_PIN_BL));
    vTaskDelay(pdMS_TO_TICKS(3000));
}
#endif

static void lcd_initialize(esp_lcd_panel_io_handle_t *out_io,
                           esp_lcd_panel_handle_t *out_panel)
{
    const gpio_config_t backlight_config = {
        .pin_bit_mask = 1ULL << LCD_PIN_BL,
        /* 同时打开输入通路，档位 9 才能读取背光引脚的实际物理电平。 */
        .mode = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&backlight_config));
    /* 新 PCB 首板诊断期间背光从板级早期初始化开始一直保持高电平，
     * LCD 初始化过程也不得拉低 GPIO47。 */
    ESP_ERROR_CHECK(gpio_set_level(LCD_PIN_BL, 1));

#if CAMERA_TEST_PROFILE == CAMERA_TEST_PERIPH_ONLY
    /* PWR_IO 刚接管整板电源时，给新 PCB 的 LCD 电源和复位 RC 留出稳定时间。 */
    vTaskDelay(pdMS_TO_TICKS(100));
#endif

    ESP_LOGI(TAG, "初始化 GMT020-02-8P / ST7789：面板 %dx%d，横屏 %dx%d，SPI %d MHz",
             LCD_PANEL_H_RES, LCD_PANEL_V_RES, LCD_H_RES, LCD_V_RES,
             LCD_PIXEL_CLOCK_HZ / 1000000);
    ESP_LOGI(TAG, "SCLK=%d MOSI=%d RST=%d DC=%d CS=%d BL=%d",
             LCD_PIN_SCLK, LCD_PIN_MOSI, LCD_PIN_RST, LCD_PIN_DC, LCD_PIN_CS,
             LCD_PIN_BL);

    const size_t transfer_size = LCD_H_RES * LCD_DRAW_LINES * sizeof(lv_color_t);
    const spi_bus_config_t bus_config = {
        .mosi_io_num = LCD_PIN_MOSI,
        .miso_io_num = -1,
        .sclk_io_num = LCD_PIN_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = transfer_size,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_SPI_HOST, &bus_config, SPI_DMA_CH_AUTO));

    const esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num = LCD_PIN_CS,
        .dc_gpio_num = LCD_PIN_DC,
        .spi_mode = 0,
        .pclk_hz = LCD_PIXEL_CLOCK_HZ,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_SPI_HOST,
                                             &io_config, out_io));

    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = LCD_PIN_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .data_endian = LCD_RGB_DATA_ENDIAN_BIG,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(*out_io, &panel_config, out_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(*out_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(*out_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(*out_panel, 0, 0));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(*out_panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(*out_panel, true));
    vTaskDelay(pdMS_TO_TICKS(20));
    ESP_ERROR_CHECK(gpio_set_level(LCD_PIN_BL, 1));

    ESP_LOGI(TAG,
             "LCD GPIO 回读：RST=%d DC=%d CS=%d BL=%d（期望 BL_ON=%d）",
             gpio_get_level(LCD_PIN_RST), gpio_get_level(LCD_PIN_DC),
             gpio_get_level(LCD_PIN_CS), gpio_get_level(LCD_PIN_BL),
             BOARD_LCD_BL_ON_LEVEL);
#if BOARD_HAS_PWR_IO
    ESP_LOGI(TAG, "LCD 供电保持回读：PWR_IO(GPIO%d)=%d",
             BOARD_PWR_IO, gpio_get_level(BOARD_PWR_IO));
#endif
}

#if CAMERA_TEST_PROFILE == CAMERA_TEST_PERIPH_ONLY
static void lcd_show_raw_test_pattern(esp_lcd_panel_handle_t panel)
{
    /* 绕过 LVGL，直接逐行写 ST7789 原生 240x320 显存。
     * 若这张图能显示而后续 LVGL 色条不能显示，问题才位于 LVGL 刷新层；
     * 若这里也完全无画面，则应检查屏幕供电、排线和 SPI/RESET 硬件。 */
    uint16_t *line = heap_caps_malloc(
        LCD_PANEL_H_RES * sizeof(uint16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (line == NULL) {
        ESP_LOGE(TAG, "ST7789 原生测试行 Buffer 分配失败");
        return;
    }

    static const uint16_t colors[] = {
        0xf800, /* 红 */
        0x07e0, /* 绿 */
        0x001f, /* 蓝 */
        0xffff, /* 白 */
        0x0000, /* 黑 */
    };

    ESP_LOGI(TAG, "ST7789 原生测试开始：绕过 LVGL，直接写 240x320 色条");
    for (int y = 0; y < LCD_PANEL_V_RES; ++y) {
        const size_t band = (size_t)y * 5U / LCD_PANEL_V_RES;
        for (int x = 0; x < LCD_PANEL_H_RES; ++x) {
            line[x] = colors[band];
        }
        ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(
            panel, 0, y, LCD_PANEL_H_RES, y + 1, line));
        /* 10 MHz 下单行约 0.4 ms；等待一个 tick，避免 DMA 尚未读完就覆盖行 Buffer。 */
        vTaskDelay(1);
    }
    vTaskDelay(pdMS_TO_TICKS(1500));
    heap_caps_free(line);
    ESP_LOGI(TAG, "ST7789 原生测试完成，接下来切换到 LVGL 色条");
}
#endif

static void lvgl_initialize(esp_lcd_panel_io_handle_t io,
                            esp_lcd_panel_handle_t panel)
{
    lvgl_port_cfg_t port_config = ESP_LVGL_PORT_INIT_CONFIG();
    /* CPU0 承担 USB 与网络实时任务，LVGL 固定到 CPU1。 */
    port_config.task_affinity = 1;
    /* 当前 esp_lvgl_port 使用 xTaskCreatePinnedToCoreWithCaps 创建任务，
     * 明确支持把任务栈放入 PSRAM。LVGL draw buffer 仍保留 DMA 内存。 */
    port_config.task_stack_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    ESP_ERROR_CHECK(lvgl_port_init(&port_config));

    const lvgl_port_display_cfg_t display_config = {
        .io_handle = io,
        .panel_handle = panel,
        .buffer_size = LCD_H_RES * LCD_DRAW_LINES,
        .double_buffer = true,
        .hres = LCD_H_RES,
        .vres = LCD_V_RES,
        .monochrome = false,
        /* 实机反馈原画面左右镜像。交换坐标轴后打开 mirror_x 修正。 */
        .rotation = {
            .swap_xy = true,
            .mirror_x = true,
            .mirror_y = false,
        },
        .flags = {
            .buff_dma = true,
            .buff_spiram = false,
        },
    };
    lv_disp_t *display = lvgl_port_add_disp(&display_config);
    ESP_ERROR_CHECK(display != NULL ? ESP_OK : ESP_ERR_NO_MEM);
    /* 安装包装回调时持 LVGL 锁，避免任务读到尚未保存的原始回调。 */
    lvgl_port_lock(0);
    s_panel_flush = display->driver->flush_cb;
    display->driver->flush_cb = rtc_lcd_flush;
    lvgl_port_unlock();
    s_lvgl_ready = true;
}

void display_driver_set_rtc_preview(bool active)
{
    /* 即使锁暂时繁忙也先阻止后续 SPI 提交，不能让降载请求静默失效。 */
    __atomic_store_n(&s_rtc_hold_lcd, active, __ATOMIC_RELEASE);
    if (!s_lvgl_ready || !lvgl_port_lock(100)) {
        ESP_LOGW(TAG, "RTC 预览刷新频率切换未完成：LVGL 尚未就绪或正忙");
        return;
    }
    lv_disp_t *display = lv_disp_get_default();
    lv_timer_t *refresh = display == NULL ? NULL : _lv_disp_get_refr_timer(display);
    if (refresh != NULL) {
        if (active) {
            /* 仅暂停显示刷新定时器，保留 LVGL 事件处理以完成动画资源回收。 */
            lv_timer_pause(refresh);
            s_home_dirty = true;
            s_last_rtc_refresh_us = 0;
            ESP_LOGI(TAG, "RTC 预览=1，LCD 内容变更触发刷新，DMA不足时延后");
        } else {
            lv_timer_set_period(refresh, LV_DISP_DEF_REFR_PERIOD);
            lv_timer_resume(refresh);
            /* RTC 期间的 flush 被跳过，恢复时重新提交当前界面全部像素。 */
            lv_obj_invalidate(lv_scr_act());
            ESP_LOGI(TAG, "RTC 预览=0，LCD 刷新已恢复");
        }
    }
    lvgl_port_unlock();
}

static lv_obj_t *create_label(lv_obj_t *parent, const lv_font_t *font,
                              lv_coord_t x, lv_coord_t y,
                              lv_coord_t width, lv_coord_t height)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_size(label, width, height);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    return label;
}

static lv_color_t weather_icon_color(int code)
{
    if (code == 0) return lv_color_hex(0xffe45c);      /* 晴：黄色 */
    if (code <= 3 || code == 45 || code == 48) return lv_color_hex(0xaeb8c2);
    if ((code >= 71 && code <= 77) || code == 85 || code == 86) {
        return lv_color_white();                       /* 雪：白色 */
    }
    if (code >= 95) return lv_color_hex(0xffa940);     /* 雷雨：橙色 */
    return lv_color_hex(0x64b5f6);                     /* 雨：蓝色 */
}

static void update_home_screen(lv_timer_t *timer)
{
    (void)timer;
    if (s_binding_required) return;
    bool battery_changed = false;
    battery_monitor_snapshot_t battery = {0};
    if (s_battery_label != NULL) {
        static bool battery_ui_valid;
        static bool last_battery_available;
        static uint8_t last_battery_percent;
        static uint16_t last_battery_decivolts;
        const bool available = battery_monitor_get_snapshot(&battery);
        battery_changed = !battery_ui_valid || available != last_battery_available ||
            (available && (battery.percent != last_battery_percent ||
                           (uint16_t)((battery.voltage_mv + 50U) / 100U) != last_battery_decivolts));
        if (battery_changed) {
            if (available) {
                lv_label_set_text_fmt(s_battery_label, "%u%%", battery.percent);
                lv_obj_set_width(s_battery_fill,
                                 (lv_coord_t)(12U * battery.percent / 100U));
            } else {
                lv_label_set_text(s_battery_label, "--%");
                lv_obj_set_width(s_battery_fill, 0);
            }
            battery_ui_valid = true;
            last_battery_available = available;
            last_battery_percent = battery.percent;
            last_battery_decivolts = available ?
                (uint16_t)((battery.voltage_mv + 50U) / 100U) : 0;
        }
    }
    static const char *weekdays[] = {
        "周日", "周一", "周二", "周三", "周四", "周五", "周六"
    };
    home_info_snapshot_t info = {0};
    if (!home_info_get_snapshot(&info)) {
        if (battery_changed && __atomic_load_n(&s_rtc_hold_lcd, __ATOMIC_ACQUIRE)) {
            s_home_dirty = true;
        }
        return;
    }

    /* 轮询只检测内容变化，不重复设置控件；避免每秒无条件触发重绘。
     * 按字段比较，不能用结构体 memcmp（含填充字节）。 */
    static home_info_snapshot_t last;
    static bool valid;
    bool changed = !valid || info.time_valid != last.time_valid ||
        info.weather_valid != last.weather_valid || info.month != last.month ||
        info.day != last.day || info.weekday != last.weekday ||
        info.hour != last.hour || info.minute != last.minute ||
        info.temperature_c != last.temperature_c || info.weather_code != last.weather_code;
    const bool rtc = __atomic_load_n(&s_rtc_hold_lcd, __ATOMIC_ACQUIRE);
    if (!changed && !battery_changed && !s_home_dirty) return;
    const int64_t now_us = esp_timer_get_time();
    if (rtc && s_last_rtc_refresh_us != 0 && now_us - s_last_rtc_refresh_us < 30000000LL) {
        s_home_dirty = true;
        return;
    }
    /* 为一次 LCD 提交及其它 DMA 用户留出余量；这是准入检查，不是预留。
     * 不足时保留事件，下次检测再尝试，不进行 SPI 失败重试循环。 */
    if (rtc && (heap_caps_get_largest_free_block(MALLOC_CAP_DMA) < 4096 ||
                heap_caps_get_free_size(MALLOC_CAP_DMA) < 8192)) {
        s_home_dirty = true;
        return;
    }
    last = info;
    valid = true;
    s_home_dirty = false;

    /* 只在有效/无效翻转时各打一行：串口可以直接看出 UI 到底有没有
     * 拿到时间与天气，不用盯着屏幕猜。 */
    static int last_time_state = -1;
    static int last_weather_state = -1;
    const int time_state = info.time_valid ? 1 : 0;
    const int weather_state = info.weather_valid ? 1 : 0;
    if (time_state != last_time_state) {
        last_time_state = time_state;
        ESP_LOGI(TAG, "首页时间：%s",
                 time_state ? "已显示真实时间" : "等待网络校时，暂显示占位符");
    }
    if (weather_state != last_weather_state) {
        last_weather_state = weather_state;
        ESP_LOGI(TAG, "首页天气：%s",
                 weather_state ? "已显示实时天气" : "等待天气接口，暂显示「获取中」");
    }

    if (info.time_valid && info.weekday >= 0 && info.weekday < 7) {
        lv_label_set_text_fmt(s_date_label, "%02d月%02d日 %s",
                              info.month, info.day, weekdays[info.weekday]);
        lv_label_set_text_fmt(s_time_label, "%02d:%02d", info.hour, info.minute);
    } else {
        lv_label_set_text(s_date_label, "--月--日 周-");
        lv_label_set_text(s_time_label, "--:--");
    }

    if (info.weather_valid) {
        lv_label_set_text(s_weather_label, home_info_weather_text(info.weather_code));
        /* LVGL 内置的 sprintf 默认不含浮点格式化（CONFIG_LV_SPRINTF_USE_FLOAT 未开），
         * 对 %f 会走 default 分支原样输出字母 f、数字被吞掉（屏幕只显示 "f°C"）。
         * 温度本就取整显示，这里先四舍五入成整数再按 %d 输出。 */
        const float temperature = info.temperature_c;
        const int temperature_rounded =
            (int)(temperature + (temperature >= 0.0f ? 0.5f : -0.5f));
        lv_label_set_text_fmt(s_temperature_label, "%d°C", temperature_rounded);
        lv_obj_set_style_bg_color(s_weather_dot, weather_icon_color(info.weather_code), 0);
        lv_obj_set_style_bg_opa(s_weather_dot, LV_OPA_COVER, 0);
    } else {
        lv_label_set_text(s_weather_label, "获取中");
        lv_label_set_text(s_temperature_label, "--°C");
        lv_obj_set_style_bg_color(s_weather_dot, lv_color_hex(0x666666), 0);
    }
    if (rtc) {
        /* 在 LVGL 线程内只执行本次事件刷新，随后重新暂停周期刷新。 */
        s_last_rtc_refresh_us = now_us;
        s_rtc_event_flush = true;
        lv_refr_now(NULL);
        s_rtc_event_flush = false;
        lv_timer_pause(_lv_disp_get_refr_timer(lv_disp_get_default()));
    }
}

/* 首页使用 LVGL 控件绘制，并显示电量计最近一次有效采样。 */
static void create_home_screen(void)
{
    lv_obj_t *screen = lv_scr_act();
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *panel = lv_obj_create(screen);
    lv_obj_set_pos(panel, 8, 12);
    lv_obj_set_size(panel, 304, 216);
    lv_obj_set_style_bg_color(panel, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(0x292929), 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_radius(panel, 28, 0);
    lv_obj_set_style_pad_all(panel, 0, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    s_date_label = create_label(panel, &lv_font_simsun_16_cjk, 8, 12, 110, 26);
    lv_label_set_long_mode(s_date_label, LV_LABEL_LONG_CLIP);

    /* 简洁的彩色天气图标；颜色随天气码变化。 */
    s_weather_dot = lv_obj_create(panel);
    lv_obj_remove_style_all(s_weather_dot);
    lv_obj_set_pos(s_weather_dot, 120, 16);
    lv_obj_set_size(s_weather_dot, 16, 16);
    lv_obj_set_style_radius(s_weather_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_weather_dot, LV_OPA_COVER, 0);

    s_weather_label = create_label(panel, &lv_font_lummiss_weather_16,
                                   138, 12, 51, 26);
    lv_label_set_long_mode(s_weather_label, LV_LABEL_LONG_CLIP);
    lv_label_set_text(s_weather_label, "获取中");
    s_temperature_label = create_label(panel, &lv_font_montserrat_16, 189, 12, 45, 26);
    lv_label_set_long_mode(s_temperature_label, LV_LABEL_LONG_CLIP);
    lv_obj_t *battery_body = lv_obj_create(panel);
    lv_obj_remove_style_all(battery_body);
    lv_obj_set_pos(battery_body, 236, 17);
    lv_obj_set_size(battery_body, 20, 12);
    lv_obj_set_style_radius(battery_body, 2, 0);
    lv_obj_set_style_border_color(battery_body, lv_color_white(), 0);
    lv_obj_set_style_border_width(battery_body, 2, 0);
    lv_obj_set_style_bg_opa(battery_body, LV_OPA_TRANSP, 0);
    s_battery_fill = lv_obj_create(battery_body);
    lv_obj_remove_style_all(s_battery_fill);
    lv_obj_set_pos(s_battery_fill, 2, 2);
    lv_obj_set_size(s_battery_fill, 0, 8);
    lv_obj_set_style_bg_color(s_battery_fill, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(s_battery_fill, LV_OPA_COVER, 0);
    lv_obj_t *battery_terminal = lv_obj_create(panel);
    lv_obj_remove_style_all(battery_terminal);
    lv_obj_set_pos(battery_terminal, 257, 20);
    lv_obj_set_size(battery_terminal, 3, 6);
    lv_obj_set_style_radius(battery_terminal, 1, 0);
    lv_obj_set_style_bg_color(battery_terminal, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(battery_terminal, LV_OPA_COVER, 0);
    s_battery_label = create_label(panel, &lv_font_montserrat_16, 261, 12, 41, 26);
    lv_label_set_long_mode(s_battery_label, LV_LABEL_LONG_CLIP);
    s_time_label = create_label(panel, &lv_font_montserrat_48, 12, 91, 280, 68);

    update_home_screen(NULL);
    lv_timer_create(update_home_screen, 1000, NULL);
}

/* 绑定页放在顶层，后续表情切换普通 screen 时不会盖住绑定码。 */
static void create_binding_screen(void)
{
    if (!s_binding_required) return;
    lv_obj_t *panel = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(panel);
    lv_obj_set_size(panel, LCD_H_RES, LCD_V_RES);
    lv_obj_set_style_bg_color(panel, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *title = create_label(panel, &lv_font_simsun_16_cjk, 10, 24, 300, 28);
    lv_label_set_text(title, "设备未绑定");
    lv_obj_t *code = create_label(panel, &lv_font_montserrat_48, 10, 77, 300, 64);
    lv_label_set_text(code, s_binding_code[0] ? s_binding_code : "------");
    lv_obj_t *hint = create_label(panel, &lv_font_simsun_16_cjk, 10, 154, 300, 70);
    lv_label_set_text(hint, s_binding_code[0] ?
        "请在应用中输入绑定码\n绑定完成后请重启设备" :
        "未获取到有效绑定码\n请检查网络后重启设备");
    ESP_LOGI(TAG, "绑定页面已显示：六位绑定码%s", s_binding_code[0] ? "有效" : "缺失或格式错误");
}

void display_driver_start(void)
{
    mem_contig_log("DISPLAY_INIT_BEFORE");
#if CAMERA_TEST_PROFILE == CAMERA_TEST_PERIPH_ONLY
    lcd_gpio_spi_fill_test();
#endif
    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_handle_t panel = NULL;
    lcd_initialize(&io, &panel);
#if CAMERA_TEST_PROFILE == CAMERA_TEST_PERIPH_ONLY
    lcd_show_raw_test_pattern(panel);
#endif
    lvgl_initialize(io, panel);
    mem_contig_log("DISPLAY_INIT_AFTER");

#if CAMERA_TEST_PROFILE == CAMERA_TEST_PERIPH_ONLY
    /* 档位 9 只用于新 PCB 屏幕验证。HOME 页面及其 1 秒更新时间器都不创建，
     * 随后加载的固定色条会一直保持，直到设备断电或复位。 */
    ESP_LOGI(TAG, "档位 9：跳过 HOME 页面，屏幕将持续保持固定测试图");
#else
    lvgl_port_lock(0);
    create_home_screen();
    create_binding_screen();
    lvgl_port_unlock();

    ESP_LOGI(TAG, "动态时间与天气首页已显示（日期/天气/电量顶部单行）");
#endif
}

void display_driver_show_test_pattern(void)
{
    /* 单独创建测试 screen，不删除 HOME。HOME 的 1 秒 timer 仍可安全更新原对象，
     * 测试结束换回完整档位并重启即可恢复首页。 */
    static const uint32_t colors[] = {
        0xff0000, /* 红 */
        0x00ff00, /* 绿 */
        0x0000ff, /* 蓝 */
        0xffffff, /* 白 */
        0x000000, /* 黑 */
    };
    static const char *labels[] = {"R", "G", "B", "W", "K"};

    lvgl_port_lock(0);
    lv_obj_t *screen = lv_obj_create(NULL);
    lv_obj_remove_style_all(screen);
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

    const lv_coord_t bar_width = LCD_H_RES / 5;
    for (size_t i = 0; i < 5; ++i) {
        lv_obj_t *bar = lv_obj_create(screen);
        lv_obj_remove_style_all(bar);
        lv_obj_set_pos(bar, (lv_coord_t)i * bar_width, 0);
        lv_obj_set_size(bar, bar_width, LCD_V_RES);
        lv_obj_set_style_bg_color(bar, lv_color_hex(colors[i]), 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);

        lv_obj_t *label = lv_label_create(bar);
        lv_label_set_text(label, labels[i]);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(
            label, (i == 1 || i == 3) ? lv_color_black() : lv_color_white(), 0);
        lv_obj_center(label);
    }

    /* 四角标记可直接判断 swap/mirror 是否正确。 */
    static const struct {
        const char *text;
        lv_align_t align;
        lv_coord_t x_offset;
        lv_coord_t y_offset;
    } corners[] = {
        {"TL", LV_ALIGN_TOP_LEFT,      4,  4},
        {"TR", LV_ALIGN_TOP_RIGHT,    -4,  4},
        {"BL", LV_ALIGN_BOTTOM_LEFT,   4, -4},
        {"BR", LV_ALIGN_BOTTOM_RIGHT, -4, -4},
    };
    for (size_t i = 0; i < sizeof(corners) / sizeof(corners[0]); ++i) {
        lv_obj_t *label = lv_label_create(screen);
        lv_label_set_text(label, corners[i].text);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(label, lv_color_hex(0xffff00), 0);
        lv_obj_set_style_bg_color(label, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(label, LV_OPA_70, 0);
        lv_obj_align(label, corners[i].align,
                     corners[i].x_offset, corners[i].y_offset);
    }

    lv_scr_load(screen);
    /* 档位 9 必须在函数返回前实际提交首屏，不能只以“对象创建成功”判断
     * 屏幕正常。后续背光低/高测试期间，显存中会一直保留这张色条图。 */
    lv_obj_invalidate(screen);
    lv_refr_now(lv_disp_get_default());
    lvgl_port_unlock();
    ESP_LOGI(TAG, "档位 9 屏幕自检图已显示：R/G/B/W/K 色条 + TL/TR/BL/BR 方向标记");
}

void display_driver_run_backlight_test(void)
{
    /* 当前只验证高电平背光链路，不再执行低/高电平切换。 */
    ESP_ERROR_CHECK(gpio_set_level(LCD_PIN_BL, 1));

#if CAMERA_TEST_PROFILE == CAMERA_TEST_PERIPH_ONLY
    /* 测试图已经同步提交。稍等传输队列结束，再把 SPI 信号恢复成普通 GPIO，
     * 给万用表提供确定不变的电平。之后不再刷新屏幕，GRAM 中的测试图应保持。 */
    vTaskDelay(pdMS_TO_TICKS(100));
    const gpio_config_t static_config = {
        .pin_bit_mask = (1ULL << LCD_PIN_RST) |
                        (1ULL << LCD_PIN_DC) |
                        (1ULL << LCD_PIN_CS) |
                        (1ULL << LCD_PIN_SCLK) |
                        (1ULL << LCD_PIN_MOSI),
        .mode = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&static_config));
    ESP_ERROR_CHECK(gpio_set_level(LCD_PIN_RST, 1));
    ESP_ERROR_CHECK(gpio_set_level(LCD_PIN_MOSI, 1));
    ESP_ERROR_CHECK(gpio_set_level(LCD_PIN_SCLK, 0));
    ESP_ERROR_CHECK(gpio_set_level(LCD_PIN_CS, 1));
    ESP_ERROR_CHECK(gpio_set_level(LCD_PIN_DC, 1));
    ESP_ERROR_CHECK(gpio_set_level(LCD_PIN_BL, 1));

    ESP_LOGW(TAG,
             "LCD 静态测量模式：RST=HIGH SDA=HIGH SCL=LOW CS=HIGH DC=HIGH BL=HIGH");
    ESP_LOGI(TAG, "LCD 静态 GPIO 回读：RST=%d SDA=%d SCL=%d CS=%d DC=%d BL=%d",
             gpio_get_level(LCD_PIN_RST), gpio_get_level(LCD_PIN_MOSI),
             gpio_get_level(LCD_PIN_SCLK), gpio_get_level(LCD_PIN_CS),
             gpio_get_level(LCD_PIN_DC), gpio_get_level(LCD_PIN_BL));
#else
    ESP_LOGI(TAG, "背光固定高电平测试：GPIO%d=%d，将持续保持 HIGH",
             LCD_PIN_BL, gpio_get_level(LCD_PIN_BL));
#endif
}

void display_driver_log_task_stack(void)
{
    /* esp_lvgl_port 当前固定使用 taskLVGL 作为内部任务名。只做诊断，
     * 找不到任务时不影响显示。ESP-IDF 的 high-water mark 单位为字节。 */
    TaskHandle_t task = xTaskGetHandle("taskLVGL");
    if (task == NULL) {
        ESP_LOGW(TAG, "LVGL task high-water：未找到 taskLVGL");
        return;
    }
    ESP_LOGI(TAG, "LVGL task stack high-water=%u bytes，stack_caps=PSRAM",
             (unsigned)uxTaskGetStackHighWaterMark(task));
}
