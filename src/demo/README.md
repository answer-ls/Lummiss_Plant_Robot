# Lummiss ESP32-P4 主固件

更新于 2026-10-09。工程使用 ESP-IDF 5.5.5，当前板型为 ESP32-P4 v3.2 新 PCB（`BOARD_USE_NEW_PCB=1`，固件最低芯片版本设为 v3.1）。`main/test_profile.h` 当前选择 `CAMERA_TEST_PROFILE=CAMERA_TEST_ALERT_ONLY`（档位 14），用于验证人体识别后的前 10 秒＋后 10 秒 H.264 片段落 TF 卡和 HTTPS 上传；恢复完整系统时改回 `CAMERA_TEST_FULL`（档位 0）。阶段性诊断和历史方案见 [`PROJECT_HANDOFF.md`](../../问题合集/PROJECT_HANDOFF.md)。

## 模块与工作方式

| 模块 | 当前路径 |
| --- | --- |
| 网络/配网 | P4 通过 ESP-Hosted SDIO 使用 C6 的 Wi-Fi/BLE。C6 已保存凭据时直接连接；无凭据时启动 BLE Security 2 配网。`CONFIG_LUMMISS_FORCE_BLE_PROVISIONING` 当前关闭。 |
| 云端 | OTA 取得配置，MQTT 承载 AI hello、MCP 与 RTC 控制；小智音频使用 UDP 加密 Opus；视频使用 WHIP/WebRTC 单向 H.264。旧版“音视频共用 Agent WebSocket”仅适用于历史固件。 |
| 音频 | 新 PCB 的 ES7210 四槽 TDM 接收 MIC1、播放参考和 MIC2，AFE 双麦 MMR 输出单声道上行，ES8311 负责播放。`components/xiaozhi_audio/xiaozhi_audio.c` 的 `XIAOZHI_SINGLE_MIC_TEST=0`。Opus 编码任务和 AFE 输出缓冲已参照独立双麦测试工程调整，主工程连续对话清晰度仍待实测复核。 |
| 摄像头/RTC | HBVCAM UVC 当前请求 1280×720 MJPEG@30；P4 硬件 JPEG 解码、CPU YUV422→YUV420、硬件 H.264 编码。当前档位 14 持续本地编码并运行 YOLO 识人，不启动 RTC 实时推流或首页。 |
| 屏幕/表情 | ST7789 320×240 横屏、LVGL 首页；表情文件从 `/sdcard/expressions/` 读取。 |
| TF 卡 | 新 PCB 使用 SPI2 SDSPI：CLK42、MOSI43、MISO41、CS44、CD39、GPIO45 低电平上电。启动早期先断电约 100 ms、初始化 SPI2，再上电等待约 200 ms 后挂载。 |

档位 10 在 GPIO47 低电平时挂载 TF 卡并初始化 LCD/LVGL，读取扇区 0 和 `/sdcard/expressions/exp_08.bin` 全文，保持低电平再等待 10 秒，然后拉高 GPIO47 读取同一扇区和文件，记录长度与内容指纹。不启动表情轮播、摄像头、Wi-Fi 或小智。此前低/高电平对照显示卡座 VDD 均约 3.3 V；低电平读卡正常，高电平读卡超时。

档位 14 在启动早期挂载 TF 卡，持续编码 H.264 到 PSRAM 环形缓冲；识人时用同一 `event_id` 上报 `device_alert`，收集后续 10 秒，写入 `/sdcard/alert_<event_id>.h264` 并完整读回校验，再用 `alert.upload.grant` 的一次性票据进行 HTTPS multipart 分块上传。成功后删除文件，失败时保留文件以供排查；本轮只做一次上传尝试，失败文件尚无自动续传。串口按 `CAPTURE_BEGIN`、`CAPTURE_END`、`TF_VERIFY`、`TF_SAVE`、`ALERT_GRANT_RX`、`UPLOAD_RESULT` 核对。此档位不启动 UI、音频或 RTC 实时推流；实际 TF 写入速度、PSRAM 余量和服务器接收结果仍需实机验证。

## 当前实机结论与限制

- **TF 卡**：120 MiB SDSC 卡在主工程启动早期挂载成功，扇区 0 连续读取通过；之后访问表情文件时曾发生底层读扇区 `ESP_ERR_TIMEOUT (0x107)`，FatFS 返回 `errno=5`。这时 `stat` 失败不表示文件一定不存在。相同卡和板在独立 `../sdspi_official_test` 工程中，保持挂载约 90 秒后再次完整读取 `/sdcard/expressions/exp_08.bin` 成功。完整系统负载与独立工程之间的差异仍待隔离。
- **RTC 预览**：最近一次播放器回退后的日志显示 UVC 完整帧约 19.6～23.6 fps，丢帧约 19.9%～33.8%；H.264 编码/发送约 10～11.5 fps，`send_fail=0`。此前另一轮丢帧曾达约 43.9%～55.1%，两轮不是同条件 A/B，不能确定改善来自播放器。表情播放器 SD 读缓存已恢复先前的 PSRAM 方式；RTC 期间并未实际播放动画。
- **丢帧归因**：历史 800×600 同次运行中，关闭编解码链路使 UVC 丢帧从 23.8% 降至 2.1%，支持负载相关，但没有证实 PSRAM 仲裁的具体机理。也没有摄像头温度或冷却对照，不能认定发热为根因。详见 [`VIDEO_20FPS_VALIDATION.md`](markdown合集/VIDEO_20FPS_VALIDATION.md) 文首更正。
- **音频**：独立双麦测试工程的连续对话效果好；主工程已调整任务与缓冲配置，但仍需用相同语句、距离和供电复测多轮 STT。不要把首轮 STT 成功当作连续监听已稳定。

比较预览问题时分别看 UVC `complete/drop`、`handoff rejected`、JPEG 错帧、H.264 `encoded/sent`、`send_fail` 和内部 DMA 最大连续块；浏览器实际播放帧率还需单独确认。TF 卡读取问题则记录首次 `0x107` 前后的模块启动顺序及卡座 VDD。

## 构建

在已激活 ESP-IDF 5.5.5 的 PowerShell 中：

```powershell
Set-Location E:\Lummiss_Plant_Robot\src\demo
idf.py -B build_rtc_mem_b build
```

IDF 扩展当前使用 `build_rtc_mem_b`；保留 `build` 目录供原配置和增量编译使用。修改固件后编译；烧录和串口端口由实机测试时另行选择，不随文档更新执行。手动生成的构建日志、串口日志和诊断日志统一写入项目根目录 `logs/`。

当前配置的核对点：`main/test_profile.h`（档位、摄像头开关），`components/board/include/board_pins.h`（新旧 PCB 与 GPIO），`sdkconfig.defaults`（目标芯片及默认内存配置），`components/sd_card/sd_card_spi.c`（新 PCB SDSPI），`components/xiaozhi_audio/xiaozhi_audio.c`（双麦和 Opus）。C6 从机工程位于 `../c6_hosted_slave_2_7_4`。
