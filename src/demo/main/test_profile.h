#ifndef LUMMISS_TEST_PROFILE_H
#define LUMMISS_TEST_PROFILE_H

/* 启动组合测试档位 —— 全工程唯一的档位号枚举点。
 *
 * 档位号 0~8 与历史实机测试记录、项目文档一一对应（"档位 6" 等写法到处都在），
 * 所以只许改名字不许改数值。新增档位只需动本文件：加一个 CAMERA_TEST_* 常量、
 * 在下面的 TP_BITS 分派里加一个分支、在 test_profile_name() 里加一条名字。
 *
 * 生产代码不要自己枚举档位号，一律用 TP_HAS(功能) 提问。
 *   反面例子（2026-09 之前 main.c / camera_driver.h 里的写法）：
 *     #define CAMERA_SKIP_UI (CAMERA_TEST_PROFILE == CAMERA_TEST_UVC_ONLY || \
 *                             CAMERA_TEST_PROFILE == CAMERA_TEST_UVC_H264_ONLY || ...)
 * 加一档要同步改 5 处，且改漏了不会报错，只会静默多开或少开一个子系统。
 *
 * 0 = 完整系统
 * 1 = 仅 UVC/USB Host
 * 2 = UVC/USB Host + UI/LVGL
 * 3 = UVC/USB Host + UI/LVGL + SD/GIF
 * 4 = UVC/USB Host + UI/LVGL + SD/GIF + WiFi/ESP-Hosted
 * 5 = 上述模块 + H.264/WebSocket（不启动天气 HTTPS）
 * 6 = UVC/USB Host + JPEG/H.264 编码，不启动 WiFi/ESP-Hosted/WebSocket/UI/SD
 * 7 = UVC/USB Host + JPEG 硬件解码，不启动 YUV/H.264/WiFi/WebSocket/UI/SD
 * 8 = UVC/USB Host + JPEG 解码/YUV 转换，不启动 H.264/WiFi/WebSocket/UI/SD
 */
#define CAMERA_TEST_FULL                   0
#define CAMERA_TEST_UVC_ONLY               1
#define CAMERA_TEST_UVC_UI                 2
#define CAMERA_TEST_UVC_UI_SD              3
#define CAMERA_TEST_UVC_UI_SD_WIFI         4
#define CAMERA_TEST_UVC_UI_SD_WIFI_VIDEO   5
#define CAMERA_TEST_UVC_H264_ONLY          6
#define CAMERA_TEST_UVC_JPEG_ONLY          7
#define CAMERA_TEST_UVC_YUV_ONLY           8

/* ← 改这一行切换档位 */
#define CAMERA_TEST_PROFILE                CAMERA_TEST_FULL

/* 完整系统基线开启 H.264 视频编码和 WebSocket 视频上传；
 * 设为 0 只关闭视频流，保留小智音频使用的同一条 WebSocket 连接。 */
#define CAMERA_VIDEO_STREAM_ENABLED        1

/* 纯 UVC 冷启动测试一次只允许请求一种模式。修改下面最后一行后必须重新
 * 编译、复位开发板并让摄像头重新枚举，禁止在同一次运行中轮换分辨率。 */
#define CAMERA_UVC_COLD_TEST_640X480        1
#define CAMERA_UVC_COLD_TEST_1280X720       2
#define CAMERA_UVC_COLD_TEST_MODE           CAMERA_UVC_COLD_TEST_640X480

/* 功能位。档位号本身不参与任何生产代码的判断，判断只认这些位。 */
#define TP_UVC      (1u << 0)   /* USB Host + UVC 取流（目前恒开，留位以备"无摄像头"档位） */
#define TP_HANDOFF  (1u << 1)   /* 把 UVC 帧复制后交给 video_streamer 下游 */
#define TP_UI       (1u << 2)   /* LVGL 显示任务 */
#define TP_SD       (1u << 3)   /* TF 卡 + GIF 轮播 */
#define TP_WIFI     (1u << 4)   /* Network Manager / ESP-Hosted / C6 */
#define TP_WEATHER  (1u << 5)   /* 天气 HTTPS 和首页信息 */
#define TP_XIAOZHI  (1u << 6)   /* 板载 ES8311 麦克风/扬声器和小智语音会话 */

