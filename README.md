# Lummiss 盆栽陪伴机器人

基于 **ESP32-P4**（v1.3）的桌面植物陪伴机器人：表情屏幕交互 + USB 摄像头实时视频 + WiFi 云端联通。

## 功能
- **表情屏**：ST7789（240×320，横屏 320×240）表情动画，多种情绪切换。
- **实时视频**：LRCPG720p USB UVC 摄像头（MJPEG）→ ESP32-P4 硬件 JPEG 解码 → CPU 分块 YUV422→YUV420 重排 → esp_h264 硬件编码 → WebSocket 二进制帧上传，800×600、4 Mbps、实测 15～20 fps。
- **语音对话**：板载 ES8311 麦克风/扬声器 + Opus 编解码 + 小智协议，经 OTA 下发的 `wss://` 地址与云端双向传输（上行 16 kHz、下行 24 kHz，60 ms 包）。
- **联网**：ESP32-C6 网络协处理器（ESP-Hosted，SDIO）连接 WiFi，支持 App 经 BLE 下发凭据。

## 当前状态与已知问题

视频和语音**共用同一条云端 WebSocket**（地址与动态 Token 由 OTA 下发）。语音链路已端到端
打通，但**摄像头开着时语音会失效**（服务端只回兜底话术）——已由 A/B 确认，根因待定位。
另有 AES 硬件加速的 DMA 内存问题会拖垮 TLS 连接。详见
[`PROJECT_HANDOFF.md`](PROJECT_HANDOFF.md) 第 10 节与附录 A.7 / A.8。

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
