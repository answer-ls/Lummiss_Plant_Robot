# Lummiss 盆栽陪伴机器人项目交接说明

更新时间：2026-09-10

## 1. 当前结论

项目目前处于“基础工程 + 硬件可行性验证”阶段，还没有形成产品最小闭环。

- 阶段 1 已建立正式基础入口：`main.c` 使用 FreeRTOS 创建 UI Task 和 Camera Task，Network Manager 已负责 NVS 基础初始化。
- 阶段 2 已完成 ST7789、LVGL 8.4 和动态时间首页代码及构建。首页使用网络校时、IP 自动定位与 Open-Meteo 天气 API，显示日期、星期、天气、温度和大号时间；电量留到电池管理阶段。镜像参数已修正，动态数据和屏幕效果仍待实机验证。
- 阶段 6 的 WiFi 基础链路已经实机通过：ESP32-P4 经 ESP-Hosted/SDIO 控制板载 ESP32-C6，连接 `wcmgc` 后由 DHCP 获得 `192.168.1.32`；断线重连代码已完成。
- 阶段 8 当前链路为 `800×600 MJPEG(YUV422) → P4 硬件 JPEG 直出 YUV422 → 轻量抽样/重排 O_UYY_E_VYY → H.264 硬件编码 → HTTP`。热复位首流 0 帧后，同格式 `stop/close/open/start` 已实机恢复约 22 FPS 输入；完整 JPEG 约 20～21 FPS，H.264 编码与发送稳定在 14.8～15.0 FPS、约 1.5 Mbps、发送失败 0。RGB565 全帧 BT.601 软件换算已删除，编解码与网络上传已拆成两个 FreeRTOS 任务。
- UI 状态机、传感器、触摸、LED、音频、小智、电机、专注模式、电源管理和整机联调均未进入实现阶段。

开发流程基线为根目录的 `盆栽陪伴机器人_开发文档.md`。本文只记录当前事实和下一步，不替代该设计文档。

## 2. 目录和环境

正式工程路径：

```text
E:\Lummiss_Plant_Robot\src\demo
```

主要目录：

```text
src/demo             当前 ESP32-P4 硬件测试工程
src/esp_draw_bit     厂商示例副本，只作参考
开发板示例           厂商完整资料
项目文档             产品需求资料
```

开发环境：

| 项目 | 当前值 |
| --- | --- |
| ESP-IDF | 5.5.5 |
| 芯片 | ESP32-P4，开发板实测 revision v1.3 |
| Flash | 16 MB |
| 图形库 | LVGL 8.4.x |
| LVGL 端口 | `espressif/esp_lvgl_port` 2.9.0 |
| 摄像头组件 | `espressif/usb_host_uvc` 2.2.0 |
| 视频编码组件 | `espressif/esp_h264` 1.4.0，ESP32-P4 v1.x 硬件库 |
| WiFi Host 组件 | `espressif/esp_hosted` 2.7.4 |
| 远程 WiFi API | `espressif/esp_wifi_remote` 1.3.0 |
| 默认串口 | COM17 |

工程根目录必须保持英文路径。旧中文路径曾导致 Python/Kconfig 的 GBK 解码错误和 Ninja 乱码路径错误。

## 3. 当前基础工程结构

`src/demo` 已从两个独立测试入口改为一个联合基础工程：

| 文件 | 职责 |
| --- | --- |
| `main/main.c` | 唯一 `app_main()`，创建 UI Task 和 Camera Task |
| `main/display_driver.c/.h` | ST7789、LVGL 和动态时间天气首页 |
| `main/camera_driver.c/.h` | USB Host、UVC 枚举和视频流统计 |
| `main/gif_assets.c/.h` | 保留的表情动画资源，当前首页不编译 |
| `components/home_info/home_info.c/.h` | IP 定位、SNTP 校时、Open-Meteo 天气和首页数据快照 |
| `components/network/network_manager.c/.h` | 网络总入口、状态、NVS/netif 和 DHCP 结果 |
| `components/network/wifi_manager.c/.h` | WiFi 事件、固定凭据连接和自动重连 |
| `components/video_streamer/video_streamer.c/.h` | MJPEG 队列、硬件 JPEG 解码、YUV422 抽样重排、H.264 编码和 HTTP 传输 |
| `tools/pc_camera_server.py` | PC 端 H.264 接收、保存、持久PyAV解码和MJPEG网页预览 |

