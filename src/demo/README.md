# Lummiss ESP32-P4 基础工程

本工程基于 ESP-IDF 5.5.5 和 FreeRTOS，是后续盆栽陪伴机器人功能的开发基础。厂商示例保留在 `../esp_draw_bit`，不在其中继续编写产品功能。

## 当前功能

- UI Task 初始化 GMT020-02-8P/ST7789 和 LVGL 8.4。
- 屏幕横屏分辨率为 320×240，显示动态时间首页：日期、星期、天气、温度和大号时间；电量等电池管理完成后再接入。
- 首页服务联网后通过公网 IP 自动取得坐标和时区，使用网络校时并从 Open-Meteo 获取当前天气；时间每秒刷新，天气每 30 分钟刷新。
- Camera Task 初始化 ESP32-P4 高速 USB Host 和 UVC 驱动。
- LRCPG720p 接入后优先使用 800×600 MJPEG；设备声明 30/25/15 FPS，没有 20 FPS 离散 MJPEG 档位，当前自动选择 ISOC alt=1，并每 5 秒输出视频链路统计。
- 摄像头帧缓冲使用 PSRAM；首页图像作为 RGB565 资源嵌入固件。
- Network Manager 通过 ESP-Hosted/SDIO 控制板载 ESP32-C6。首次使用或清除凭据后，设备以 BLE 广播等待 App 下发 WiFi；已有凭据时直接以 STA 模式连接并通过 DHCP 获取 IPv4 地址。
- WiFi 断开后每 2 秒自动发起重连；连接状态和 IP 信息只由 Network Manager 对外提供。
- Video Streamer 从完整 MJPEG 帧中处理 800×600@20 FPS 上限，经 P4 硬件 JPEG 直出 YUV422、CPU 分块色度抽样/重排和 H.264 硬件编码，再由独立上传任务通过 WebSocket 二进制帧发送（每帧前置 16 字节自描述头携带分辨率与帧率）。当前编码参数为 800×600、4 Mbps、GOP 20；实测编码约 15～20 FPS。
- **上传目标已改为云端**：地址与动态 Token 来自 OTA 结果，当前为 `wss://www.lummiss.com/server/lummiss/v1/`，用 `Device-Id` / `Client-Id` / `Authorization: Bearer <token>` 三个头鉴权；`ota_client` 现在会拒绝非 `wss://` 的返回值。**代码里已无局域网地址**，PC 端 `tools/pc_camera_server.py`（8000 预览页 / 8001 WS）仍在但已无设备连接，本文件下方的旧描述属于历史。
- 新增 `components/xiaozhi_audio`：板载 ES8311 麦克风/扬声器 + Opus 编解码 + 小智协议（hello/listen/tts/stt/ping/pong）+ **AFE 唤醒词检测**（"Hi, Lummiss" 唤醒词，使用 ESP-SR Hi-Max 模型，从 SPIFFS "model" 分区加载）。唤醒词检测在 WebSocket 连接后自动启用，检测到后回调触发聊天上行，回答结束后自动恢复监听。**它不自己建连接**，与视频共用 `video_streamer` 持有的同一条 Agent WebSocket。

## 2026-09-15 最新验证状态

**视频 + 语音已同时跑在云端那条 WSS 上，语音链路本身能通；唤醒词引擎已就绪。**

唤醒词引擎基于 ESP-SR AFE + Hi-Max 模型（`espressif/esp-sr: ~2.3.0`），从 1 MB SPIFFS
"model" 分区加载。初始化在 `xiaozhi_audio_start()`（主任务上下文）中完成——不能在 `service_task`
（7KB 栈）中调用，因为 `esp_srmodel_init` → `spi_flash_mmap` 需要临时禁用另一核的缓存，
7KB 栈不够。唤醒词检测到后停止 MIC 上行并触发聊天，回答结束后自动恢复监听。

语音链路端到端打通的证据（**摄像头断开**时）：
`小智开始回答` → `小智回答文本：{"type":"tts","state":"sentence_start",...}` →
`小智回答结束，恢复麦克风上行`，`SPK frames` 0→62→85、`drop=0`，`mic peak=11676`。

```text
上行：16 kHz 单声道 60 ms Opus，约 106 字节/包 ≈ 14 kbps，每 5 秒 +84 包
下行：24 kHz 单声道 60 ms Opus
判据：SPK frames=0 且 drop=0 = 服务端没发（不是本地解码失败）
      mic peak 几百是环境底噪（284~544），说话时才是 11676，别去调 30 dB 增益
      本版服务端不发 stt，只能用回答内容判断识别结果
```

