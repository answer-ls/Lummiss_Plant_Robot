#include <stdbool.h>
#include <stdint.h>

#include "driver/spi_master.h"
#include "esp_err.h"
#include "esp_lcd_io_spi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"

static const char *TAG = "FACE_DEMO";

/* GMT020-02-8P（ST7789）按竖屏 240×320 使用 LVGL 8.4。 */
#define LCD_H_RES               240
#define LCD_V_RES               320
#define LCD_DRAW_LINES          24
#define LCD_PIXEL_CLOCK_HZ      (20 * 1000 * 1000)

/* 用户给出的 4 线 SPI 接线。背光 BL 接 3V3，程序不能调节亮度。 */
#define LCD_PIN_SCLK            20
#define LCD_PIN_MOSI            32
#define LCD_PIN_RST             3
#define LCD_PIN_DC              2
#define LCD_PIN_CS              1
#define LCD_SPI_HOST            SPI2_HOST

typedef enum {
    FACE_NEUTRAL = 0,
    FACE_SMILE,
    FACE_HAPPY,
    FACE_COUNT,
} face_id_t;

static lv_obj_t *s_face_layers[FACE_COUNT];
static lv_obj_t *s_neutral_open_eyes;
static lv_obj_t *s_neutral_closed_eyes;
static face_id_t s_current_face;
static uint32_t s_face_started_at;
static uint32_t s_next_blink_at;
static uint32_t s_blink_ends_at;
static bool s_is_blinking;

static const uint32_t s_face_duration_ms[FACE_COUNT] = {
    [FACE_NEUTRAL] = 5000,
    [FACE_SMILE] = 2500,
    [FACE_HAPPY] = 3000,
};

/* LVGL 计时值会回绕，用有符号差值判断时刻是否到达。 */
static bool time_reached(uint32_t now, uint32_t target)
{
    return (int32_t)(now - target) >= 0;
}

static lv_obj_t *create_clean_layer(lv_obj_t *parent)
{
    lv_obj_t *layer = lv_obj_create(parent);
    lv_obj_remove_style_all(layer);
    lv_obj_set_size(layer, LCD_H_RES, LCD_V_RES);
    lv_obj_set_pos(layer, 0, 0);
    lv_obj_clear_flag(layer, LV_OBJ_FLAG_SCROLLABLE);
    return layer;
}

static lv_obj_t *create_circle(lv_obj_t *parent, int x, int y, int width, int height,
                               lv_color_t color)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, width, height);
    lv_obj_set_style_bg_color(obj, color, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(obj, LV_RADIUS_CIRCLE, 0);
    return obj;
}