当前统一构建目录为 `build_main_verified`。旧的 `build_screen_verified` 和 `build_camera_verified` 只是历史验证产物，不再对应当前入口。

## 4. 按开发文档阶段审计

状态含义：完成表示代码、构建和已有验收证据均满足当前阶段；部分完成表示只完成其中一部分或缺少真机确认。

| 阶段 | 状态 | 已完成 | 仍缺少 |
| --- | --- | --- | --- |
| 阶段 0：需求冻结 | 部分完成 | 已有总体系统框图；屏幕、USB 摄像头接口已明确 | 完整 BOM；音频、触摸、传感器、LED、C6、电机、电源 GPIO 表；统一接口定义 |
| 阶段 1：基础工程 | 基本完成 | ESP-IDF 工程、串口日志、UI/Camera FreeRTOS 任务、NVS 初始化、PSRAM、16 MB Flash、分区表、ESP32-P4 v1.x 镜像 | NVS 的产品数据结构及读写验证 |
| 阶段 2：屏幕 | 部分完成 | ST7789 驱动、LVGL 8.4、320×240 动态时间首页、IP 定位、网络校时、天气 API、镜像修正、8 组表情资源保留、源码构建通过 | 真机颜色/方向/API/稳定性验收；LISTEN/THINK/REPLY/SLEEP/FAULT 页面；页面切换接口；电池管理完成后接入电量 |
| 阶段 3：UI 状态机 | 未开始 | 当前只有静态时间首页 | Event Bus、状态请求、优先级、覆盖、恢复、超时和异常 |
| 阶段 4：传感器/触摸/LED | 未开始 | 无 | 土壤、光照、温湿度、左右触摸、呼吸灯 |
| 阶段 5：音频 | 未开始 | 无 | 麦克风、扬声器、I2S、PCM、Opus |
| 阶段 6：Wi-Fi | 部分完成 | P4-C6 ESP-Hosted/SDIO、独立 Network Manager、固定凭据、DHCP 实机成功、2 秒自动重连 | NVS 凭据、BLE 配网、HTTP 通用封装 |
| 阶段 7：小智 | 未开始 | 无 | 基础语音链路、UI 状态映射、MCP |
| 阶段 8：摄像头 | 部分完成 | USB Host/UVC 枚举；800×600 MJPEG；JPEG 完整性门控；硬件 JPEG 解码；双任务 H.264 编码与 WebSocket 实机打通；16 字节帧头自描述分辨率；下行命令 ping/status 双向通道；断线自动重连（不复位）；15 FPS 发送；PC 保存/持久解码/MJPEG 预览；热复位同格式重开恢复已通过 | 浏览器实时画面验收、10 分钟及更长稳定性、C6 固件版本对齐、正式 Camera API、运行期改分辨率 |
| 阶段 9：旋转底座 | 未开始 | 无 | 电机、编码器、回零、角度、启停和堵转保护 |
| 阶段 10：专注模式 | 未开始 | 无 | 全部功能 |
| 阶段 11：电源管理 | 未开始 | 无 | 电量、充电、休眠和各外设降功耗 |
| 阶段 12：整机联调 | 未开始 | 无 | 全部联调组合 |
| 阶段 13：稳定性 | 未开始 | 只有摄像头短期稳定收帧证据 | 72 小时、断网、热插拔、泄漏、温升和功耗测试 |

## 5. 第一阶段执行清单的实际状态

```text
[x] 建立 ESP-IDF 工程
[~] 确认 GPIO 分配：只确认屏幕和高速 USB，整机 GPIO 未冻结
[~] ST7789 点亮：驱动与固件已完成，缺少用户对实机显示的确认
[~] LVGL 8.4.0 跑通：组件、端口及图片对象已编译进固件，缺少屏幕实机确认
[~] 动画帧播放：8 组素材已保留，当前常驻首页不编译动画资源
[~] HOME 页面：动态时间、日期和天气已接入并构建通过，等待实机/API 验收；电量等待电池管理
[ ] LISTEN 页面
[ ] THINK 页面
[ ] REPLY 页面
[ ] UI 状态机
[ ] 麦克风采集
[ ] 扬声器播放
[x] Wi-Fi 联网：P4→C6→路由器→DHCP 已获得 192.168.1.32
[ ] 小智基础语音链路
```

