#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "driver/spi_master.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_lcd_io_spi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"

#include "display_driver.h"
#include "gif_assets.h"

static const char *TAG = "DISPLAY";

/* ===== 屏幕与接线常量 =====
 * GMT020-02-8P（ST7789）面板原生是竖屏 240(宽)×320(高)，本程序把它旋转成
 * 横屏使用：LVGL 逻辑分辨率 320(宽)×240(高)。旋转由 esp_lvgl_port 在注册
 * 显示端口时写入面板（MADCTL 换轴），代码里不要再手动调用 swap_xy/mirror。
 */
#define LCD_PANEL_H_RES         240      /* 面板原生宽度（仅用于日志/注释） */
#define LCD_PANEL_V_RES         320      /* 面板原生高度（仅用于日志/注释） */
#define LCD_H_RES               320      /* LVGL 逻辑宽（横屏） */
#define LCD_V_RES               240      /* LVGL 逻辑高（横屏） */
#define LCD_DRAW_LINES          40       /* 每段刷屏行数（全宽 GIF 整幅重绘，加大提高吞吐） */
#define LCD_PIXEL_CLOCK_HZ      (40 * 1000 * 1000)   /* 面板 SPI 时钟；全幅约 20fps 需高于 20MHz */

/* 用户给出的 4 线 SPI 接线。背光 BL 接 3V3，程序不能调节亮度。 */
#define LCD_PIN_SCLK            20
#define LCD_PIN_MOSI            32
#define LCD_PIN_RST             3
#define LCD_PIN_DC              2
#define LCD_PIN_CS              1
#define LCD_SPI_HOST            SPI2_HOST

/* ===== GIF 播放参数 =====
 * 素材是 16:9，横屏上下留黑边：中央 GIF_FRAME_H 高的区域播放动画。
 */
#define GIF_LEFT               ((LCD_H_RES - GIF_FRAME_W) / 2)   /* = 0，恰好占满整宽 */
#define GIF_TOP                ((LCD_V_RES - GIF_FRAME_H) / 2)   /* = 30，上下各留 30px */
#define GIF_TICK_MS            10        /* 播放调度定时器周期，越小越贴近素材帧率 */
#define EMOTION_DWELL_MS       6000      /* 每个情绪停留时长，到点切下一个 */

/* 帧解码缓冲：存一帧 320x180 的 RGB565，放 PSRAM 减少内部 RAM 占用 */
static lv_color_t *s_frame_buf;
static lv_obj_t *s_img;                 /* 显示 GIF 帧的全屏图片对象 */
static lv_img_dsc_t s_img_dsc;          /* 图片描述符，data 固定指向 s_frame_buf */

/* 播放状态 */
static lummiss_gif_id_t s_emotion;      /* 当前情绪（gif_assets[] 下标） */
static uint32_t s_frame_idx;            /* 当前帧号 */
static uint32_t s_next_frame_at;        /* 当前帧应被替换的时刻（lv_tick_get） */
static uint32_t s_next_emotion_at;      /* 切换到下一情绪的时刻 */

/* ===== GIF 帧解码 =====
 * 把某个情绪的第 frame 帧，从 RLE 数据解出来写到 dst。
 * RLE 格式（见 gif_assets.h / gif2c.py）：{run_len, color_idx} 反复，
 * {0,0} 为帧结束标记；palette 每色 2 字节、RGB565 高字节在前。
 * 这里按两个字节整体写入（高字节在前正是面板/ LV_COLOR_16_SWAP 布局），
 * 不需要逐位换算，也不会产生字节交换。
 */
static void decode_gif_frame(const lummiss_gif_asset_t *asset,
                             uint32_t frame,
                             lv_color_t *dst)
{
    const uint8_t *stream = asset->rle_data + asset->frame_offset[frame];
    const uint8_t *end    = asset->rle_data + asset->frame_offset[frame + 1];
    const uint8_t *pal    = asset->palette;
    uint8_t *out          = (uint8_t *)dst;

    while (stream < end) {
        const uint8_t run = stream[0];   /* 连续同色像素个数 */
        const uint8_t idx = stream[1];   /* 调色板索引 */
        stream += 2;
        if (run == 0) {
            break;                       /* 帧结束标记 {0,0} */
        }
        const uint8_t hi = pal[idx * 2 + 0];   /* 颜色高字节在前 */
        const uint8_t lo = pal[idx * 2 + 1];
        for (uint32_t i = 0; i < run; ++i) {
            *out++ = hi;
            *out++ = lo;
        }
    }
}

