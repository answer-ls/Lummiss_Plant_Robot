# ESP32-P4 USB 摄像头 H.264 实时回传测试

## 当前实现链路

```text
LRCPG720p USB 摄像头
→ UVC 800×600 MJPEG（设备声明 30 FPS，实测约 22 FPS）
→ ESP32-P4 硬件 JPEG 直接解码为 UYVY/YUV422
→ 软件仅做垂直色度抽样和 O_UYY_E_VYY 字节重排
→ ESP32-P4 H.264 硬件编码，编码上限 20 FPS / 4 Mbps / GOP 20（当前实测约 15 FPS）
→ 每帧前置 16 字节自描述头（magic/宽/高/帧率/帧类型/序号）
→ 独立上传任务通过 WebSocket 二进制帧逐帧发送（当前对照档位关闭网络）
→ ESP-Hosted / ESP32-C6 / WiFi
→ Windows PC 192.168.1.66:8001/ws
→ 保存 Annex-B `.h264` 码流（只落 payload）并生成浏览器预览
```

分辨率与帧率随每帧的 16 字节头携带，PC 端据此自适应几何参数，不需要在连接建立时协商；
因此改分辨率只影响 P4 侧的编译期常量，协议和 PC 端都不用动。

摄像头本身没有 H.264/H.265 输出。YUY2 直采在本机的热复位流程中不稳定，当前主线已改为 MJPEG 加硬件 JPEG 解码。ESP32-P4 当前组件只提供 H.264 硬件编码，本阶段不实现 H.265。

## 启动步骤

先启动电脑服务器：

```powershell
Set-Location E:\Lummiss_Plant_Robot\src\demo
.\_start_camera_server.bat
```

再构建、烧录和监视：

```powershell
idf.py -B build_main_verified build
idf.py -B build_main_verified -p COM17 flash monitor
```

服务器状态和预览地址为 `http://127.0.0.1:8000/`。首次运行启动脚本会自动安装 PyAV/OpenCV/websockets。`GET /preview.mjpg` 是浏览器持续预览流；视频帧走 **WebSocket `ws://<PC>:8001/ws`**，`GET /h264`（HTTP POST）作为回退通道保留。

下行命令在 HTTP 侧下发，广播给所有 WebSocket 连接：

```text
GET http://127.0.0.1:8000/cmd?cmd=ping     # 探活，P4 回 {"ok":true,"cmd":"ping"}
GET http://127.0.0.1:8000/cmd?cmd=status   # 取 P4 内部计数
```

P4 的回复是异步到达的，通过 `GET /status` 的 `last_command_reply` 字段读取。

电脑 WLAN IPv4 变化后，需要修改 `components/video_streamer/video_streamer.c` 中的 `VIDEO_STREAM_WS_URL`。

## 热复位恢复结论

已知现象：摄像头随开发板一起重新上电时视频链路可以工作；只复位 P4、摄像头不断电时，目标分辨率的首个 MJPEG 流可能保持 0 帧。640×480 和当前 800×600 均实测出现过这一现象。

实机日志已经证明：第一次目标 MJPEG 流为 0 帧时，完整 `stop/close` 后保持相同格式重新 `open/start` 可以恢复。当前 800×600 实测恢复后收到约 22 FPS，完整 JPEG 约 20～21 FPS。因此不再需要切到高分辨率再缩放的方案。固件恢复等待为 10 秒：

1. 首次打开 800×600 MJPEG。
2. 连续 10 秒没有完整的 800×600 MJPEG 帧时，完整执行 `stop/close`。
3. 保持 800×600 MJPEG，重新执行一次 `open/start`。
4. 原地重试仍连续 10 秒无帧时，才轮转到下一个设备格式。

```text
CAMERA: 连续 10 秒没有可编码帧：关闭并原地重试 800x600 MJPEG（1/1）
CAMERA: 尝试摄像头格式 1/...：800x600 MJPEG @ 30.00 FPS
CAMERA: RX frames=... 完整800x600MJPEG +... fps=约22
```

若同格式重开仍失败，驱动才轮转到设备声明的下一个格式，避免摄像头异常时无限重试。

## 帧率优化方案

开发板实测芯片为 ESP32-P4 revision v1.3，工程的 `CONFIG_ESP_REV_MIN_FULL=100`。`esp_h264` 1.4.0 的通用格式表虽列出 `RGB565_LE`，但该修订分支在运行时只接受 `ESP_H264_RAW_FMT_O_UYY_E_VYY`；直接把编码器输入改成 RGB565 会在创建编码器时返回参数错误。

LRCPG720p 保存的 MJPEG 帧经头部检查为 YUV422 采样，ESP-IDF JPEG 驱动对应的直接输出顺序为 `U Y0 V Y1`。当前实现因此让 JPEG 硬件直接输出 YUV422，再对相邻两行的 U/V 求平均并重排为编码器需要的交错 YUV420。原有 RGB565 拆色、每像素 BT.601 乘法和限幅已全部删除；这也使视频链路不再受 RGB565 字节序影响。

摄像头侧和视频输入槽统一预留 512 KB MJPEG 缓冲，并使用 3 个帧缓冲、8 个 16 KB URB（MPS=3072 向上对齐后每块占 18432 B）。当前自动选择 ISOC alt=1（`effective_MPS=3072`，设备 `BULK=0`）。帧进入解码队列前检查 JPEG `FF D8`（SOI）和 `FF D9`（EOI），从尾部找到 EOI 后只提交有效 JPEG 数据。串口累计显示目标帧、完整帧、缺 SOI、缺 EOI和过大帧；损坏帧直接丢弃，不进入硬件解码器。`video_streamer` 组件单独使用 `-O3` 编译以降低 YUV 重排耗时。

