#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_check.h"
#include "esp_memory_utils.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "bsp_board_extra.h"
#include "lvgl.h"

static const char *TAG = "TOUCH_DRAW";

#define MAX_POINTS 2000

typedef struct {
    lv_point_t points[MAX_POINTS];
    uint16_t point_count;
    lv_color_t current_color;
    lv_coord_t line_width;
    int last_x;
    int last_y;
    bool is_drawing;
} draw_canvas_t;

static draw_canvas_t canvas;

static void screen_touch_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    
    lv_indev_t *indev = lv_indev_get_act();
    if (indev == NULL) {
        return;
    }
    
    lv_point_t point;
    lv_indev_get_point(indev, &point);
    
    ESP_LOGD(TAG, "Touch at (%d, %d), event=%d", point.x, point.y, code);
    
    if (code == LV_EVENT_PRESSED) {
        canvas.is_drawing = true;
        canvas.last_x = point.x;
        canvas.last_y = point.y;
        
        if (canvas.point_count < MAX_POINTS - 1) {
            canvas.points[canvas.point_count].x = point.x;
            canvas.points[canvas.point_count].y = point.y;
            canvas.point_count++;
            
            lv_obj_t *new_line = lv_line_create(lv_scr_act());
            lv_point_t single_point[] = { {point.x, point.y} };
            lv_line_set_points(new_line, single_point, 1);
            lv_obj_set_style_line_color(new_line, canvas.current_color, 0);
            lv_obj_set_style_line_width(new_line, canvas.line_width, 0);
            lv_obj_set_style_line_rounded(new_line, true, 0);
        }
        
    } else if (code == LV_EVENT_PRESSING && canvas.is_drawing) {
        if (canvas.point_count < MAX_POINTS - 1) {
            canvas.points[canvas.point_count].x = point.x;
            canvas.points[canvas.point_count].y = point.y;
            canvas.point_count++;
            
            lv_obj_t *last_line = NULL;
            lv_obj_t *child = lv_obj_get_child(lv_scr_act(), 0);
            while (child) {
                if (lv_obj_check_type(child, &lv_line_class)) {
                    last_line = child;
                }
                child = lv_obj_get_child(lv_scr_act(), lv_obj_get_index(child) + 1);
            }
            
            if (last_line) {
                lv_line_set_points(last_line, canvas.points, canvas.point_count);
            }
        }
        
        canvas.last_x = point.x;
        canvas.last_y = point.y;
        
    } else if (code == LV_EVENT_RELEASED) {
        canvas.is_drawing = false;
        canvas.last_x = -1;
        canvas.last_y = -1;
    }
}

static void clear_btn_event_cb(lv_event_t *e)
{
    ESP_LOGI(TAG, "Clear canvas");
    
    canvas.point_count = 0;
    canvas.last_x = -1;
    canvas.last_y = -1;
    canvas.is_drawing = false;
    
    lv_obj_t *child = lv_obj_get_child(lv_scr_act(), 0);
    while (child) {
        if (lv_obj_check_type(child, &lv_line_class)) {
            lv_obj_del(child);
            child = lv_obj_get_child(lv_scr_act(), 0);
        } else {
            child = lv_obj_get_child(lv_scr_act(), lv_obj_get_index(child) + 1);
        }
    }
}

static void color_btn_event_cb(lv_event_t *e)
{
    if (canvas.current_color.full == lv_color_make(255, 0, 0).full) {
        canvas.current_color = lv_color_make(0, 255, 0);
    } else if (canvas.current_color.full == lv_color_make(0, 255, 0).full) {
        canvas.current_color = lv_color_make(0, 0, 255);
    } else if (canvas.current_color.full == lv_color_make(0, 0, 255).full) {
        canvas.current_color = lv_color_make(255, 255, 0);
    } else if (canvas.current_color.full == lv_color_make(255, 255, 0).full) {
        canvas.current_color = lv_color_make(128, 0, 128);
    } else {
        canvas.current_color = lv_color_make(255, 0, 0);
    }
    
    lv_obj_t *btn = lv_event_get_target(e);
    if (btn) {
        lv_obj_set_style_bg_color(btn, canvas.current_color, 0);
    }
    
    ESP_LOGI(TAG, "Color changed");
}

static void create_touch_draw_demo(void)
{
    uint32_t h_res = BSP_LCD_H_RES;
    uint32_t v_res = BSP_LCD_V_RES;
    
    ESP_LOGI(TAG, "Screen resolution: %lux%lu", h_res, v_res);
    
    memset(&canvas, 0, sizeof(draw_canvas_t));
    canvas.current_color = lv_color_make(255, 0, 0);
    canvas.line_width = 5;
    canvas.last_x = -1;
    canvas.last_y = -1;
    canvas.is_drawing = false;
    
    lv_obj_set_style_bg_color(lv_scr_act(), lv_color_white(), 0);
    
    lv_obj_t *title = lv_label_create(lv_scr_act());
    lv_label_set_text(title, "Touch to Draw");
    lv_obj_set_style_text_color(title, lv_color_black(), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);
    
    lv_obj_t *hint = lv_label_create(lv_scr_act());
    lv_label_set_text(hint, "Tap for dots, drag for lines");
    lv_obj_set_style_text_color(hint, lv_color_make(100, 100, 100), 0);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 40);
    
    lv_obj_add_event_cb(lv_scr_act(), screen_touch_event_cb, LV_EVENT_ALL, NULL);
    
    lv_obj_t *color_btn = lv_btn_create(lv_scr_act());
    lv_obj_set_size(color_btn, 80, 40);
    lv_obj_align(color_btn, LV_ALIGN_BOTTOM_LEFT, 20, -20);
    lv_obj_set_style_bg_color(color_btn, canvas.current_color, 0);
    lv_obj_add_event_cb(color_btn, color_btn_event_cb, LV_EVENT_CLICKED, NULL);
    
    lv_obj_t *color_label = lv_label_create(color_btn);
    lv_label_set_text(color_label, "Color");
    lv_obj_center(color_label);
    
    lv_obj_t *clear_btn = lv_btn_create(lv_scr_act());
    lv_obj_set_size(clear_btn, 80, 40);
    lv_obj_align(clear_btn, LV_ALIGN_BOTTOM_RIGHT, -20, -20);
    lv_obj_set_style_bg_color(clear_btn, lv_color_make(200, 50, 50), 0);
    lv_obj_add_event_cb(clear_btn, clear_btn_event_cb, LV_EVENT_CLICKED, NULL);
    
    lv_obj_t *clear_label = lv_label_create(clear_btn);
    lv_label_set_text(clear_label, "Clear");
    lv_obj_center(clear_label);
    
    ESP_LOGI(TAG, "Touch draw demo created successfully");
    ESP_LOGI(TAG, "Touch the screen to start drawing!");
}

void app_main(void)
{
    bsp_display_cfg_t cfg = {
        .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
        .buffer_size = BSP_LCD_DRAW_BUFF_SIZE,
        .double_buffer = BSP_LCD_DRAW_BUFF_DOUBLE,
        .flags = {
            .buff_dma = true,
            .buff_spiram = false,
            .sw_rotate = false,
        }
    };
    bsp_display_start_with_config(&cfg);
    bsp_display_backlight_on();

    bsp_display_lock(0);

    create_touch_draw_demo();

    bsp_display_unlock();
}