MVP1 六项中，ST7789、LVGL、超过 5 个表情资源和时间首页静态设计稿已经具备代码；动态首页、植物传感器、呼吸灯未完成。由于屏幕还缺少实机显示反馈，MVP1 不能判定完成。

## 6. 屏幕当前实现

屏幕与接线：

| GMT020-02-8P | ESP32-P4 |
| --- | --- |
| GND | GND |
| VCC | 3V3 |
| SCL/SCLK | GPIO20 |
| SDA/MOSI | GPIO32 |
| RST/RES | GPIO3 |
| DC | GPIO2 |
| CS | GPIO1 |
| BL/BLK | 3V3 |

当前显示参数：

- ST7789 面板原生 240×320，LVGL 逻辑分辨率为横屏 320×240。
- SPI2，40 MHz，RGB565，颜色反相开启，显存偏移 `(0, 0)`。
- BL 直接接 3V3，只能常亮，程序无法调光或熄灭背光。
- 当前使用 LVGL 控件绘制 320×240 首页，联网前显示占位符，联网成功后更新真实数据。
- 时间由网络校时维护；IP 定位服务提供城市坐标和时区，Open-Meteo 返回当前温度与 WMO 天气码。
- 实机反馈原显示左右镜像，横屏旋转配置已改为 `swap_xy=true, mirror_x=true, mirror_y=false`。
- 电量区域已按当前阶段要求隐藏，待阶段 11 电池管理具备可靠数据后再加入。
- 天气每 30 分钟更新一次；定位每 6 小时更新一次；断线时保留最近一次成功结果。

原动画源文件及 `gif_assets.c/h` 继续保留，供后续 LISTEN/THINK/REPLY 等状态页面复用；当前 `main/CMakeLists.txt` 不编译这些动画资源。

2026-09-10 接入动态时间、天气 API 和镜像修正后的构建结果：

```text
lummiss_main.bin：0x19afb0，8 MB 应用分区剩余 80%
bootloader.bin：0x5310，Bootloader 分区剩余 13%
构建结果：通过
```

烧录后正常现象应为：屏幕先显示时间和天气占位符；WiFi 联网后串口依次输出“网络时间同步成功”“自动定位成功”和“天气更新”，页面自动替换为当前日期、星期、时间、天气和温度，不显示电量。文字应正常朝向且不再左右镜像。

## 7. 摄像头当前验证结论

硬件为 LRCPG720p USB 摄像头，连接 ESP32-P4 高速 USB Host 口。已有完整日志证明：

- UVC 枚举成功并读取到真实格式描述符。
- 支持多组 MJPEG/YUY2 分辨率；当前选择 800×600 MJPEG，设备声明 30 FPS，实测输入约 22 FPS。
- 帧数和累计字节持续增加，800×600 约 39～42 KB/帧，空帧为 0。
- 原先 `@0.0FPS` 打不开流的问题已通过读取设备实际帧率修复。

摄像头驱动由独立 Camera Task 运行。当前 `components/video_streamer` 接收 800×600 MJPEG，按 15 FPS 放入两个 PSRAM 输入槽；实测 MJPEG 是 YUV422 采样，ESP32-P4 硬件 JPEG 解码器直接输出 `U Y0 V Y1`。软件只对相邻两行 U/V 求平均并重排为 P4 v1.x H.264 编码器要求的 `O_UYY_E_VYY`，再编码为目标 1.5 Mbps、GOP 15 的 Annex-B H.264。编解码任务把码流放入 4 个输出槽，独立上传任务通过 WebSocket 二进制帧发送（每帧前置 16 字节自描述头携带宽高/帧率/帧类型/序号），网络等待不会阻塞编解码。注意输出槽内编码缓冲必须保持 128 字节 cache line 对齐（槽按 128 对齐分配，编码数据放 +128、帧头放 +112），否则 `esp_cache_msync` 失效会让 CPU 读到旧帧数据。原 RGB565 拆色与 BT.601 全帧换算和 YUY2 主线均已删除。