**A/B（同一次烧录、唯一差别是摄像头插着与否）**：

| 摄像头 | 服务端回答 | 下行 |
| --- | --- | --- |
| 开着 | "主人，lummiss现在有点忙" / "我们稍后再试吧"（兜底话术） | `SPK frames=62`，随后 `send_fail=1`、TTS 结束后 WS 断 |
| 断开 | "我一直都在呢，您请说。"（真实回答） | `SPK frames=46 drop=0` |

候选根因有两个，**尚未区分**：(a) 协议层 —— 后端文档要求"一个 Binary payload = 一个完整
Opus packet"，而我们把每帧十几 KB 的 H.264 也发在同一条连接上，服务端无法区分；
(b) 资源层 —— 编解码链路每帧约 30 ms 的 PSRAM 密集访问把上行音频挤晚。区分办法：保留
整条编解码链路但跳过 `video_streamer.c:1200` 的发送，看语音是否恢复。详见
`../../../PROJECT_HANDOFF.md` 第 10 节与附录 A.7。

**另一个独立问题：AES 的 DMA 描述符分配失败会拖垮 TLS，进而断掉整条链路。**
报错链是 `esp-aes: Failed to allocate memory ...` → TLS 写失败 → WS 断连 →
`JPEG_DEC` 从 15 ms 涨到 662 ms、编码掉到 0 fps。已改为 `CONFIG_MBEDTLS_HARDWARE_AES=n`
（P4 上 AES 恒走 DMA 描述符路径、没有 Kconfig 可关，只有整个关掉硬件 AES 才能摘掉它），
**待复测**，判据是串口里不再出现 `esp-aes:`。这条**不是**上面 A/B 的根因——摄像头断开那一场
也有同样的报错而语音是好的。推导过程见附录 A.8。

## 2026-09-16 HBVCAM UVC 固定冷启动测试

新摄像头 `HBVCAM CAMERA`（VID `058F`、PID `3822`）已在电脑端确认支持 MJPEG。
纯 UVC 诊断固件在 `main/test_profile.h` 中使用编译期固定模式：

```c
#define CAMERA_UVC_COLD_TEST_MODE CAMERA_UVC_COLD_TEST_640X480
```

另一个可选档位是 `CAMERA_UVC_COLD_TEST_1280X720`。每次切换分辨率都要重新构建并冷启动/重新枚举，
同一次运行不会自动轮转格式。USB 参数保持 `alt=1`、`effective_MPS=3072`、`8×16 KB URB`、
6 packets/transfer 和 26 字节 Probe/Commit。

640×480 的两轮日志已证明 SOI、EOI、FID 和 EOF 都存在；此前 `frame_len` 停在约 2136 字节的原因是
普通 `actual_num_bytes==0` ISOC 包被错误地设置成 `skip_current_frame`。现已修复为只统计并忽略，
并增加空包/活动帧中止、header-only、PTS、组帧原因和 128 条关键事件 ring 诊断。

构建成功后，640×480 实机复测应确认空包之后 `frame_len` 继续增加，并观察：

```text
[UVC_EMPTY] ... abort_by_empty=0 ... abort_by_header_only=0
[UVC_BOUNDARY] ... finish EOF/FID/EOI 有效计数
UVC 本次启动成功
```

## 2026-09-12 验证状态（历史）

当前烧录档位为 `CAMERA_TEST_FULL=0`（WiFi + LVGL + SD GIF + WebSocket 全开）。UVC 丢帧的
可控变量已于本日由**同一次运行内的 A/B** 定位为**编解码链路自身的 PSRAM 流量**：链路关闭时
丢帧 2.1%、complete 29.4 fps；链路运行（16 fps）时丢帧 23.8%、complete 22.9 fps，而
`callback_gap_max` 两种状态都是 9 ms。WiFi、LVGL/GIF 轮播、回调 memcpy 在该窗口内均在运行，
已被排除。完整推导见 `../../../PROJECT_HANDOFF.md` 第 7 节与附录 A.5。

```text
芯片：ESP32-P4 revision v1.3
UVC：800×600 MJPEG，requested 30 FPS，自动选择 ISOC alt=1，effective_MPS=3072
URB：8 × 16 KB（MPS=3072 向上对齐为 18432 B/个，合计 144 KiB）；BULK=0
USB DMA 内存：CONFIG_USB_HOST_DWC_DMA_CAP_MEMORY_IN_PSRAM=y（必须）
MJPEG handoff：3 × 256 KB
UVC complete：20～30 FPS（编解码链路关闭时可到 29～30 FPS）
UVC drop：19%～29%（编解码链路关闭时只有 0%～6%）
encoded：15～20 FPS ← 20 FPS 目标未达成
JPEG_DEC：约 6.7 ms
YUV_CONV：约 15.3 ms
H264_ENC：约 8.3 ms
callback_gap_max：9 ms —— 与丢帧率不相关，不要再用它做判据
invalid：约 2.2 帧/秒（≈ 完整帧的 10%），camera_driver 的 SOI/EOI 浅检查看不到
发送：sent == encoded，send_fail=0 —— 上传路径不是瓶颈
```