static lv_obj_t *create_arc(lv_obj_t *parent, int x, int y, int width, int height,
                            uint16_t start_angle, uint16_t end_angle,
                            lv_color_t color, int line_width)
{
    lv_obj_t *arc = lv_arc_create(parent);
    lv_obj_remove_style_all(arc);
    lv_obj_set_pos(arc, x, y);
    lv_obj_set_size(arc, width, height);
    lv_arc_set_angles(arc, start_angle, end_angle);
    lv_obj_set_style_arc_color(arc, color, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(arc, line_width, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(arc, true, LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(arc, LV_OPA_COVER, LV_PART_INDICATOR);
    return arc;
}

static lv_obj_t *create_line(lv_obj_t *parent, int x, int y,
                             const lv_point_t *points, uint16_t point_count,
                             lv_color_t color, int line_width)
{
    lv_obj_t *line = lv_line_create(parent);
    lv_obj_remove_style_all(line);
    lv_line_set_points(line, points, point_count);
    lv_obj_set_pos(line, x, y);
    lv_obj_set_style_line_color(line, color, 0);
    lv_obj_set_style_line_width(line, line_width, 0);
    lv_obj_set_style_line_rounded(line, true, 0);
    return line;
}

static void create_neutral_face(lv_obj_t *screen)
{
    const lv_color_t white = lv_color_white();
    const lv_color_t black = lv_color_black();
    const lv_color_t red = lv_color_hex(0xFF5364);
    lv_obj_t *layer = create_clean_layer(screen);
    s_face_layers[FACE_NEUTRAL] = layer;

    /* 睁眼由白色眼球和黑色瞳孔组成。 */
    s_neutral_open_eyes = create_clean_layer(layer);
    lv_obj_t *left_eye = create_circle(s_neutral_open_eyes, 66, 126, 38, 38, white);
    lv_obj_t *right_eye = create_circle(s_neutral_open_eyes, 136, 126, 38, 38, white);
    create_circle(left_eye, 12, 10, 14, 18, black);
    create_circle(right_eye, 12, 10, 14, 18, black);

    /* 眨眼时用两条短白线替换睁开的眼睛。 */
    s_neutral_closed_eyes = create_clean_layer(layer);
    create_circle(s_neutral_closed_eyes, 67, 143, 36, 5, white);
    create_circle(s_neutral_closed_eyes, 137, 143, 36, 5, white);
    lv_obj_add_flag(s_neutral_closed_eyes, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *mouth = create_circle(layer, 110, 184, 20, 5, red);
    lv_obj_set_style_radius(mouth, 3, 0);
}

static void create_smile_face(lv_obj_t *screen)
{
    const lv_color_t white = lv_color_white();
    const lv_color_t red = lv_color_hex(0xFF5364);
    lv_obj_t *layer = create_clean_layer(screen);
    s_face_layers[FACE_SMILE] = layer;

    /* 眼睛使用上半圆弧，嘴部使用下半圆弧。 */
    create_arc(layer, 60, 126, 46, 34, 195, 345, white, 6);
    create_arc(layer, 134, 126, 46, 34, 195, 345, white, 6);
    create_arc(layer, 99, 163, 42, 30, 20, 160, red, 5);
}

static void create_happy_face(lv_obj_t *screen)
{
    static const lv_point_t left_eye_points[] = {{0, 0}, {14, 9}, {0, 18}};
    static const lv_point_t right_eye_points[] = {{14, 0}, {0, 9}, {14, 18}};
    const lv_color_t white = lv_color_white();
    const lv_color_t black = lv_color_black();
    const lv_color_t red = lv_color_hex(0xFF5364);
    const lv_color_t pink = lv_color_hex(0xFF8FA0);
    lv_obj_t *layer = create_clean_layer(screen);
    s_face_layers[FACE_HAPPY] = layer;

    /* 挤眼笑使用相向的折线，贴近文档示意图。 */
    create_line(layer, 69, 132, left_eye_points, 3, white, 6);
    create_line(layer, 157, 132, right_eye_points, 3, white, 6);
    create_circle(layer, 49, 165, 20, 14, red);
    create_circle(layer, 171, 165, 20, 14, red);

    /* 红色圆形嘴部上方由黑色遮罩切开，形成张嘴大笑的下半圆。 */
    create_circle(layer, 96, 151, 48, 48, red);
    lv_obj_t *mouth_mask = lv_obj_create(layer);
    lv_obj_remove_style_all(mouth_mask);
    lv_obj_set_pos(mouth_mask, 91, 145);
    lv_obj_set_size(mouth_mask, 58, 24);
    lv_obj_set_style_bg_color(mouth_mask, black, 0);
    lv_obj_set_style_bg_opa(mouth_mask, LV_OPA_COVER, 0);
    create_circle(layer, 106, 181, 28, 12, pink);
}

static void set_object_y(void *object, int32_t value)
{
    lv_obj_set_y((lv_obj_t *)object, (lv_coord_t)value);
}

static void set_object_opa(void *object, int32_t value)
{
    lv_obj_set_style_opa((lv_obj_t *)object, (lv_opa_t)value, 0);
}

static void start_vertical_animation(lv_obj_t *object, int y_end, uint32_t duration_ms)
{
    lv_anim_t animation;
    lv_anim_init(&animation);
    lv_anim_set_var(&animation, object);
    lv_anim_set_exec_cb(&animation, set_object_y);
    lv_anim_set_values(&animation, 0, y_end);
    lv_anim_set_time(&animation, duration_ms);
    lv_anim_set_playback_time(&animation, duration_ms);
    lv_anim_set_repeat_count(&animation, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&animation, lv_anim_path_ease_in_out);
    lv_anim_start(&animation);
}

static void show_face(face_id_t face, uint32_t now)
{
    for (int i = 0; i < FACE_COUNT; ++i) {
        if (i == face) {
            lv_obj_clear_flag(s_face_layers[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_face_layers[i], LV_OBJ_FLAG_HIDDEN);
        }
    }

    /* 表情从黑色背景快速淡入，避免切换时突然跳变。 */
    lv_obj_set_style_opa(s_face_layers[face], LV_OPA_TRANSP, 0);
    lv_anim_t fade;
    lv_anim_init(&fade);
    lv_anim_set_var(&fade, s_face_layers[face]);
    lv_anim_set_exec_cb(&fade, set_object_opa);
    lv_anim_set_values(&fade, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_time(&fade, 220);
    lv_anim_start(&fade);

    s_current_face = face;
    s_face_started_at = now;
    s_is_blinking = false;
    lv_obj_clear_flag(s_neutral_open_eyes, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_neutral_closed_eyes, LV_OBJ_FLAG_HIDDEN);
    s_next_blink_at = now + 1800 + (esp_random() % 1800);

    static const char *face_names[] = {"EXP-01 中性待机", "EXP-02 微笑", "EXP-03 开心"};
    ESP_LOGI(TAG, "显示表情：%s", face_names[face]);
}

static void expression_timer_callback(lv_timer_t *timer)
{
    (void)timer;
    const uint32_t now = lv_tick_get();

    /* 中性待机每隔约 1.8～3.6 秒眨眼一次，每次闭眼 130 ms。 */
    if (s_current_face == FACE_NEUTRAL) {
        if (!s_is_blinking && time_reached(now, s_next_blink_at)) {
            s_is_blinking = true;
            s_blink_ends_at = now + 130;
            lv_obj_add_flag(s_neutral_open_eyes, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(s_neutral_closed_eyes, LV_OBJ_FLAG_HIDDEN);
        } else if (s_is_blinking && time_reached(now, s_blink_ends_at)) {
            s_is_blinking = false;
            s_next_blink_at = now + 1800 + (esp_random() % 1800);
            lv_obj_clear_flag(s_neutral_open_eyes, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_neutral_closed_eyes, LV_OBJ_FLAG_HIDDEN);
        }
    }

    if ((uint32_t)(now - s_face_started_at) >= s_face_duration_ms[s_current_face]) {
        show_face((face_id_t)((s_current_face + 1) % FACE_COUNT), now);
    }
}

static void lcd_initialize(esp_lcd_panel_io_handle_t *out_io,
                           esp_lcd_panel_handle_t *out_panel)
{
    ESP_LOGI(TAG, "初始化 GMT020-02-8P / ST7789：%dx%d，SPI %d MHz",
             LCD_H_RES, LCD_V_RES, LCD_PIXEL_CLOCK_HZ / 1000000);
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
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(*out_panel, false));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(*out_panel, false, false));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(*out_panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(*out_panel, true));
}

static void lvgl_initialize(esp_lcd_panel_io_handle_t io,
                            esp_lcd_panel_handle_t panel)
{
    const lvgl_port_cfg_t port_config = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_ERROR_CHECK(lvgl_port_init(&port_config));

    const lvgl_port_display_cfg_t display_config = {
        .io_handle = io,
        .panel_handle = panel,
        .buffer_size = LCD_H_RES * LCD_DRAW_LINES,
        .double_buffer = true,
        .hres = LCD_H_RES,
        .vres = LCD_V_RES,
        .monochrome = false,
        .rotation = {
            .swap_xy = false,
            .mirror_x = false,
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

static void create_expression_demo(void)
{
    lv_obj_t *screen = lv_scr_act();
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    create_neutral_face(screen);
    create_smile_face(screen);
    create_happy_face(screen);

    /* 三个表情分别使用轻呼吸、轻微上浮和活泼弹跳。 */
    start_vertical_animation(s_face_layers[FACE_NEUTRAL], -2, 1500);
    start_vertical_animation(s_face_layers[FACE_SMILE], -2, 700);
    start_vertical_animation(s_face_layers[FACE_HAPPY], -7, 320);

    show_face(FACE_NEUTRAL, lv_tick_get());
    lv_timer_create(expression_timer_callback, 30, NULL);
}

void app_main(void)
{
    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_handle_t panel = NULL;
    lcd_initialize(&io, &panel);
    lvgl_initialize(io, panel);

    lvgl_port_lock(0);
    create_expression_demo();
    lvgl_port_unlock();

    ESP_LOGI(TAG, "LVGL 三表情演示已启动；BL 接 3V3，背光不受程序控制");
}