冷启动重新插电时视频链路已实机打通。只复位 P4、摄像头不断电时，首个目标 MJPEG 流可能持续 0 帧；640×480 和 800×600 均已证明完整 `stop/close` 后保持原格式重新 `open/start` 可以恢复。当前 800×600 在约 10 秒触发原地重试后恢复约 22 FPS；第二次仍失败才轮转格式。

当前 800×600 实测输入 21.99～22.88 FPS，完整 JPEG 约 20～21 FPS，H.264 编码与发送 14.8～15.0 FPS、1459～1517 kbps、发送失败 0；平均耗时约为 JPEG 6.7 ms、YUV 重排 15.4～15.5 ms、H.264 8.1 ms、HTTP 37.5～40.1 ms，HTTP 峰值约 229 ms，过载累计 3 帧。673 个目标 JPEG 中 42 帧缺 SOI（约 6.2%），缺 EOI 和过大帧为 0；损坏帧已在解码前丢弃，因此本轮没有 UVC 溢出或 JPEG 解码错误。1280×720 曾持续发生 UVC 溢出和 JPEG 解码错误，已放弃作为当前实时分辨率。PC 端采用有界 H.264 队列、持久 PyAV 解码器和 `/preview.mjpg` 长连接，等待用户对 800×600 实时画面的最终验收。

注意：开发板是 ESP32-P4 revision v1.3，`CONFIG_ESP_REV_MIN_FULL=100`。`esp_h264` 1.4.0 的通用格式表虽然列有 `RGB565_LE`，该修订的实际参数检查只接受 `O_UYY_E_VYY`，所以不能直接把 RGB565 传给编码器。当前 YUV422 快速路径是在这一硬件约束下删除主要软件换算开销的安全实现。

## 8. WiFi 当前实现

当前链路目标为：

```text
ESP32-P4 → ESP-Hosted/SDIO → 板载 ESP32-C6 → WiFi 路由器 → DHCP
```

已建立独立 `components/network` 组件。`main.c` 对网络只调用 `network_manager_init()` 和 `network_manager_start()`，没有放入 WiFi event handler。Network Manager 负责 NVS、TCP/IP 协议栈、默认 STA netif、连接状态和 IP 结果；WiFi Manager 负责 `esp_wifi_remote` 初始化、WiFi/IP 事件和每 2 秒自动重连。

本阶段使用固定 SSID `wcmgc`，密码保存在 `network_manager.c`，并通过 `WIFI_STORAGE_RAM` 避免远端 C6 保存旧凭据。后续 BLE 配网阶段应将凭据移入 NVS，并保留现有 Manager API 作为业务模块唯一网络入口。

ESP-Hosted 参数来自同型号开发板厂商示例，最终生成的 `sdkconfig` 已核对：

| 项目 | 配置 |
| --- | --- |
| C6 从机目标 | ESP32-C6 |
| SDIO | Slot 1、4-bit、40 MHz |
| CLK / CMD | GPIO18 / GPIO19 |
| D0 / D1 / D2 / D3 | GPIO14 / GPIO15 / GPIO16 / GPIO17 |
| C6 RESET | GPIO54，高电平有效，每次 P4 启动时复位 C6 |
| Host 组件 | `esp_hosted 2.7.4` |
| Remote API | `esp_wifi_remote 1.3.0` |

真机日志已证明 SDIO 成功识别 ESP32-C6，WiFi 连接成功并获得 IPv4 `192.168.1.32`、网关 `192.168.1.1`。日志同时提示 Host 2.7.0 高于 C6 协处理器 2.3.0；当前联网和 UVC 并行运行正常，但后续出现 RPC timeout 时需要升级 C6 从机固件。

若在 STA 连接日志之前发生 Hosted/SDIO 握手失败，应确认板载 C6 从机固件。厂商提供的参考文件为：

```text
E:\Lummiss_Plant_Robot\开发板示例\JC1060P470C_I_W_Y\8-Burn operation\Burn files\JC-C6-slave_v2.3.2.bin
```

## 9. 构建方法

```powershell
Set-Location E:\Lummiss_Plant_Robot\src\demo
cmd /c _build_main.bat
idf.py -B build_main_verified -p COM17 flash monitor
```