/* 进入一个新的情绪：停在它第 0 帧，并设定换帧/换情绪的时刻 */
static void start_emotion(lummiss_gif_id_t id)
{
    const lummiss_gif_asset_t *asset = &gif_assets[id];

    s_emotion = id;
    s_frame_idx = 0;

    /* 先解出第 0 帧并刷新显示 */
    decode_gif_frame(asset, 0, s_frame_buf);
    lv_obj_invalidate(s_img);

    const uint32_t now = lv_tick_get();
    /* 帧的停留时长取素材自带时长（毫秒），首帧 0 结束后切到第 1 帧 */
    s_next_frame_at = now + asset->duration_ms[0];
    /* 整个情绪停留 EMOTION_DWELL_MS 后切换到下一情绪 */
    s_next_emotion_at = now + EMOTION_DWELL_MS;

    ESP_LOGI(TAG, "切换情绪：%s（共 %u 帧，停留 %u ms）",
             asset->name, asset->frame_count, EMOTION_DWELL_MS);
}

/* 播放调度：按素材帧率换帧；情绪到点后按 gif_assets[] 顺序循环 */
static void gif_timer_callback(lv_timer_t *timer)
{
    (void)timer;
    const uint32_t now = lv_tick_get();

    /* LVGL 毫秒计时会回绕，用有符号差判断“是否已到点” */
    if ((int32_t)(now - s_next_emotion_at) >= 0) {
        /* 当前情绪已停够时长：切下一个情绪（循环） */
        start_emotion((lummiss_gif_id_t)((s_emotion + 1) % LUMMISS_GIF_COUNT));
        return;
    }
    if ((int32_t)(now - s_next_frame_at) >= 0) {
        /* 该帧已到切换时间：解出并显示下一帧 */
        const lummiss_gif_asset_t *asset = &gif_assets[s_emotion];
        s_frame_idx++;
        if (s_frame_idx >= asset->frame_count) {
            s_frame_idx = 0;             /* 一轮播完，从头再来 */
        }
        decode_gif_frame(asset, s_frame_idx, s_frame_buf);
        lv_obj_invalidate(s_img);        /* 缓冲内容已更新，让 LVGL 重绘该图 */
        s_next_frame_at = now + asset->duration_ms[s_frame_idx];
    }
}

static void lcd_initialize(esp_lcd_panel_io_handle_t *out_io,
                           esp_lcd_panel_handle_t *out_panel)
{
    ESP_LOGI(TAG, "初始化 GMT020-02-8P / ST7789：面板 %dx%d，旋转为横屏 %dx%d，SPI %d MHz",
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
    /* 注意：这里不再手动调用 esp_lcd_panel_swap_xy/mirror，
     * 旋转统一交给下面 lvgl_initialize 里的 rotation 配置（swap_xy=true）。 */
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
        /* 横屏：交换 X/Y 轴（由 esp_lvgl_port 在 add_disp 时写入面板）。
         * 若实机上下/左右颠倒，改 mirror_x/mirror_y 其中一位即可。 */
        .rotation = {
            .swap_xy = true,
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

/* 创建 GIF 播放界面：黑底 + 一块全宽 lv_img，源为常驻 PSRAM 帧缓冲 */
static void create_gif_demo(void)
{
    lv_obj_t *screen = lv_scr_act();
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    /* 分配一块 PSRAM 帧缓冲（320*180*2B ≈ 115 KB），失败则退回内部 RAM */
    const size_t frame_bytes = (size_t)GIF_FRAME_W * GIF_FRAME_H * sizeof(lv_color_t);
    s_frame_buf = heap_caps_malloc(frame_bytes, MALLOC_CAP_SPIRAM);
    if (s_frame_buf == NULL) {
        ESP_LOGW(TAG, "PSRAM 不足，帧缓冲退回内部 RAM（%u 字节）", (unsigned)frame_bytes);
        s_frame_buf = malloc(frame_bytes);
    }
    if (s_frame_buf == NULL) {
        ESP_LOGE(TAG, "帧缓冲分配失败：%u 字节", (unsigned)frame_bytes);
        abort();
    }

    /* 图片描述符：TRUE_COLOR(RGB565)、尺寸 GIF_FRAME_W x GIF_FRAME_H，
     * data 固定指向 s_frame_buf。之后换帧只需改缓冲内容并 invalidate。 */
    s_img_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
    s_img_dsc.header.w = GIF_FRAME_W;
    s_img_dsc.header.h = GIF_FRAME_H;
    s_img_dsc.data_size = frame_bytes;
    s_img_dsc.data = (const uint8_t *)s_frame_buf;

    s_img = lv_img_create(screen);
    lv_obj_remove_style_all(s_img);
    lv_img_set_src(s_img, &s_img_dsc);
    lv_obj_set_pos(s_img, GIF_LEFT, GIF_TOP);

    /* 播放调度定时器；先从第一个情绪开始 */
    lv_timer_create(gif_timer_callback, GIF_TICK_MS, NULL);
    start_emotion(LUMMISS_GIF_BLINK);
}

void display_driver_start(void)
{
    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_handle_t panel = NULL;
    lcd_initialize(&io, &panel);
    lvgl_initialize(io, panel);

    lvgl_port_lock(0);
    create_gif_demo();
    lvgl_port_unlock();

    ESP_LOGI(TAG, "8 组表情动画已启动（横屏 320x240）；BL 接 3V3，背光不受程序控制");
}
