#include <stdint.h>
#include <stdio.h>

#include "driver/spi_master.h"
#include "esp_err.h"
#include "esp_lcd_io_spi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"

#include "display_driver.h"
#include "home_info.h"

LV_FONT_DECLARE(lv_font_lummiss_weather_16);

static const char *TAG = "DISPLAY";

/* GMT020-02-8P（ST7789）原生为 240×320，本项目交换 X/Y 后横屏使用。 */
#define LCD_PANEL_H_RES         240
#define LCD_PANEL_V_RES         320
#define LCD_H_RES               320
#define LCD_V_RES               240
#define LCD_DRAW_LINES          20
#define LCD_PIXEL_CLOCK_HZ      (40 * 1000 * 1000)

/* 用户确认的 4 线 SPI 接线。背光 BL 接 3V3，程序不能调节亮度。 */
#define LCD_PIN_SCLK            20
#define LCD_PIN_MOSI            32
#define LCD_PIN_RST             3
#define LCD_PIN_DC              2
#define LCD_PIN_CS              1
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

static void lcd_initialize(esp_lcd_panel_io_handle_t *out_io,
                           esp_lcd_panel_handle_t *out_panel)
{
    ESP_LOGI(TAG, "初始化 GMT020-02-8P / ST7789：面板 %dx%d，横屏 %dx%d，SPI %d MHz",
             LCD_PANEL_H_RES, LCD_PANEL_V_RES, LCD_H_RES, LCD_V_RES,
             LCD_PIXEL_CLOCK_HZ / 1000000);
    ESP_LOGI(TAG, "SCLK=%d MOSI=%d RST=%d DC=%d CS=%d",
             LCD_PIN_SCLK, LCD_PIN_MOSI, LCD_PIN_RST, LCD_PIN_DC, LCD_PIN_CS);

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
}

static void lvgl_initialize(esp_lcd_panel_io_handle_t io,
                            esp_lcd_panel_handle_t panel)
{
    lvgl_port_cfg_t port_config = ESP_LVGL_PORT_INIT_CONFIG();
    /* CPU0 承担 USB 与网络实时任务，LVGL 固定到 CPU1。 */
    port_config.task_affinity = 1;
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
    static const char *weekdays[] = {
        "星期日", "星期一", "星期二", "星期三", "星期四", "星期五", "星期六"
    };
    home_info_snapshot_t info = {0};
    if (!home_info_get_snapshot(&info)) {
        return;
    }

    if (info.time_valid && info.weekday >= 0 && info.weekday < 7) {
        lv_label_set_text_fmt(s_date_label, "%02d月%02d日 %s",
                              info.month, info.day, weekdays[info.weekday]);
        lv_label_set_text_fmt(s_time_label, "%02d:%02d", info.hour, info.minute);
    } else {
        lv_label_set_text(s_date_label, "--月--日 星期-");
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
}

/* 首页使用 LVGL 控件绘制，联网后可更新真实信息。电量等待电池管理模块。 */
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

    s_date_label = create_label(panel, &lv_font_simsun_16_cjk, 18, 34, 142, 26);

    /* 简洁的彩色天气图标；颜色随天气码变化。 */
    s_weather_dot = lv_obj_create(panel);
    lv_obj_remove_style_all(s_weather_dot);
    lv_obj_set_pos(s_weather_dot, 166, 36);
    lv_obj_set_size(s_weather_dot, 20, 20);
    lv_obj_set_style_radius(s_weather_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_weather_dot, LV_OPA_COVER, 0);

    s_weather_label = create_label(panel, &lv_font_lummiss_weather_16,
                                   190, 34, 54, 26);
    s_temperature_label = create_label(panel, &lv_font_montserrat_20, 238, 32, 62, 30);
    s_time_label = create_label(panel, &lv_font_montserrat_48, 12, 91, 280, 68);

    update_home_screen(NULL);
    lv_timer_create(update_home_screen, 1000, NULL);
}

void display_driver_start(void)
{
    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_handle_t panel = NULL;
    lcd_initialize(&io, &panel);
    lvgl_initialize(io, panel);

    lvgl_port_lock(0);
    create_home_screen();
    lvgl_port_unlock();

    ESP_LOGI(TAG, "动态时间与天气首页已显示（横屏 320x240，镜像已修正，无电量）");
}