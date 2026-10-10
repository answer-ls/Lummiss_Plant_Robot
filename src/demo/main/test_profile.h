#ifndef LUMMISS_TEST_PROFILE_H
#define LUMMISS_TEST_PROFILE_H

#include "board_pins.h"

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
 * 9 = UI/LCD + 按键/限位/TTP223 + 本地麦克风/扬声器自检，
 *     不启动 UVC/WiFi/WebSocket/SD/小智/天气
 * 10 = GPIO47 低电平读表情文件，10 秒后拉高重读并在屏幕轮播表情
 * 11 = 仅测试ESP-Hosted/C6 WiFi连接与DHCP，不访问SD卡
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
#define CAMERA_TEST_PERIPH_ONLY            9
#define CAMERA_TEST_SD_ONLY               10    // GPIO47 读卡对照与屏幕表情轮播
#define CAMERA_TEST_WIFI_ONLY             11
#define CAMERA_TEST_STEPPER_ONLY          12  /* 仅电机循环两圈往返自检 */
#define CAMERA_TEST_AUDIO_RAW             13  /* ES7210 四槽原始采集 */
#define CAMERA_TEST_ALERT_ONLY            14  /* UVC + YOLO + MQTT 人体预警上报 */

/* ← 改这一行切换档位。 */
#define CAMERA_TEST_PROFILE                CAMERA_TEST_FULL

/* 完整系统欠压排查：分阶段启动并在各阶段保持一段时间观察。
 * 只延后后续模块启动，不禁用欠压保护，也不更改各驱动的工作参数。 */
#define FULL_POWER_DIAG_HOLD_MS             3000
/* 本地人体检测：当前 1280×720 MJPEG 会缩放补边后交给 YOLO11n。 */
#define CAMERA_PERSON_DETECT_ENABLED       0

/* UVC 丢包诊断：完整系统启动时自动打开摄像头并持续收帧，
 * 无需等待 RTC 预览指令；视频编码和上传仍由服务端命令开启。
 * 诊断结束后设回 0，即恢复原来的 RTC 按需启动摄像头。 */
#define CAMERA_UVC_AUTOSTART               0

/* 视频上传采用「按需开启」：开机默认 IDLE，只建立 WebSocket 控制通道，
 * 必须等服务端下发 VIDEO_START 才编码上传；VIDEO_STOP / 断线自动停止，
 * 重连不自动恢复（详见 components/video_streamer/video_streamer.h）。
 *
 * 这个宏只是**调试开关**：设 1 会在摄像头驱动初始化后立刻强制开视频，
 * 用于手边没有服务端时单独看画面。生产/正常联调必须保持 0。 */
#define CAMERA_VIDEO_STREAM_AUTOSTART      0

/* 纯 UVC 冷启动测试一次只允许请求一种模式。修改下面最后一行后必须重新
 * 编译、复位开发板并让摄像头重新枚举，禁止在同一次运行中轮换分辨率。 */
#define CAMERA_UVC_COLD_TEST_640X480        1
#define CAMERA_UVC_COLD_TEST_1280X720       2
#define CAMERA_UVC_COLD_TEST_MODE           CAMERA_UVC_COLD_TEST_1280X720

/* 功能位。档位号本身不参与任何生产代码的判断，判断只认这些位。 */
#define TP_UVC      (1u << 0)   /* USB Host + UVC 取流（目前恒开，留位以备"无摄像头"档位） */
#define TP_HANDOFF  (1u << 1)   /* 把 UVC 帧复制后交给 video_streamer 下游 */
#define TP_UI       (1u << 2)   /* LVGL 显示任务 */
#define TP_SD       (1u << 3)   /* TF 卡 + GIF 轮播 */
#define TP_WIFI     (1u << 4)   /* Network Manager / ESP-Hosted / C6 */
#define TP_WEATHER  (1u << 5)   /* 天气 HTTPS 和首页信息 */
#define TP_XIAOZHI  (1u << 6)   /* 板载 ES8311 麦克风/扬声器和小智语音会话 */
#define TP_PERIPH   (1u << 7)   /* 机械按键 + TTP223 触摸 + WS2812 氛围灯（纯驱动自检） */

/* 档位 → 功能位，唯一的映射点。
 *
 * 档位 0（完整系统）暂时不带 TP_PERIPH：RMT 通道和三个采样/灯效任务都会新增
 * 内部内存占用，而当前第一优先级问题正是内部内存被挤碎，所以在长测通过前
 * 不让完整档位多背一块负担。要在完整系统里也启用，把下面完整系统那一行的
 * TP_BITS 末尾加上 " | TP_PERIPH" 即可，没有别的改动。 */
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
#elif CAMERA_TEST_PROFILE == CAMERA_TEST_SD_ONLY
#  define TP_BITS (TP_UI | TP_SD)
#elif CAMERA_TEST_PROFILE == CAMERA_TEST_STEPPER_ONLY || CAMERA_TEST_PROFILE == CAMERA_TEST_AUDIO_RAW
#  define TP_BITS (0)
#elif CAMERA_TEST_PROFILE == CAMERA_TEST_WIFI_ONLY
#  define TP_BITS (TP_WIFI)
#elif CAMERA_TEST_PROFILE == CAMERA_TEST_ALERT_ONLY
#  define TP_BITS (TP_UVC | TP_HANDOFF | TP_SD | TP_WIFI)
#elif CAMERA_TEST_PROFILE == CAMERA_TEST_PERIPH_ONLY
#  define TP_BITS (TP_UI | TP_PERIPH)
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
#if CAMERA_TEST_PROFILE == CAMERA_TEST_AUDIO_RAW
    return "ES7210 原始四槽 BASE/A/B/C 采集";
#elif CAMERA_TEST_PROFILE == CAMERA_TEST_STEPPER_ONLY
    return "仅步进电机：正反各两圈，每轮间隔10秒";
#elif CAMERA_TEST_PROFILE == CAMERA_TEST_WIFI_ONLY
    return "仅WiFi连接/DHCP（不访问SD卡）";
#elif CAMERA_TEST_PROFILE == CAMERA_TEST_ALERT_ONLY
    return "UVC + YOLO + 20秒H264预警片段 + TF/HTTPS（无 UI/音频/天气）";
#elif CAMERA_TEST_PROFILE == CAMERA_TEST_SD_ONLY
    return "GPIO47 低/高电平读卡对照后轮播表情（无摄像头/WiFi）";
#elif CAMERA_TEST_PROFILE == CAMERA_TEST_FULL
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
#elif CAMERA_TEST_PROFILE == CAMERA_TEST_PERIPH_ONLY
    return "UI/LCD + 按键/限位/TTP223 + MIC/SPK 本地自检（无 UVC/WiFi/WS/SD/小智/天气）";
#endif
}

#endif /* LUMMISS_TEST_PROFILE_H */
