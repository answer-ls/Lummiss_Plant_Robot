/* 本文件由 tools/gif2c.py 自动生成，请勿手工修改。 */
/* 屏幕 GIF 表情资源：8 个情绪，按本文件顺序轮播。 */
#ifndef LUMMISS_GIF_ASSETS_H
#define LUMMISS_GIF_ASSETS_H

#include "stdint.h"

/* 动画内容区尺寸：16:9，置于横屏 320x240 时上下各留 30px 黑边 */
#define GIF_FRAME_W 320
#define GIF_FRAME_H 180

/* 情绪 ID：顺序即屏幕轮播顺序（与 tools/gif2c.py 中 EMOTIONS 一致） */
typedef enum {
    LUMMISS_GIF_BLINK = 0,    /* 眨眼 */
    LUMMISS_GIF_XI = 1,    /* 喜 */
    LUMMISS_GIF_NU = 2,    /* 怒 */
    LUMMISS_GIF_AI = 3,    /* 哀 */
    LUMMISS_GIF_LE = 4,    /* 乐 */
    LUMMISS_GIF_THINK = 5,    /* 思考 */
    LUMMISS_GIF_SURPRISE = 6,    /* 惊讶 */
    LUMMISS_GIF_CONFUSED = 7,    /* 疑惑 */
    LUMMISS_GIF_COUNT
} lummiss_gif_id_t;

/* 单个情绪的资源描述：
 *   palette    256 色 x 2B（RGB565 高位字节在前，即 lv_color16_t 布局）
 *   duration_ms  每帧停留毫秒数
 *   frame_offset 各帧在 rle_data 中的累计偏移，长度 frame_count+1
 *   rle_data     逐帧游程数据，每帧以 {0,0} 结束
 */
typedef struct {
    const char *name;
    uint8_t     frame_count;
    const uint8_t  *palette;
    const uint16_t *duration_ms;
    const uint32_t *frame_offset;
    const uint8_t  *rle_data;
} lummiss_gif_asset_t;

/* 资源表：gif_assets[id] 与上方的情绪 ID 一一对应 */
extern const lummiss_gif_asset_t gif_assets[LUMMISS_GIF_COUNT];

#endif /* LUMMISS_GIF_ASSETS_H */
