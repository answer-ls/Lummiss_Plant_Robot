# Lummiss 盆栽陪伴机器人

截至 2026-10-08，主固件位于 [`../src/demo/`](../src/demo/README.md)，使用 ESP-IDF 5.5.5，面向 ESP32-P4 v1.3 新 PCB；ESP32-C6 通过 ESP-Hosted/SDIO 提供 Wi-Fi 和 BLE。当前 `CAMERA_TEST_PROFILE=CAMERA_TEST_FULL`（档位 0），YOLO 关闭，摄像头随 RTC 预览按需启动。旧实验的完整过程和未决问题见 [`PROJECT_HANDOFF.md`](PROJECT_HANDOFF.md) 顶部“当前快照”。

## 当前链路

| 功能 | 实现 | 当前状态 |
| --- | --- | --- |
| 云端 | MQTT 控制/MCP、UDP 加密 Opus、小智会话、WHIP/WebRTC H.264 | 基本链路已在实机跑通；RTC 帧率和长时间稳定性仍待优化 |
| 音频 | ES7210 双麦 MIC1/MIC2 加播放参考输入 AFE，ES8311 播放，Opus 60 ms 帧 | 独立双麦工程连续对话效果好；主工程已对齐 AFE 输出缓冲及 Opus 编码任务，识别效果仍需同条件复测 |
| 视频 | HBVCAM USB UVC 1280×720 MJPEG@30 → JPEG 硬解 → CPU YUV422→YUV420 → H.264 → WebRTC | 最近一次预览 UVC 完整帧约 19.6～23.6 fps、丢帧约 19.9%～33.8%；编码/发送约 10～11.5 fps，未达 20 fps 目标 |
| TF 卡/表情 | 新 PCB SPI2 SDSPI，启动早期挂载 `/sdcard`；表情文件位于 `/sdcard/expressions/` | 120 MiB 卡可挂载并读扇区 0，但完整系统运行后读表情文件会报 `0x107`/`errno=5`；独立工程保持挂载约 90 秒后复读同一文件成功，差异尚待隔离 |
| 配网 | C6 保存凭据；无凭据时 BLE Security 2 配网 | 既有凭据直接联网；强制配网开关当前关闭 |

RTC 预览的 UVC 丢帧、编码端限速和浏览器实际播放帧率应分别统计。历史 800×600 同次运行 A/B 显示关闭编解码时 UVC 丢帧从 23.8% 降到 2.1%，说明负载相关；这**没有证明** PSRAM 仲裁的具体机理，也没有摄像头温度数据。此前把丢帧直接归因于 TLS/GIF 事件的解释已经撤回，详见 [`VIDEO_20FPS_VALIDATION.md`](../src/demo/markdown合集/VIDEO_20FPS_VALIDATION.md)。

## 工程入口

- [`../src/demo/README.md`](../src/demo/README.md)：当前配置、主要模块和构建方法。
- [`PROJECT_HANDOFF.md`](PROJECT_HANDOFF.md)：当前快照及按日期保留的诊断依据；旧章节中的“当前”按其记录日期理解。
- [`../src/sdspi_official_test/`](../src/sdspi_official_test/main/main.c)：官方 SDSPI 流程的独立对照。
- [`../src/xiaozhi_dual_mic_reference_test/`](../src/xiaozhi_dual_mic_reference_test/README.md)：双麦连续对话对照。

手动保存的构建、串口和诊断日志统一放在项目根目录 `logs/`。固件修改后使用 `src/demo/build_rtc_mem_b` 编译，同时保持 `src/demo/build` 可用；烧录需由测试者明确执行。
