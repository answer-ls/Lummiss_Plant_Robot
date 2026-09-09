# Lummiss ESP32-P4 基础工程

本工程基于 ESP-IDF 5.5.5 和 FreeRTOS，是后续盆栽陪伴机器人功能的开发基础。厂商示例保留在 `../esp_draw_bit`，不在其中继续编写产品功能。

## 当前功能

- UI Task 初始化 GMT020-02-8P/ST7789 和 LVGL 8.4。
- 屏幕横屏分辨率为 320×240，循环播放现有 8 组动画表情。
- Camera Task 初始化 ESP32-P4 高速 USB Host 和 UVC 驱动。
- LRCPG720p 接入后优先使用 800×600 MJPEG 30 FPS，并每 10 秒输出 JPEG 完整性和收帧统计。
- 屏幕动画缓冲和摄像头帧缓冲使用 PSRAM。
- Network Manager 通过 ESP-Hosted/SDIO 控制板载 ESP32-C6，以 STA 模式连接固定 WiFi，并通过 DHCP 获取 IPv4 地址。
- WiFi 断开后每 2 秒自动发起重连；连接状态和 IP 信息只由 Network Manager 对外提供。
- Video Streamer 从完整 MJPEG 帧中抽取 15 FPS，经 P4 硬件 JPEG 直出 YUV422、轻量色度抽样/重排和 H.264 硬件编码，再由独立上传任务通过 HTTP/1.1 长连接实时发送到局域网 PC。当前编码参数为 800×600、1.5 Mbps、GOP 15。
- PC 服务器使用持久 PyAV H.264 解码器逐帧解码，通过 `/preview.mjpg` 向浏览器连续推送 MJPEG；浏览器预览不再反复打开和重解整个 GOP。

## 主组件结构

```text
components/network/
├── network_manager.c/.h     网络总入口、NVS、netif、连接状态和 DHCP 结果
├── wifi_manager.c/.h        esp_wifi_remote 事件、连接和自动重连
├── CMakeLists.txt           Network Manager 组件依赖
└── idf_component.yml        ESP-Hosted/esp_wifi_remote 版本要求

components/video_streamer/
├── video_streamer.c/.h      JPEG 解码、YUV 重排、H.264 硬件编码和 HTTP 实时发送
├── CMakeLists.txt           编码器、HTTP 与 Network Manager 依赖
└── idf_component.yml        esp_h264 1.4.0 版本要求

main/
├── main.c                    唯一 app_main()，创建 FreeRTOS 任务
├── display_driver.c/.h       ST7789、LVGL 和表情播放器
├── camera_driver.c/.h        USB Host 和 UVC 摄像头管理
├── gif_assets.c/.h           8 组表情动画资源
└── idf_component.yml         管理组件版本
```

`main.c` 当前创建：

| 任务 | 优先级 | 栈大小 | 职责 |
| --- | ---: | ---: | --- |
| `ui_task` | 6 | 8192 字节 | 初始化屏幕和 LVGL；后续接收 UI 事件 |
| `camera_task` | 7 | 8192 字节 | 运行 USB Host/UVC 摄像头驱动 |

UVC 组件还会创建 USB 事件任务和驱动后台任务。所有显式 LVGL 初始化都由 UI Task 发起，摄像头任务不直接操作 LVGL。

`app_main()` 对网络只调用 `network_manager_init()` 和 `network_manager_start()`；WiFi 系统事件、DHCP 状态及重连逻辑均封装在 `components/network` 中。当前固定 SSID 和密码位于 `network_manager.c`，以后 BLE 配网阶段再改为读取 NVS。

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

首次运行会自动安装 `tools/camera_server_requirements.txt` 中的 PyAV 和 OpenCV。服务器启动后访问 `http://127.0.0.1:8000/`；页面显示H.264接收帧率、浏览器预览帧率和预览队列丢帧数。

在已经激活 ESP-IDF 5.5.5 的终端中：

```powershell
idf.py -B build_main_verified build
idf.py -B build_main_verified -p COM17 flash monitor
```

VS Code 工作区也已默认使用 `build_main_verified` 和 Ninja。

2026-09-09 切换到 800×600 并加入 JPEG 完整性检查后的联合构建结果：

```text
lummiss_main.bin：0x196cc0
8 MB 应用分区剩余：80%
bootloader.bin：0x5310
构建结果：通过
```

## 烧录后的正常现象

1. 串口出现 `APP_MAIN`，随后可看到 ESP-Hosted 初始化和 SDIO 与 C6 建链日志。
2. 出现 `WiFi STA 已启动，开始连接路由器` 和 `正在通过 ESP32-C6 连接 WiFi`。
3. 成功连接后出现 `WiFi 联网成功`，并输出非 `0.0.0.0` 的 IPv4、网关和掩码。这才表示 P4→C6→路由器→DHCP 链路完整成功。
4. 屏幕黑底横屏显示，中央区域依次播放眨眼、喜、怒、哀、乐、思考、惊讶、疑惑。
5. 摄像头未插入时，UI 动画和 WiFi 仍应正常运行。
6. 摄像头插入高速 USB 口后，串口显示使用 `800x600 MJPEG 30 FPS` 和 `Stream started`。热复位后首流可能为 0 帧，10 秒后原地重开可恢复；实测恢复后输入约 22 FPS、完整 JPEG 约 20～21 FPS。
7. PC 服务器运行时，`VIDEO_STREAM` 每 10 秒汇总一次，编码和发送应接近 15 FPS、码率约 1.5 Mbps、发送失败为 0，并显示 JPEG、YUV 重排、H.264 和 HTTP 四段平均耗时；浏览器页面中的“接收”和“网页预览”应接近 15 FPS，PC 的 `tools/camera_captures` 中生成持续增大的 `.h264` 文件。

密码错误或路由器不可达时，串口会反复出现 `WiFi 已断开`、原因码和 2 秒后重连。若在这些日志之前就出现 Hosted/SDIO 初始化失败，应先检查板载 C6 固件；厂商提供的参考固件位于 `开发板示例/JC1060P470C_I_W_Y/8-Burn operation/Burn files/JC-C6-slave_v2.3.2.bin`。

PC 服务器的启动、热复位恢复实验、JPEG 完整性统计、颜色验收和实时速率判断见 `CAMERA_UPLOAD_TEST.md`。当前采用 HTTP/1.1 长连接传输 H.264 实时帧；H.265 不受当前 ESP32-P4 编码器支持，WebSocket 可在云端协议确定后替换传输层。ESP-Hosted Host 2.7.x 与板载 C6 2.3.x 存在版本警告，当前联网和视频传输正常，后续应升级 C6 固件并做回归测试。
