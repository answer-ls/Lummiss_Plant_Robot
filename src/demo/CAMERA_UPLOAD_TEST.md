# ESP32-P4 USB 摄像头 H.264 实时回传测试

## 当前实现链路

```text
LRCPG720p USB 摄像头
→ UVC 640×480 MJPEG 30 FPS
→ ESP32-P4 硬件 JPEG 直接解码为 UYVY/YUV422
→ 软件仅做垂直色度抽样和 O_UYY_E_VYY 字节重排
→ ESP32-P4 H.264 硬件编码，目标 10 FPS / 800 kbps / GOP 10
→ HTTP/1.1 长连接逐帧 POST
→ ESP-Hosted / ESP32-C6 / WiFi
→ Windows PC 192.168.1.66:8000
→ 保存 Annex-B `.h264` 码流并生成浏览器预览
```

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

服务器状态和预览地址为 `http://127.0.0.1:8000/`。首次运行启动脚本会自动安装 PyAV/OpenCV。`GET /h264` 返回状态，`GET /preview.mjpg` 是浏览器持续预览流。电脑 WLAN IPv4 变化后，需要修改 `components/video_streamer/video_streamer.c` 中的 `VIDEO_STREAM_URL`。

## 热复位恢复结论

已知现象：摄像头随开发板一起重新上电时，640×480 MJPEG 视频链路可以工作；只复位 P4、摄像头不断电时，首个 640×480 流可能保持 0 帧，而切到 1280×960 后立即有帧。

实机日志已经证明：第一次 640×480 MJPEG 为 0 帧时，完整 `stop/close` 后保持相同格式重新 `open/start`，立即恢复约 30 FPS。因此不再需要 1280×960 解码缩放方案。固件已将恢复等待从 30 秒缩短为 10 秒：

1. 首次打开 640×480 MJPEG。
2. 连续 10 秒没有“可编码的 640×480 MJPEG 帧”时，完整执行 `stop/close`。
3. 保持 640×480 MJPEG，重新执行一次 `open/start`。
4. 原地重试仍连续 10 秒无帧时，才轮转到下一个设备格式。

```text
CAMERA: 连续 10 秒没有可编码帧：关闭并原地重试 640x480 MJPEG（1/1）
CAMERA: 尝试摄像头格式 1/...：640x480 MJPEG @ 30.00 FPS
CAMERA: RX frames=... 可编码640x480MJPEG +... fps=约30
```

若同格式重开仍失败，驱动才轮转到设备声明的下一个格式，避免摄像头异常时无限重试。

## 帧率优化方案

开发板实测芯片为 ESP32-P4 revision v1.3，工程的 `CONFIG_ESP_REV_MIN_FULL=100`。`esp_h264` 1.4.0 的通用格式表虽列出 `RGB565_LE`，但该修订分支在运行时只接受 `ESP_H264_RAW_FMT_O_UYY_E_VYY`；直接把编码器输入改成 RGB565 会在创建编码器时返回参数错误。

LRCPG720p 保存的 MJPEG 帧经头部检查为 YUV422 采样，ESP-IDF JPEG 驱动对应的直接输出顺序为 `U Y0 V Y1`。当前实现因此让 JPEG 硬件直接输出 YUV422，再对相邻两行的 U/V 求平均并重排为编码器需要的交错 YUV420。原有 RGB565 拆色、每像素 BT.601 乘法和限幅已全部删除；这也使视频链路不再受 RGB565 字节序影响。

## 正常运行指标

冷启动或恢复成功后，串口每 10 秒应出现：

```text
CAMERA: RX frames=... 可编码640x480MJPEG +约300，fps=约30，empty=0
VIDEO_STREAM: H.264：编码=约10 fps，发送=约10 fps，约800 kbps，耗时 J=.../Y=.../H=.../HTTP=... ms，过载丢帧=0，失败=0
```

PC 页面现在显示“最近 10 秒”的帧率和码率，不再只显示受历史 0 帧时段影响的会话平均值。服务器控制台也每 10 秒显示最近速率。若超过 2 秒未收到帧，页面最近速率会变为 0。固件中的“过载丢帧”只统计队列忙或队列写入失败；为把 30 FPS 降至 10 FPS 而主动跳过的帧不会被误报为故障。

原始码流保存在：

```text
E:\Lummiss_Plant_Robot\src\demo\tools\camera_captures\camera_*.h264
```

PC 接收线程把每个H.264访问单元放入有界队列，持久PyAV解码器保留参考帧并只解码新帧。浏览器通过一个长期MJPEG连接接收内存中的最新JPEG，不再每500毫秒复制并重新解码整个GOP。磁盘 `latest_h264.jpg` 仅作为每秒一次的调试快照；即使被Windows看图程序锁定，网页仍能更新。若预览队列积压，服务器会等待下一个IDR重新同步，避免丢失P帧后持续花屏。

## 画面验收

烧录后请确认亮度与颜色正常。当前链路不再经过 RGB565；若仍有整体偏色，应重点核对 JPEG YUV422 输出顺序和 PC 解码结果。若图像出现成对像素或隔行错位，保存对应串口耗时日志和 PC 预览，优先检查 YUV422 到 O_UYY_E_VYY 的重排。

## 本轮通过条件

- 热复位后 10 秒内触发同格式重开，随后恢复约 30 FPS 摄像头输入。
- 恢复后 UVC 可编码帧约 30 FPS，H.264 编码和发送约 10 FPS。
- PC 最近速率与固件最近速率相符，`发送失败=0`。
- PC 页面“网页预览”接近接收帧率，通常约 9～10 FPS，预览丢帧不持续增加。
- 浏览器或播放器画面颜色正确、连续，无明显破帧。
- 连续运行 10 分钟无崩溃、USB 溢出、JPEG 解码错误或 H.264 编码错误。

ESP-Hosted SDIO 路径曾偶发一次 `tlsf_free` assert，当前只记录观察；若再次出现，应单独保存从启动到崩溃的完整日志并统计复现频率。