`_build_main.bat` 可在普通终端激活本机 ESP-IDF 5.5.5 并完成联合构建。由于本机安装器把 Python 约束文件放在特殊位置，脚本设置了 `IDF_PYTHON_CHECK_CONSTRAINTS=no`；实际 Python 依赖检查仍在激活阶段显示为 OK。

VS Code 当前默认使用 `build_main_verified`，生成器必须为 Ninja。若出现 `NMake Makefiles`，需要确认工作区打开的是 `src/demo` 并重新加载 VS Code 设置。

## 10. 现在最需要解决的问题

1. **800×600 网页预览待最终验收。** 设备端已稳定发送约 15 FPS；需重启服务器并确认页面接收和预览均接近 15 FPS、画面连续、预览丢帧不持续增加。
2. **短期设备链路已通过，长期稳定性未测。** 热复位同格式重开、JPEG 完整性门控、双任务编解码/上传均已实机工作；需连续运行至少 10 分钟，再逐步扩展到长时间测试。
3. **动态首页待真机结论。** 构建通过不能证明镜像修正、中文字体、IP 自动定位和两个 HTTPS API 在设备网络上均正常，需要烧录后的照片及 `HOME_INFO` 日志。
4. **阶段 0 尚未冻结。** 除屏幕、USB 和 P4-C6 SDIO 外，关键器件型号与 GPIO 未定，会阻塞阶段 4、5、9、11。
5. **当前只完成第一层模块化。** 屏幕和摄像头已提取为驱动文件，但仍位于主组件，尚未拆成独立 ESP-IDF 组件、服务层和业务层。
6. **没有页面和状态机。** 当前静态 HOME 不能代表 HOME→LISTEN→THINK→REPLY→恢复流程，也无法做优先级和异常覆盖。
7. **屏幕背光不可控。** BL 接 3V3 无法满足休眠、低电量和亮度调节，需要正式硬件增加背光驱动和 PWM GPIO。

## 11. 按流程继续的顺序

1. 关闭旧 PC 服务器后重新运行 `src/demo/_start_camera_server.bat`，打开 `http://127.0.0.1:8000/`；确认 800×600 的接收和网页预览均接近 15 FPS。
2. 连续运行 10 分钟，观察缺 SOI 比例、页面预览丢帧、固件过载丢帧、USB/JPEG/H.264 错误、WiFi 发送失败和 ESP-Hosted 崩溃。
3. 当前双任务流水线保留 20 FPS 性能余量；在 15 FPS 稳定性验收后，可测试 800×600@20 FPS。传输层已于 2026-09-10 由 HTTP 长连接替换为 WebSocket，并建立了下行命令通道（`/cmd?cmd=ping|status`）。
4. 补齐阶段 0 的 BOM、完整 GPIO 表和音频、传感器、电机接口定义；P4-C6 SDIO 引脚已经按厂商示例确定。
5. 验收 HOME 页动态时间、日期、天气与镜像方向，再建立 LISTEN、THINK、REPLY、SLEEP、FAULT 页面骨架；电量在阶段 11 完成电池管理后接入。
6. 继续把 display、ui、expression 拆分为独立组件，提供 `expression_play()` 等统一接口。
7. 完成阶段 3：实现 App Event Bus、UI 状态机、优先级、超时和状态恢复；只有 UI Task 调用 LVGL。
8. ~~H.264 HTTP 实时链路通过后替换为 WebSocket~~ **已完成（2026-09-10）**：P4 侧走 `esp_websocket_client`（`ws://<PC>:8001/ws`），每帧前置 16 字节自描述头（分辨率/帧率随帧携带）；PC 侧 HTTP(8000) 保留预览页、`/h264` 回退入口与 `/cmd` 下行命令。编码队列和 Network Manager 接口未动。后续要做运行期改分辨率：需把 `VIDEO_STREAM_WIDTH/HEIGHT` 从编译期宏改为运行期变量，并同步 `camera_driver.c` 的格式耦合（兜底格式表、格式提升、帧门控、`preferred_mjpeg` 判定），帧头自描述已解除 PC 端对分辨率的感知需求。

下一个会话开始时，先读取本文件、`盆栽陪伴机器人_开发文档.md` 和 `src/demo/CAMERA_UPLOAD_TEST.md`，再根据 H.264 串口统计、PC `/status`、浏览器预览及保存的 `.h264` 继续。修改 C/C++ 源码时继续使用中文注释。