**`CONFIG_USB_HOST_DWC_DMA_CAP_MEMORY_IN_PSRAM` 必须为 `y`。** 内部 DMA 池只有
146 KiB（启动日志 `Reserving pool of 146K of internal memory for DMA/internal
allocations`），而 8 个 URB 要 144 KiB。codec-only 档位下摄像头是第一个抢池子的，144 挤
146 刚好够，所以那里可以落内部 RAM 并做到 0% 丢帧；**完整档位的 WiFi(SDIO)/LVGL/LCD SPI
会先占用同一个池，分配必然失败**，而失败又会被 `uvc_host.c` 的 double-free（约 341 行
`uvc_transfers_free()` 释放后不置空，被错误路径二次调用）放大成开机重启循环。

注意这条配置防的是**开机重启循环**，与上面的稳态丢帧是两回事——完整档位下 URB 本来就落在
PSRAM，而编解码链路一关，丢帧立刻从 23.8% 掉到 2.1%。

下一步是降低编解码链路的 PSRAM 压力（YUV 重排 15.3 ms 是三项里最大的一项）。**不要再做
档位二分**：`callback_gap_max` 在丢帧 23.8% 和 2.1% 两种状态下都是 9 ms，对档位不敏感。
原先"丢帧在启动约 34 s 后跳变、与 ESP-TLS / GIF 轮播重合"的线索也已证伪——后续日志里
那些事件照常发生而丢帧降到 2.1%。

## 2026-09-11 验证状态（历史）

当天烧录的是隔离档位 `CAMERA_TEST_UVC_H264_ONLY=6`，暂停 UI/LVGL、天气 HTTPS、
WiFi/ESP-Hosted 和 WebSocket，只保留 UVC、JPEG 硬解、CPU YUV 转换和 H.264 硬编。
下列数字属于该档位，**不代表完整档位**。

```text
URB：96 × 32 KB（当时配置，后改为 8 × 16 KB）
UVC complete：约 19～21 FPS
UVC drop：约 31.6%～36.7%
H.264 实际：约 15 FPS
callback_gap_max：10 ms
```

CPU YUV 转换现在按 8 个 2 行宏块分块，块间执行 `taskYIELD()` 并插入 20 µs 短空隙，以缩短连续 PSRAM 访问窗口。该优化保持输出格式不变，但当前短测尚未证明能降低 UVC 丢帧。

P4 v1.3 + ESP-IDF 5.5.5 上，DMA2D/PPA 的 YUV422→YUV420 路径返回 `ESP_ERR_NOT_SUPPORTED`，因此代码安全回退 CPU；不要移除版本判断强行调用私有 DMA2D API。首帧超时路径已改为等待所有 USB transfer 回调退出后完整重建 USB Host/UVC，最近一次重建后首帧耗时约 249 ms。

## 主组件结构

```text
components/network/
├── network_manager.c/.h     网络总入口、NVS、netif、连接状态和 DHCP 结果
├── wifi_manager.c/.h        esp_wifi_remote 事件、连接和自动重连
├── CMakeLists.txt           Network Manager 组件依赖
└── idf_component.yml        ESP-Hosted/esp_wifi_remote 版本要求

components/provisioning/
├── provisioning_manager.c/.h  BLE 配网、Security 2、设备名和每设备 PoP
├── Kconfig.projbuild           仅供开发验证的强制配网开关
└── CMakeLists.txt              官方 network_provisioning 组件依赖

components/video_streamer/
├── video_streamer.c/.h      JPEG 解码、YUV 重排、H.264 硬件编码；并持有唯一的 Agent
│                            WebSocket（视频二进制帧 + 小智文本/Opus 都走它）
├── CMakeLists.txt           esp_websocket_client 等依赖
└── idf_component.yml        esp_h264 1.4.0、esp_websocket_client 版本要求

components/xiaozhi_audio/
├── xiaozhi_audio.c/.h       ES8311 采集/播放、Opus 编解码和小智协议
├── wake_word.c/.h           AFE 唤醒词检测（Hi-Max 模型），16 kHz 单声道输入
└── CMakeLists.txt           esp_audio_codec、esp_codec_dev、esp-sr 依赖

components/ota_client/
├── ota_client.c/.h          取 OTA 结果里的 WSS 地址与动态 Token（串口只打长度不打正文）
└── CMakeLists.txt           HTTPS/JSON 依赖

components/home_info/
├── home_info.c/.h           自动定位、网络校时、天气 API 和线程安全快照
└── CMakeLists.txt           HTTPS、JSON 与 Network Manager 依赖

main/
├── main.c                    唯一 app_main()，创建 FreeRTOS 任务；先跑 OTA，再把结果
│                             交给 video_streamer，然后按档位启动小智/天气/摄像头
├── test_profile.h            启动组合档位的唯一定义点（档位 0~8 与功能位映射）
├── display_driver.c/.h       ST7789、LVGL 和动态时间天气首页
├── screen_carousel.c/.h      TF 卡表情轮播（当前是 anim_bin_player 的 RGB565 BIN 双缓冲）
├── camera_driver.c/.h        USB Host 和 UVC 摄像头管理
└── idf_component.yml         管理组件版本
```

