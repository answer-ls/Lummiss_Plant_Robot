#ifndef LUMMISS_SCREEN_CAROUSEL_H
#define LUMMISS_SCREEN_CAROUSEL_H

/* 屏幕轮播：天气首页停留 5 秒，然后 TF 卡上的每个 LUM1 BIN 各停 5 秒，
 * 走完一圈回到天气首页，如此循环。
 *
 * 必须在 display_driver_start() 之后调用：本模块把 LVGL 已经建好的默认屏幕
 * （天气首页）当作轮播的第一环，并复用已初始化的 LVGL 环境。
 *
 * 内部自行挂载 TF 卡。挂载失败、或卡上找不到 .bin 时，只打在日志里然后
 * 停在天气首页，不影响屏幕和摄像头链路。 */
void screen_carousel_start(void);

#endif /* LUMMISS_SCREEN_CAROUSEL_H */