1280×720 曾实测约 27 FPS 输入和约 14 FPS 编码，但持续出现 UVC 帧缓冲溢出及 JPEG 硬件解码错误，因此不作为当前实时传输分辨率。800×600 的最新实测中没有再出现 UVC 溢出或 JPEG 解码错误。

## 2026-09-11 最新 CPU 分块转换状态

本节记录的是隔离档位 `CAMERA_TEST_UVC_H264_ONLY=6` 的测量，暂停 UI/LVGL、天气 HTTPS、WiFi/ESP-Hosted 和 WebSocket，只保留 UVC→JPEG 硬解→CPU YUV 转换→H.264 硬编。**该档位下 URB 可以落内部 RAM 并做到 0% 丢帧，但这条路径不能推广到完整档位**——144 KiB 挤不进 146 KiB 的内部 DMA 池，会分配失败并触发组件 double-free 重启循环，完整档位必须让 URB 落 PSRAM（`CONFIG_USB_HOST_DWC_DMA_CAP_MEMORY_IN_PSRAM=y`）。摄像头的 MJPEG 描述符只有 30/25/15 FPS，没有 800×600 MJPEG 20 FPS 离散档位，因此日志中的 UVC requested 仍为 30 FPS，20 FPS 只作为编码处理上限。

CPU 转换已由整帧连续重排改为 8 个 2 行宏块一段：每段完成后执行 `taskYIELD()`，并插入 20 µs 短空隙，降低单次连续 PSRAM 访问窗口。输出仍为 H.264 所需的 `O_UYY_E_VYY`，没有改变抽样和字节布局。

实测结果：

```text
UVC complete       19.0～20.7 FPS
UVC drop           31.6%～36.7%
JPEG_DEC           6.6～6.8 ms
YUV_CONV           15.0～15.4 ms
H264_ENC           约 8.0 ms
H.264 encoded      14.9～15.5 FPS
callback_gap_max   10 ms
SOI/EOI/非法头     本轮为 0
```

分块后转换耗时仍约 15 ms，短测 UVC 丢帧没有明显下降；因此这项改动目前只能确认降低了连续访问窗口，不能认为已经修复 UVC 丢包。后续应使用相同运行时长对比 tile 行数和块间空隙，并继续检查 PSRAM 仲裁与 USB ISOC 调度。

P4 v1.3 + ESP-IDF 5.5.5 上 DMA2D/PPA 的 YUV422→YUV420 硬件路径不可用，初始化返回 `ESP_ERR_NOT_SUPPORTED`，代码自动回退 CPU。不要删除版本检查并强行调用私有 DMA2D API，否则可能再次出现 DMA2D 超时或 `transaction not in-flight`。

## 正常运行指标

冷启动或恢复成功后，串口每 10 秒应出现：

```text
CAMERA: UVC requested=30，complete=20～30 fps，drop=2.6%～32%，ISOC alt=1，URB=8×16 KB
VIDEO_STREAM: H.264：编码=15～20 fps；耗时 J=约6.7/Y=约15.1/H=约8.3 ms；sent==encoded，send_fail=0
```

PC 页面显示“最近 10 秒”的帧率和码率，不受历史 0 帧时段影响。服务器控制台也每 10 秒显示最近速率；若超过 2 秒未收到帧，页面最近速率会变为 0。固件中的“过载丢帧”只统计队列忙或队列写入失败；为把摄像头输入降至 15 FPS 而主动跳过的帧不会被误报为故障。

历史 800×600 联网测试中，H.264 编码和发送约 15 FPS、发送失败为 0；2026-09-11 的无网络对照测试确认，即使关闭 WebSocket，UVC 仍有约 30% 级丢帧，因此当前主要问题不能归因于网络发送。

原始码流保存在：

```text
E:\Lummiss_Plant_Robot\src\demo\tools\camera_captures\camera_*.h264
```

PC 接收线程把每个H.264访问单元放入有界队列，持久PyAV解码器保留参考帧并只解码新帧。浏览器通过一个长期MJPEG连接接收内存中的最新JPEG，不再每500毫秒复制并重新解码整个GOP。磁盘 `latest_h264.jpg` 仅作为每秒一次的调试快照；即使被Windows看图程序锁定，网页仍能更新。若预览队列积压，服务器会等待下一个IDR重新同步，避免丢失P帧后持续花屏。

## 画面验收

烧录后请确认亮度与颜色正常。当前链路不再经过 RGB565；若仍有整体偏色，应重点核对 JPEG YUV422 输出顺序和 PC 解码结果。若图像出现成对像素或隔行错位，保存对应串口耗时日志和 PC 预览，优先检查 YUV422 到 O_UYY_E_VYY 的重排。

## 本轮通过条件

- 热复位后 10 秒内触发同格式重开，随后恢复约 22 FPS 摄像头输入。
- 恢复后完整 JPEG 约 20～21 FPS，H.264 编码和发送约 15 FPS。
- PC 最近速率与固件最近速率相符，`发送失败=0`。
- PC 页面“网页预览”接近接收帧率，通常约 15 FPS，预览丢帧不持续增加。
- 浏览器或播放器画面颜色正确、连续，无明显破帧。
- 连续运行 10 分钟无崩溃、USB 溢出、JPEG 解码错误或 H.264 编码错误。

ESP-Hosted SDIO 路径曾偶发一次 `tlsf_free` assert，当前只记录观察；若再次出现，应单独保存从启动到崩溃的完整日志并统计复现频率。当前 Host 组件日志版本为 2.7.x，板载 C6 固件为 2.3.x，启动时会提示版本不一致；现有联网和视频测试可运行，后续应升级 C6 从机固件并重新验证 RPC 稳定性。