`main.c` 当前创建：

| 任务 | 优先级 | 栈大小 | 职责 |
| --- | ---: | ---: | --- |
| `ui_task` | 6 | 8192 字节 | 初始化屏幕和 LVGL；后续接收 UI 事件 |
| `camera_task` | 7 | 8192 字节 | 运行 USB Host/UVC 摄像头驱动 |

UVC 组件还会创建 USB 事件任务和驱动后台任务。所有显式 LVGL 初始化都由 UI Task 发起，摄像头任务不直接操作 LVGL。

`components/xiaozhi_audio` 内部创建以下任务（由 `xiaozhi_audio_start()` 触发）：

| 任务 | 核心 | 优先级 | 栈大小 | 职责 |
| --- | ---: | ---: | ---: | --- |
| `xiaozhi_service` | CPU0 | 5 | 7168 字节 | 网络事件、WS 状态同步、Opus 编码与上行 |
| `xiaozhi_mic` | CPU0 | 6 | 5120 字节 | ES8311 I2S 接收、PCM 缓冲、周期性统计 |
| `xiaozhi_spk` | CPU0 | 6 | 4096 字节 | ES8311 I2S 发送 |
| `xiaozhi_decode` | CPU1 | 9 | 10240 字节 | 下行 Opus 解码 |
| `wake_detect` | CPU1 | 3 | 5120 字节 | AFE 唤醒词检测（Hi-Max 模型，16 kHz） |

`app_main()` 对网络只调用 `network_manager_init()` 和 `network_manager_start()`；WiFi 系统事件、DHCP 状态、BLE 配网和重连逻辑均封装在 `components/network` 与 `components/provisioning` 中。固件不再包含固定 SSID 或密码，WiFi 凭据由 App 经 BLE 下发并保存在板载 C6 的 Flash。App 对接协议见 `BLE_WIFI_PROVISIONING.md`。

## ESP32-P4 与 ESP32-C6

当前参数按本开发板厂商示例配置：

| ESP-Hosted SDIO 信号 | ESP32-P4 GPIO |
| --- | ---: |
| CLK | 18 |
| CMD | 19 |
| D0 | 14 |
| D1 | 15 |
| D2 | 16 |
| D3 | 17 |
| C6 RESET | 54，高电平有效 |

SDIO 使用 Slot 1、4-bit、40 MHz。依赖锁定结果为 `esp_hosted 2.7.4` 和 `esp_wifi_remote 1.3.0`。

## 屏幕接线

| 屏幕引脚 | ESP32-P4 |
| --- | --- |
| GND | GND |
| VCC | 3V3 |
| SCL/SCLK | GPIO20 |
| SDA/MOSI | GPIO32 |
| RST/RES | GPIO3 |
| DC | GPIO2 |
| CS | GPIO1 |
| BL/BLK | 3V3 |

BL 直接接 3V3，当前程序不能控制背光亮度。

## 构建与烧录

在普通终端中：

```powershell
Set-Location E:\Lummiss_Plant_Robot\src\demo
cmd /c _build_main.bat
```

启动 PC 视频接收服务器：

```powershell
.\_start_camera_server.bat
```

首次运行会自动安装 `tools/camera_server_requirements.txt` 中的 PyAV、OpenCV 和 websockets。服务器启动后访问 `http://127.0.0.1:8000/`；页面显示H.264接收帧率、浏览器预览帧率和预览队列丢帧数，并提供下发命令按钮。

