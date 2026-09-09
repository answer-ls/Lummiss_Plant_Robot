# Lummiss 盆栽陪伴机器人

基于 **ESP32-P4**（v1.3）的桌面植物陪伴机器人：表情屏幕交互 + USB 摄像头实时视频 + WiFi 云端联通。

## 功能
- **表情屏**：ST7789（240×320，横屏 320×240）表情动画，多种情绪切换。
- **实时视频预览**：LRCPG720p USB UVC 摄像头（MJPEG）→ ESP32-P4 硬件 JPEG 解码 → esp_h264 硬件编码 → HTTP 上传，800×600@15fps。
- **联网**：ESP32-C6 网络协处理器（ESP-Hosted，SDIO）连接 WiFi。

## 目录
| 路径 | 说明 |
|---|---|
| `src/demo/` | 主固件工程（ESP-IDF v5.5.5），含视频流水线、摄像头/屏幕驱动、表情资源；构建与烧录步骤见其 `README.md` |
| `PROJECT_HANDOFF.md` | 开发交接/进展记录 |
| `盆栽陪伴机器人_开发文档.md` | 产品开发文档（外包装、需求说明等） |

## 硬件平台
- 主控 ESP32-P4 rev v1.3 + 32MB PSRAM + 16MB Flash
- 摄像头 LRCPG720p USB UVC（MJPEG）
- 屏幕 ST7789 SPI
- 无线 ESP32-C6（ESP-Hosted SDIO）

## 快速开始（固件）
详见 [`src/demo/README.md`](src/demo/README.md)。
