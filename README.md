# Lummiss 盆栽陪伴机器人

基于 **ESP32-P4**（v1.3）的桌面植物陪伴机器人：表情屏幕交互 + USB 摄像头实时视频 + WiFi 云端联通。

## 功能
- **表情屏**：ST7789（240×320，横屏 320×240）表情动画，多种情绪切换。
- **实时视频**：LRCPG720p USB UVC 摄像头（MJPEG）→ ESP32-P4 硬件 JPEG 解码 → CPU 分块 YUV422→YUV420 重排 → esp_h264 硬件编码 → WebSocket 二进制帧上传，800×600、4 Mbps、实测 15～20 fps。
- **语音对话**：板载 ES8311 麦克风/扬声器 + Opus 编解码 + 小智协议，经 OTA 下发的 `wss://` 地址与云端双向传输（上行 16 kHz、下行 24 kHz，60 ms 包）。
- **联网**：ESP32-C6 网络协处理器（ESP-Hosted，SDIO）连接 WiFi，支持 App 经 BLE 下发凭据。

## 当前状态与已知问题

视频和语音**共用同一条云端 WebSocket**（地址与动态 Token 由 OTA 下发）。

**2026-09-15 更正：摄像头推流时语音是通的。** 同一次运行里视频 13.9～15.6 fps 稳定上传
（`send_fail=0`），同时上行 16.7 包/秒不断，服务端正常识别（`stt` 文本"好"）并回了完整回答。
此前"摄像头开着时语音会失效（服务端只回兜底话术）"的结论**不再成立**——那次 A/B 时上传
路径还在抢占/中断，不是摄像头本身。

真正卡住系统的是**内部 DMA 堆耗尽**，不是协议冲突：

- 全系统共用一块开机预留的内部 DMA 区（`Reserving pool of 146K of internal memory`，
  大小由 `CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL` 决定）。运行中它长期只剩
  3～13 KB 空闲、**最大连续块 1 KB**。
- ESP-Hosted 的 SDIO 收包路径要为**每个包**取一块 **1664 字节、64 字节对齐**的
  内部 DMA 缓冲，取不到就 `assert` → panic 重启（`sdio_drv.c:862`
  `assert(pkt_rxbuff)`，非 streaming 分支才有这条断言）。实测 64.8 秒时
  `[VIDEO] MEM DMA=10/1 KB` 之后立刻崩在这一行。
- 同一块池子被挤空时还会连带出：表情动画 `allocate_dma_buf: not enough mem`、
  `transport: STA TX transport buffer unavailable`、TLS `PK verify failed 0x4290`。
- 下行 TTS 播放队列已增加到 24 包，并用 `qovf/derr` 区分队列溢出与解码/写入错误。
  最新对照恢复了 Git 基线的 CPU0/P6 音频任务配置，避免与 CPU1 的 JPEG/YUV/H.264
  长时间争用。新日志进一步确认 H.264 单帧发送可阻塞 1.4～1.9 秒，并导致播放队列
  `qovf=4`；现已开启 WebSocket 独立发送锁，使视频上行阻塞时仍可接收下行 Opus。

详见 [`PROJECT_HANDOFF.md`](PROJECT_HANDOFF.md) 第 10 节与附录 A.7（语音实测口径与更正）、
A.8（AES）、A.9（内部 DMA 池与本次三处改动）。

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