⚠️ **这一段是历史**：视频帧原本走局域网 `ws://<PC>:8001/ws`，HTTP（8000）负责预览页与下行命令 `/cmd?cmd=ping|status`。当前固件的上传目标由 OTA 下发（`wss://www.lummiss.com/server/lummiss/v1/`），**PC 服务器还能起，但不会有设备连它**，页面会一直是 0 帧。要用它做本地预览，需要让 `video_streamer` 回到本地地址。

在已经激活 ESP-IDF 5.5.5 的终端中：

```powershell
idf.py -B build build
idf.py -B build -p COM17 flash monitor
```

VS Code 工作区已默认使用 `build`、`sdkconfig.bletest` 和 Ninja。该配置为正常联网模式：已有 Wi-Fi 凭据时不会强制启动 BLE 配网。

2026-09-11 CPU 分块转换版本的联合构建结果：

```text
lummiss_main.bin：0xb2d60
最小应用分区剩余：91%
bootloader.bin：0x5310
构建结果：通过
```

## 烧录后的正常现象

1. 串口出现 `APP_MAIN`，随后可看到 ESP-Hosted 初始化和 SDIO 与 C6 建链日志。
2. C6 已保存凭据时，出现 `C6 已保存 Wi-Fi 凭据，无需启动 BLE 配网`、`WiFi STA 已启动` 和连接日志。首次启动时则广播 `LUMMISS_XXXXXX`，等待 App 完成 BLE 配网。
3. 成功连接后出现 `WiFi 联网成功`，并输出非 `0.0.0.0` 的 IPv4、网关和掩码。这表示 P4→C6→路由器→DHCP 链路完整成功；首次配网还会在约 5 秒后关闭 BLE 广播，再启动 OTA、UI 和视频任务。
4. 屏幕黑底横屏显示时间首页，文字方向正常且不再左右镜像；当前不会显示电量。
5. 联网前页面显示占位符；联网后串口依次出现“网络时间同步成功”“自动定位成功”和“天气更新”，随后页面显示当前日期、星期、时间、天气和温度。
6. 摄像头插入高速 USB 口后，串口显示 `800x600 MJPEG`、自动选择 `ISOC alt=1` 和 `Stream started`。热复位后首流可能为 0 帧，完整停止并重建后可恢复；最近一次首帧耗时约 249 ms。
7. 完整联网档位下，`VIDEO_STREAM` 每 5 秒汇总一次，实际编码通常约 15 FPS；当前 CPU 分块版本仍需重点观察 UVC 丢帧、H.264 耗时、发送失败和网页预览丢帧，不能预期仅靠分块达到 20 FPS。
8. OTA 完成后串口出现 `OTA 响应已接收（N 字节）`（**只打长度，不打正文**——正文含动态 Token），随后 `WS 已连接`。视频与小智共用这一条连接。
9. `CAMERA_TEST_FULL`（档位 0）会启动小智：串口出现 `小智` 相关日志和 `MIC packets` / `SPK frames` 周期统计。说话时 `mic peak` 应到一万以上；服务端回话时 `SPK frames` 增长且 `drop=0`。**当前摄像头开着时语音会失效**（服务端回兜底话术），详见上方 2026-09-15 状态。
10. 唤醒词引擎就绪时串口输出 `唤醒词引擎已就绪，等待 WebSocket 连接后启用`；WebSocket 连接后自动启动检测。若初始化失败则输出 `唤醒词引擎初始化失败，将退化为无唤醒词模式`，此时 MIC 上行在连接建立后立即开始。

密码错误或路由器不可达时，串口会反复出现 `WiFi 已断开`、原因码和 2 秒后重连。若在这些日志之前就出现 Hosted/SDIO 初始化失败，应先检查板载 C6 固件；厂商提供的参考固件位于 `开发板示例/JC1060P470C_I_W_Y/8-Burn operation/Burn files/JC-C6-slave_v2.3.2.bin`。

PC 服务器的启动、热复位恢复实验、JPEG 完整性统计、颜色验收和实时速率判断见 `CAMERA_UPLOAD_TEST.md`。H.264 实时帧已从 HTTP/1.1 长连接改为 WebSocket 二进制帧传输（8001），每帧前置 16 字节自描述头携带分辨率与帧率，PC 端据此自适应；HTTP 侧保留 `/h264` 回退入口与下行命令通道。H.265 不受当前 ESP32-P4 编码器支持。ESP-Hosted Host 2.7.x 与板载 C6 2.3.x 存在版本警告，当前联网和视频传输正常，后续应升级 C6 固件并做回归测试。