/* 档位 → 功能位，唯一的映射点。 */
#if   CAMERA_TEST_PROFILE == CAMERA_TEST_FULL
#  define TP_BITS (TP_UVC | TP_HANDOFF | TP_UI | TP_SD | TP_WIFI | TP_WEATHER | TP_XIAOZHI)
#elif CAMERA_TEST_PROFILE == CAMERA_TEST_UVC_ONLY
#  define TP_BITS (TP_UVC)
#elif CAMERA_TEST_PROFILE == CAMERA_TEST_UVC_UI
#  define TP_BITS (TP_UVC | TP_UI)
#elif CAMERA_TEST_PROFILE == CAMERA_TEST_UVC_UI_SD
#  define TP_BITS (TP_UVC | TP_UI | TP_SD)
#elif CAMERA_TEST_PROFILE == CAMERA_TEST_UVC_UI_SD_WIFI
#  define TP_BITS (TP_UVC | TP_UI | TP_SD | TP_WIFI)
#elif CAMERA_TEST_PROFILE == CAMERA_TEST_UVC_UI_SD_WIFI_VIDEO
#  define TP_BITS (TP_UVC | TP_HANDOFF | TP_UI | TP_SD | TP_WIFI)
#elif CAMERA_TEST_PROFILE == CAMERA_TEST_UVC_H264_ONLY || \
      CAMERA_TEST_PROFILE == CAMERA_TEST_UVC_JPEG_ONLY || \
      CAMERA_TEST_PROFILE == CAMERA_TEST_UVC_YUV_ONLY
/* 6/7/8 由 VIDEO_STREAM_*_TEST 在 video_streamer 内部裁剪编码阶段，
 * 这里只负责把帧交下去（详见 video_streamer.h 的说明）。 */
#  define TP_BITS (TP_UVC | TP_HANDOFF)
#else
#  error "未知的 CAMERA_TEST_PROFILE"
#endif

/* 生产代码只用这一个：编译期常量，仍走 #if，所以隔离档位的代码剪裁效果不变，
 * 历史读数（callback_gap_max 等）可以直接对照。 */
#define TP_HAS(feature)     ((TP_BITS & TP_##feature) != 0)

static inline const char *test_profile_name(void)
{
#if   CAMERA_TEST_PROFILE == CAMERA_TEST_FULL
    return "完整系统";
#elif CAMERA_TEST_PROFILE == CAMERA_TEST_UVC_ONLY
    return "仅 UVC/USB Host";
#elif CAMERA_TEST_PROFILE == CAMERA_TEST_UVC_UI
    return "UVC + UI/LVGL";
#elif CAMERA_TEST_PROFILE == CAMERA_TEST_UVC_UI_SD
    return "UVC + UI/LVGL + SD/GIF";
#elif CAMERA_TEST_PROFILE == CAMERA_TEST_UVC_UI_SD_WIFI
    return "UVC + UI/LVGL + SD/GIF + WiFi";
#elif CAMERA_TEST_PROFILE == CAMERA_TEST_UVC_UI_SD_WIFI_VIDEO
    return "UVC + UI/LVGL + SD/GIF + WiFi + H.264/WebSocket（无天气）";
#elif CAMERA_TEST_PROFILE == CAMERA_TEST_UVC_H264_ONLY
    return "UVC + JPEG/H.264 编码（无 WiFi/WS/UI/SD）";
#elif CAMERA_TEST_PROFILE == CAMERA_TEST_UVC_JPEG_ONLY
    return "UVC + JPEG 硬解（无 YUV/H.264/WiFi/WS/UI/SD）";
#elif CAMERA_TEST_PROFILE == CAMERA_TEST_UVC_YUV_ONLY
    return "UVC + JPEG 解码/YUV 转换（无 H.264/WiFi/WS/UI/SD）";
#endif
}

#endif /* LUMMISS_TEST_PROFILE_H */
