# Lummiss 盆栽陪伴机器人项目交接说明

更新时间：2026-09-14

## 1. 当前结论

项目目前处于“基础工程 + 硬件可行性验证”阶段，还没有形成产品最小闭环。

- 阶段 1 已建立正式基础入口：`main.c` 使用 FreeRTOS 创建 UI Task 和 Camera Task，Network Manager 已负责 NVS 基础初始化。
- 阶段 2 已完成 ST7789、LVGL 8.4 和动态时间首页代码及构建。首页使用网络校时、IP 自动定位与 Open-Meteo 天气 API，显示日期、星期、天气、温度和大号时间；电量留到电池管理阶段。镜像参数已修正，动态数据和屏幕效果仍待实机验证。
- 阶段 6 的 BLE + WiFi 配网链路已经实机通过：ESP32-P4 经 ESP-Hosted/SDIO 使用板载 ESP32-C6 的 BLE Controller 和 WiFi。首次启动以 `LUMMISS_XXXXXX` 广播，App 通过官方 Unified Provisioning Security 2 下发凭据；成功后停止广播并启动业务。已有凭据时直接联网，断线自动重连。
- 阶段 8 当前链路为 `800×600 MJPEG(YUV422) → P4 硬件 JPEG 直出 YUV422 → CPU 分块抽样/重排 O_UYY_E_VYY → H.264 硬件编码`；网络上传代码仍保留，但当前烧录的对照档位关闭 WebSocket。首帧超时会完整停止并确认 USB transfer 回调退出，再执行 USB Host/UVC 重建；本轮首帧重建后约 249 ms 成功。实测完整档位下 UVC 完整帧约 20～30 FPS、丢帧 19%～29%（2026-09-12 由同一次运行内的 A/B 定位为**编解码链路自身的 PSRAM 流量**，见第 7 节），H.264 实际约 15～20 FPS，**800×600@20 FPS 目标尚未达成**。RGB565 全帧 BT.601 软件换算已删除，编解码与网络上传已拆成两个 FreeRTOS 任务。
- 2026-09-11 已确认：当前 P4 v1.3 + ESP-IDF 5.5.5 不能可靠使用 PPA/DMA2D 做 YUV422→YUV420；工程保留能力探测并回退 CPU。CPU 转换改为 8 个 2 行宏块一段，块间调度让出并插入 20 µs 短空隙，当前转换耗时约 15.0～15.4 ms。该改动未使短测 UVC 丢帧明显下降，剩余瓶颈仍是 H.264/PSRAM 与 USB ISOC 的长期争用或调度。
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
| BLE/WiFi 配网组件 | `espressif/network_provisioning` 1.2.4 |
| 默认串口 | COM17 |

工程根目录必须保持英文路径。旧中文路径曾导致 Python/Kconfig 的 GBK 解码错误和 Ninja 乱码路径错误。

## 3. 当前基础工程结构

`src/demo` 已从两个独立测试入口改为一个联合基础工程：

| 文件 | 职责 |
| --- | --- |
| `main/main.c` | 唯一 `app_main()`，按档位启动 Network / 首页信息 / UI Task / Camera Task |
| `main/test_profile.h` | **启动组合测试档位的唯一定义点**：档位号 0~8、档位→功能位映射、`TP_HAS()` 和 `test_profile_name()` |
| `main/display_driver.c/.h` | ST7789、LVGL 和动态时间天气首页 |
| `main/screen_carousel.c/.h` | TF 卡 GIF 轮播：挂载 SD、预读任务、LVGL 定时器每 5 秒换 `lv_gif` 的 `src` |
| `main/camera_driver.c/.h` | USB Host、UVC 枚举、格式轮转、卡死恢复、帧回调和视频流统计 |
| `components/home_info/home_info.c/.h` | IP 定位、SNTP 校时、Open-Meteo 天气和首页数据快照 |
| `components/network/network_manager.c/.h` | 网络总入口、BLE 配网状态、NVS/netif 和 DHCP 结果 |
| `components/network/wifi_manager.c/.h` | WiFi 事件、C6 已保存凭据连接和自动重连 |
| `components/provisioning/provisioning_manager.c/.h` | BLE 配网、Security 2、设备名、每设备 PoP 和配网生命周期 |
| `BLE_WIFI_PROVISIONING.md` | App 对接参数、二维码格式、配网流程和重新配网接口 |
| `components/sd_card/sd_card.c/.h` | SDMMC Slot 0 挂载，含片上 LDO ch.4 供电和重试 |
| `components/video_streamer/video_streamer.c/.h` | MJPEG 队列、硬件 JPEG 解码、CPU 分块 YUV422→YUV420、H.264 编码和 WebSocket 传输 |
| `components/video_streamer/dma2d_yuv.c/.h` | DMA2D 硬件 YUV422→YUV420（**本板 v1.3 不可用**，仅 codec-only 档位以外永不生效） |
| `tools/pc_camera_server.py` | PC 端 H.264 接收、保存、持久 PyAV 解码和 MJPEG 网页预览 |
| `tools/capture_camera_serial.py` | 采集串口日志到文件，供实机对照测试复盘 |
| `tools/summarize_camera_serial.py` | 汇总串口日志：跳过启动前 15 秒，报窗口均值和累计数增量 |
| `tools/capture_to_mp4.py` | 把接收到的 H.264 转存为 mp4 |
| `tools/gif_resize_for_sd.py` | 把表情 GIF 缩放到屏幕尺寸并写入 TF 卡目录 |
| `tools/gif2c.py` | 把 GIF 转成 C 数组（**当前产物 `main/gif_assets.c` 已删除**，改走 TF 卡运行时解码） |

当前统一构建目录为 `build_main_verified`。旧的 `build_screen_verified`、`build_camera_verified` 和 `build_c6_274` 只是历史验证产物，已于 2026-09-12 删除，不再对应当前入口。

**2026-09-12 清理**：删除无调用者的 `components/mjpeg_streamer/`（2026-09-10 直通预览的遗留，全树零引用）和未编译进固件的 `main/gif_assets.c/.h`（22k 行生成资源，已被 TF 卡运行时解码取代）。测试档位宏从 `camera_driver.h` + `main.c` 两处 6 个手写布尔宏收敛到 `main/test_profile.h` 一处。

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
| 阶段 8：摄像头 | 部分完成 | USB Host/UVC 枚举；800×600 MJPEG；JPEG 完整性门控；硬件 JPEG 解码；CPU 分块 YUV 转换；双任务 H.264 编码与 WebSocket 链路；16 字节帧头自描述分辨率；下行命令 ping/status；断线自动重连；PC 保存/持久解码/MJPEG 预览；首帧超时后的完整 USB Host/UVC 重建 | 10 分钟以上稳定性；浏览器实时画面长期验收；C6 固件版本对齐；正式 Camera API；运行期改分辨率 |
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

原动画源文件已删除（2026-09-12）：`main/gif_assets.c/.h` 是 `tools/gif2c.py` 生成的
22k 行内嵌资源，从未编译进固件，且已被 `screen_carousel` 的 TF 卡运行时解码取代。
表情素材本身保留在 TF 卡和 `项目文档/` 里，需要重新生成时跑 `tools/gif2c.py`。

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
- 支持多组 MJPEG/YUY2 分辨率；当前选择 800×600 MJPEG，设备声明 30 FPS，实测 UVC 完整帧 20～30 FPS（随负载波动，见下方丢帧结论）。
- 帧数和累计字节持续增加，800×600 约 39～42 KB/帧，空帧为 0。
- 原先 `@0.0FPS` 打不开流的问题已通过读取设备实际帧率修复。

摄像头驱动由独立 Camera Task 运行。当前 `components/video_streamer` 接收 800×600 MJPEG，按 20 FPS 门控放入 3 个 PSRAM 输入槽；实测 MJPEG 是 YUV422 采样，ESP32-P4 硬件 JPEG 解码器直接输出 `U Y0 V Y1`。软件只对相邻两行 U/V 求平均并重排为 P4 v1.x H.264 编码器要求的 `O_UYY_E_VYY`，再编码为目标 4 Mbps、GOP 20 的 Annex-B H.264。编解码任务把码流放入 4 个输出槽，独立上传任务通过 WebSocket 二进制帧发送（每帧前置 16 字节自描述头携带宽高/帧率/帧类型/序号），网络等待不会阻塞编解码。注意输出槽内编码缓冲必须保持 128 字节 cache line 对齐（槽按 128 对齐分配，编码数据放 +128、帧头放 +112），否则 `esp_cache_msync` 失效会让 CPU 读到旧帧数据。原 RGB565 拆色与 BT.601 全帧换算和 YUY2 主线均已删除。

冷启动重新插电时视频链路已实机打通。只复位 P4、摄像头不断电时，首个目标 MJPEG 流可能持续 0 帧；640×480 和 800×600 均已证明完整 `stop/close` 后保持原格式重新 `open/start` 可以恢复。当前 800×600 在约 5 秒（`CAMERA_STALL_LIMIT=1` × 5 秒报告周期）触发原地重试后恢复取流；第二次仍失败才轮转格式。

2026-09-12 完整档位（WiFi + LVGL + SD + WebSocket 全开）实测：UVC 完整帧 20～30 FPS、
丢帧 2.6%～32%，H.264 编码与发送 15～20 FPS、`sent == encoded`、`send_fail=0`
（**所以上传路径不是瓶颈**）；平均耗时 JPEG 6.7 ms、YUV 重排 15.3 ms、H.264 8.3 ms。
**800×600@20 FPS 目标尚未达成**，卡在 UVC 丢帧而非编解码耗时。逐窗口读数见附录 A.2。

**丢帧的可控变量是编解码链路本身，不是 URB 位置，也不是 WiFi/LVGL。** 当天日志里
WebSocket 曾断线约 56 秒，这段时间 `video_streamer_submit_jpeg()` 的入口门控把整条链路
关掉，`encoded / JPEG_DEC / YUV_CONV / H264_ENC` 全部归零，于是同一次运行内出现了天然对照：

| 窗口 | 编解码链路 | UVC complete | UVC drop | callback_gap_max |
| --- | --- | ---: | ---: | ---: |
| t=872～988 s | 运行（16 fps） | 22.9 fps | **23.8%**（24 窗均值） | 9 ms |
| t=998～1043 s | 关闭（0 fps） | **29.4 fps** | **2.1%**（10 窗均值） | 9 ms |
| t=1048 s 起 | 恢复 | 22.9 fps | 回到 21～28% | 9 ms |

这 56 秒里 WiFi 始终是通的（TCP 反复重连成功）、GIF 轮播一直在跑（`CAROUSEL: GIF 预读完成`
没停）、UVC 回调里那次 256 KB `memcpy` 照常执行（`camera_frame_cb` 的复制与 WS 状态无关）。
**三者都被排除，唯一变化的是编解码链路开不开。** 机理是每帧 JPEG 解 6.7 + YUV 重排 15.3
+ 编码 8.3 ≈ 30 ms 的 PSRAM 密集访问，约 16 次/秒 ≈ 半个核持续压 PSRAM 仲裁，与 USB ISOC
DMA 抢带宽。

**`callback_gap_max` 不再是有效判据。** 上面两种状态都是 9 ms，与丢帧率不相关；此前记录的
"完整档位 22 ms"未复现。它是个窗口内最大值统计量，单次抖动就能顶上去。**据此进行的逐档
二分计划作废**（见第 11 节）。

注意 `encoded` 会低于 UVC complete，这部分**不是**全靠丢帧解释：`video_streamer_submit_jpeg()`
的 20 FPS 提交门控把超出的帧计入 `rate_limited`。该计数器此前没有打进周期日志，于是
`drop_input=0 drop_output=0` 与 `encoded < complete` 并存却看不出差在哪里；2026-09-12 已把
`rate_limit=` 加进 `[VIDEO] Queue` 行。另有一个独立通道：约 10% 的完整帧在
`video_jpeg_structurally_valid()` 处被判定为帧内损坏并计入 `invalid`，而 camera_driver 的
SOI/EOI 浅检查看不到它们（两边检查深度不同），同一轮改动已给 `invalid` 加了失败原因归因。

1280×720 曾持续发生 UVC 溢出和 JPEG 解码错误，已放弃作为当前实时分辨率。PC 端采用有界 H.264 队列、持久 PyAV 解码器和 `/preview.mjpg` 长连接，等待用户对 800×600 实时画面的最终验收。

注意：开发板是 ESP32-P4 revision v1.3，`CONFIG_ESP_REV_MIN_FULL=100`。`esp_h264` 1.4.0 的通用格式表虽然列有 `RGB565_LE`，该修订的实际参数检查只接受 `O_UYY_E_VYY`，所以不能直接把 RGB565 传给编码器。当前 YUV422 快速路径是在这一硬件约束下删除主要软件换算开销的安全实现。

### 2026-09-11 最新视频对照状态

- 当天烧录的是隔离档位 `CAMERA_TEST_UVC_H264_ONLY=6`（文档里曾误写成 `CAMERA_TEST_CODEC_ONLY`，代码中从未有过这个名字）：暂停天气 HTTPS、UI/LVGL、WiFi/ESP-Hosted 和 WebSocket，只保留 UVC→JPEG 硬解→CPU YUV 转换→H.264 硬编，用于隔离 USB、编码和 PSRAM 的关系。
- 摄像头格式描述符声明 MJPEG 帧率为 30/25/15 FPS，没有 800×600 MJPEG 20 FPS 离散档位；因此代码请求 20 FPS 时回退到设备默认的 30 FPS，编码器侧上限仍为 20 FPS。
- UVC 当前自动选择 `ISOC alt=1`：`effective_MPS=3072`，`BULK=0`；URB 配置为 `8 × 16 KB`（每块 16 KB 被 MPS=3072 向上取整成 6 个 ISOC 包，实占 18432 B）。曾对照测试 `alt=2 / effective_MPS=2436`，UVC 丢帧没有稳定改善，已恢复自动选择。
- MJPEG handoff 复制池已由 `4 × 512 KB` 改为 `3 × 256 KB`，仅减少约 1.25 MB PSRAM 占用，不解决总线争用。
- CPU YUV 转换已改为每 8 个 2 行宏块分块处理，块间 `taskYIELD()` 加 20 µs 总线空隙；实测 `YUV_CONV=15.0～15.4 ms`，短测 UVC 丢帧仍为 `31.6%～36.7%`，因此该改动目前只视为降低连续访问窗口，不能视为已修复丢包。
- 最新短测统计：`UVC complete=19.0～20.7 FPS`、`H264=14.9～15.5 FPS`、`JPEG_DEC=6.6～6.8 ms`、`H264_ENC=8.0 ms`、`callback_gap_max=10 ms`，未见 SOI/EOI 缺失、非法头或 handoff 拒绝；启动首帧超时后完整重建成功，首帧约 249 ms。
- P4 v1.3 上 DMA2D/PPA 的 YUV422→YUV420 硬件路径不可用，初始化返回 `ESP_ERR_NOT_SUPPORTED`，每帧安全回退 CPU；不要删除版本判断强行调用私有 DMA2D API。

### 2026-09-12 两个不同的问题：开机重启循环 vs 稳态丢帧

这两个问题的成因不同，此前被并成了一条"URB 争 PSRAM"的结论，下面拆开。

**(a) 开机重启循环 —— 只有把 URB 放内部 RAM 才会触发。**
`8 × 18432 B = 144 KiB`，而内部 DMA 池只有 146 KiB（启动日志
`esp_psram: Reserving pool of 146K of internal memory for DMA/internal allocations`）。
codec-only 档位（`CAMERA_TEST_UVC_H264_ONLY`）里摄像头是第一个来抢池子的，144 挤 146 刚好
够；**完整档位的 WiFi(SDIO)/LVGL/LCD SPI DMA 会先占用同一个池，UVC 必然分配失败**。
失败会被组件放大成崩溃：`uvc_host.c` 的 `uvc_transfers_free()`（约 341 行）释放后既不置空
`xfers` 也不归零 `num_of_xfers`，而 `uvc_host_stream_open` 的错误路径（约 880 行 →
`uvc_device_remove`）会二次调用它 → double free →
`assert failed: heap_caps_free ... free() target pointer is outside heap areas` →
**开机重启循环**，而不是一条错误日志。该组件在 2.5.1 版本更新后没有本地补丁，需要重打
幂等补丁或依赖下面的配置规避。

**(b) 稳态丢帧 —— 编解码链路的 PSRAM 流量，与本条 (a) 无关。**
证据是第 7 节上方那个同一次运行内的 A/B：完整档位下 URB 是落在 PSRAM 的，而只要把编解码
链路关掉，丢帧就从 23.8% 掉到 2.1%。**URB 在不在 PSRAM 不是稳态丢帧的变量。**

**最终配置固定为 `8 × 16 KB URB + CONFIG_USB_HOST_DWC_DMA_CAP_MEMORY_IN_PSRAM=y`**
（URB 数据缓冲 + HCD DMA 描述符一起落 PSRAM）。**这条配置是为了规避 (a) 的重启循环，不是
为了降低 (b) 的丢帧**；代价是完整档位稳态丢帧 19%～29%。

附录 A.1 的缓冲容量对照（96×32 KB → 8×16 KB 把丢帧从 ~35% 压到 ~10%）仍然成立，但它原先
配套的机理——"PSRAM 仲裁延迟 ISOC 回调，`callback_gap_max` 因此升高"——已被上面的 A/B 取代：
如果是回调延迟导致丢帧，`callback_gap_max` 应该在丢帧时同步升高，而实测丢帧 23.8% 与 2.1%
两种状态下它都是 9 ms。缩容有效的原因目前**未有解释**，不要再拿它当结论用。

## 8. WiFi 当前实现

当前链路目标为：

```text
ESP32-P4 → ESP-Hosted/SDIO → 板载 ESP32-C6 → WiFi 路由器 → DHCP
```

已建立独立 `components/network` 和 `components/provisioning` 组件。`main.c` 对网络只调用 `network_manager_init()` 和 `network_manager_start()`，没有放入 WiFi/BLE event handler。Network Manager 负责 NVS、TCP/IP、默认 STA netif、配网状态和 IP 结果；WiFi Manager 负责 `esp_wifi_remote` 初始化、WiFi/IP 事件和每 2 秒自动重连；Provisioning Manager 负责官方 BLE 配网协议与 Security 2。

固定 SSID/密码已从源码删除。C6 无凭据时启动 BLE 广播，设备名为 `LUMMISS_XXXXXX`，使用自定义 UUID `021a9004-0382-4aea-bff4-6b3f1c5adfb4`、用户名 `lummiss` 和每设备随机 PoP。凭据验证成功后等待约 5 秒返回最终状态，再停止广播并启动 OTA、UI、摄像头和视频。C6 有保存凭据时直接连接。App 对接约定见 `src/demo/BLE_WIFI_PROVISIONING.md`。

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
| 配网协议 | `network_provisioning 1.2.4`，BLE + Security 2 |

2026-09-14 真机验证已覆盖 BLE 扫描、Security 2 会话、凭据下发、C6 连接、DHCP 成功、配网服务关闭和正常业务启动；重新烧录正常配置后，也验证了“已有凭据直接联网”的启动路径。日志同时提示 Host 2.7.0 高于 C6 协处理器 2.3.0；当前 BLE/WiFi 与 UVC 并行运行正常，后续仍应升级 C6 从机固件并做完整回归。

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

1. **UVC 丢帧拖垮了编码帧率，20 FPS 目标未达成。** 丢帧来源已由同一次运行内的 A/B 定位为**编解码链路自身的 PSRAM 流量**：关掉链路丢帧 2.1%，打开 23.8%，而 WiFi/LVGL/GIF 轮播/回调 memcpy 在两者中都在跑（见第 7 节）。URB 必须落 PSRAM 是**另一个**问题——它防的是开机重启循环（144 KiB 挤不进 146 KiB 内部 DMA 池触发组件 double-free），与稳态丢帧无关。**当前完整档位实测：UVC complete 20~30 FPS、丢帧 19%~29%、H.264 15~20 FPS。** 下一步是降低编解码链路的 PSRAM 压力（YUV 重排 15.3 ms 是三项里最大的一项），**不要再做档位二分**。
2. **WebSocket 断线会恶化成 56 秒黑屏。** 2026-09-12 日志里 `transport_poll_write(0)` 触发一次后，重连上去不到 1 秒又断，连续 5 次，累计 56 秒无画面。成因是 `VIDEO_WS_SEND_TIMEOUT_MS` 过短（原 100 ms），被 `esp_websocket_client` 判为致命错误直接 abort；同时重连间隔是 8000 ms，比它自己注释里写的 2 秒长 4 倍。已改为写超时 2000 ms（与 `network_timeout_ms` 拆成两个宏）、重连 2000 ms，**待实机复测**。详见附录 A.6。
3. **800×600 网页预览待最终验收。** 完整联网档位（`CAMERA_TEST_FULL=0`）已在实机跑通、WebSocket 正常连接、`sent == encoded` 且 `send_fail=0`；但设备侧只有 15~20 FPS，预览会卡在这个帧率上。需等第 1 条的 PSRAM 压力结论出来、帧率提上去之后再做正式验收；验收前应先确认第 2 条的断线问题已复测通过，否则预览会周期性中断。
4. **短期设备链路已通过，长期稳定性未测。** 热复位完整重建、JPEG 完整性门控、双任务编解码/上传和 CPU 分块转换均已实机工作；需连续运行至少 10 分钟，再逐步扩展到长时间测试。
5. **动态首页待真机结论。** 构建通过不能证明镜像修正、中文字体、IP 自动定位和两个 HTTPS API 在设备网络上均正常，需要烧录后的照片及 `HOME_INFO` 日志。
6. **阶段 0 尚未冻结。** 除屏幕、USB 和 P4-C6 SDIO 外，关键器件型号与 GPIO 未定，会阻塞阶段 4、5、9、11。
7. **当前只完成第一层模块化。** 屏幕和摄像头已提取为驱动文件，但仍位于主组件，尚未拆成独立 ESP-IDF 组件、服务层和业务层。
8. **没有页面和状态机。** 当前静态 HOME 不能代表 HOME→LISTEN→THINK→REPLY→恢复流程，也无法做优先级和异常覆盖。
9. **屏幕背光不可控。** BL 接 3V3 无法满足休眠、低电量和亮度调节，需要正式硬件增加背光驱动和 PWM GPIO。

## 11. 按流程继续的顺序

1. **降低编解码链路的 PSRAM 流量**——这是唯一还在动的变量（见第 7 节 A/B）。按耗时排序，YUV 重排 15.3 ms 是三项里最大的一项，先评估 P4 v1.3 上 DMA2D/PPA 不可用时的替代路径；其次看 JPEG 解码 6.7 ms 的 PSRAM 读写量。**不要再做档位二分**：`callback_gap_max` 在丢帧 23.8% 和 2.1% 两种状态下都是 9 ms，对档位不敏感，拿它当判据只会得到噪声。若要继续分离变量，用已验证有效的手段——直接开/关整条编解码链路（WebSocket 断线就是这个效果），而不是切档位。
2. **复测 WebSocket 断线修复**（附录 A.6）：烧录后长时间跑，确认不再出现"连上不到 1 秒又断"的循环。判据是 `transport_ws: Error transport_poll_write(0)` 不再出现；若仍出现，说明 2000 ms 写超时还不够，下一步要给 socket 加 `SO_SNDTIMEO`（写超时只约束那次 select，不约束随后的 `send()`）。
3. **完整档位（`CAMERA_TEST_FULL=0`）现已跑通**：启动 `src/demo/_start_camera_server.bat`，打开 `http://127.0.0.1:8000/`，确认接收速率（`sent == encoded`，`send_fail=0`）和网页预览丢帧。当前瓶颈在设备侧 UVC 丢帧，不在上传。
4. 只有在 CPU 分块转换前后完成同条件对照后，才继续调整 tile 大小或短空隙；当前不能把 UVC 丢帧下降归因于该优化。传输层已于 2026-09-10 由 HTTP 长连接替换为 WebSocket，并建立了下行命令通道（`/cmd?cmd=ping|status`）。
5. 补齐阶段 0 的 BOM、完整 GPIO 表和音频、传感器、电机接口定义；P4-C6 SDIO 引脚已经按厂商示例确定。
6. 验收 HOME 页动态时间、日期、天气与镜像方向，再建立 LISTEN、THINK、REPLY、SLEEP、FAULT 页面骨架；电量在阶段 11 完成电池管理后接入。
7. 继续把 display、ui、expression 拆分为独立组件，提供 `expression_play()` 等统一接口。
8. 完成阶段 3：实现 App Event Bus、UI 状态机、优先级、超时和状态恢复；只有 UI Task 调用 LVGL。
9. ~~H.264 HTTP 实时链路通过后替换为 WebSocket~~ **已完成（2026-09-10）**：P4 侧走 `esp_websocket_client`（`ws://<PC>:8001/ws`），每帧前置 16 字节自描述头（分辨率/帧率随帧携带）；PC 侧 HTTP(8000) 保留预览页、`/h264` 回退入口与 `/cmd` 下行命令。编码队列和 Network Manager 接口未动。后续要做运行期改分辨率：需把 `VIDEO_STREAM_WIDTH/HEIGHT` 从编译期宏改为运行期变量，并同步 `camera_driver.c` 的格式耦合（兜底格式表、格式提升、帧门控、`preferred_mjpeg` 判定），帧头自描述已解除 PC 端对分辨率的感知需求。

下一个会话开始时，先读取本文件、`盆栽陪伴机器人_开发文档.md` 和 `src/demo/CAMERA_UPLOAD_TEST.md`，再根据 H.264 串口统计、PC `/status`、浏览器预览及保存的 `.h264` 继续。修改 C/C++ 源码时继续使用中文注释。

## 附录 A. 视频链路实测数据

正文只留结论，逐窗口原始读数集中在这里。复测方法见 A.4。

### A.1 UVC 丢帧对照实验（codec-only 档位）

2026-09-12，`CAMERA_TEST_UVC_H264_ONLY`（档位 6），每组短测 60～70 秒，汇总时排除
设备启动后前 15 秒。

| 组别 | UVC FPS | UVC 丢帧 | H.264 FPS | H.264 耗时 | 结论 |
| --- | ---: | ---: | ---: | ---: | --- |
| 原固件，USB 96×32 KB PSRAM | 19.43 | 35.23% | 17.12 | 8.04 ms | 复现持续 skipped |
| 仅启用 16 KB 缓存回写分块 | 20.22 | 32.58% | 17.47 | 8.28 ms | 未解决 |
| H.264 输出槽 720000 B → 128 KB | 19.75 | 34.17% | 17.48 | 8.28 ms | 省约 2.25 MiB PSRAM，未解决 |
| H.264 DMA burst 128 B → 16 B | 17.28 | 42.37% | 11.50 | 21.44 ms | 负优化，已恢复 128 B |
| USB 8×16 KB **内部 RAM** | 30.05 | **0%** | 19.99 | 8.15 ms | 11 个稳态窗口 skipped/invalid/丢整帧全为零 |
| USB 8×16 KB PSRAM（反向对照） | 27.0 | 10.2% | 19.9 | 8.27 ms | `callback_gap_max` 1→8 ms |

**第 5 行的"内部 RAM 清零"只在 codec-only 档位成立，不能推广到完整档位**——见第 7 节 (a) 和 A.2。

**本表原先配套的机理已撤回。** 表内数据（同为 codec 运行时，96×32 KB → 8×16 KB 把丢帧从
35% 压到 10%）仍然成立，但当时把它解释成"PSRAM 仲裁延迟 ISOC 回调，因此 `callback_gap_max`
升高"是错的：后续 A/B 显示丢帧 23.8% 与 2.1% 两种状态下 `callback_gap_max` 都是 9 ms。
缩容为什么有效，**目前没有解释**，不要再拿这套机理往下推。

### A.2 完整档位逐窗口读数（`CAMERA_TEST_FULL`，URB 8×16 KB + 落 PSRAM）

2026-09-12，WiFi 在 t=11.6 s 拿到 `192.168.1.32` 并连上 WebSocket，8 个 USB transfer
分配成功。约 70 秒稳态：

| 时刻 | UVC complete | UVC drop | encoded | callback_gap_max |
| --- | ---: | ---: | ---: | ---: |
| t=19.4 s | 28.5 | 5.3% | 19.7 | 22 ms |
| t=24.5 s | 29.3 | 2.6% | 20.1 | 22 ms |
| t=29.5 s | 28.6 | 4.6% | 19.7 | 22 ms |
| t=34.5 s | 24.5 | 18.0% | 17.3 | 22 ms |
| t=39.5 s | 22.7 | 24.5% | 15.1 | 22 ms |
| t=44.6 s | 20.5 | 31.8% | 15.9 | 22 ms |
| t=49.6 s | 24.3 | 19.2% | 17.9 | 22 ms |
| t=54.6 s | 21.9 | 27.2% | 16.9 | 22 ms |
| t=59.6 s | 25.5 | 14.7% | 19.1 | 22 ms |
| t=64.7 s | 24.9 | 17.2% | 19.1 | 22 ms |
| t=69.7 s | 26.5 | 11.9% | 19.3 | 22 ms |

- **丢帧在 t≈34.5 s 从 2.6~5.3% 跳到 12~32%**，时间点与
  `W esp-tls: Failed to open new connection in specified timeout` +
  `HOME_INFO: HTTPS 请求失败：ESP_ERR_HTTP_CONNECT` +
  `CAROUSEL: GIF 预读完成 [1/8]` 的循环重启重合。
  **这条线索已被证伪，不要再查。** 后续一轮日志在 t=993～1048 s 的 WebSocket 断线窗口里
  GIF 轮播与 HTTPS 超时**照常发生**，而丢帧降到 2.1%——那些事件不是丢帧的原因。
- `callback_gap_max` 全程 22 ms 的记录**未复现**：后续完整档位日志里它稳定在 9 ms，且与
  丢帧率不相关（丢帧 23.8% 和 2.1% 两种状态都是 9 ms）。原先"这 13 ms 增量就是
  WiFi/LVGL/SD/GIF 抢 PSRAM"的归因**已撤回**。它是窗口内最大值统计，对偶尔一次抖动敏感，
  不适合当判据。
- 全程 `sent == encoded`、`send_fail=0` → **上传路径不是瓶颈**（这条成立）。

### A.3 内存与配置算术

内部 DMA 池只有 146 KiB（启动日志 `esp_psram: Reserving pool of 146K of internal
memory for DMA/internal allocations`），而 URB 要：

```text
16 KB 被 MPS=3072 向上取整成 6 个 ISOC 包 → 每块实占 18432 B
8 × 18432 B = 147456 B = 144 KiB          （另有描述符和控制缓冲）
```

144 KiB 挤 146 KiB 在 codec-only 档位刚好够，完整档位的 WiFi(SDIO)/LVGL/LCD SPI 会先
占用同一个池 → 分配必然失败 → 被 `uvc_host.c` 的 `uvc_transfers_free()` double-free
放大成开机重启循环。所以 `CONFIG_USB_HOST_DWC_DMA_CAP_MEMORY_IN_PSRAM` 必须为 `y`。

当前 PSRAM 供给（相对实际每帧 <1 MB 的需求明显偏宽，是可回收项）：输入环 3×512 KB +
UVC 帧缓冲 3×512 KB + handoff 复制池 3×256 KB + 输出槽 4×128 KB ≈ **4.3 MB**。

### A.4 复测工具与原始数据位置

```powershell
python tools/capture_camera_serial.py --port COM17 --seconds 70 --output test_results/run.log
python tools/summarize_camera_serial.py test_results/run.log
```

采集工具不主动复位设备；汇总默认跳过设备启动后前 15 秒，分别报告窗口均值和累计数增量。

原始日志、固件快照与构建输出在 `test_results/20260912_cache/`（**约 800 MB，已被
`.gitignore` 排除，只在本机保留**）：各档位的 `build_*.log` / `flash_*.log` / `*.bin` /
`*.elf`、`baseline_firmware/`（改动前的源码快照）、`server.log`（PC 端接收速率）和
`preview/`。`A.1`/`A.2` 的数字都能从这些日志复算，删掉就失去可追溯性。

`src/demo/VIDEO_20FPS_VALIDATION.md` 是同一批实验的成文记录，与本附录内容重叠，
以本附录为准。

### A.5 编解码链路开/关 A/B 逐窗口（2026-09-12 第三轮，完整档位）

同一次运行内取得，未重启、未改配置。分界点是 WebSocket 断线：断线期间
`video_streamer_submit_jpeg()` 的入口门控（`!s_ws_connected` 即返回 false）让帧根本进不了
队列，于是 `JPEG_DEC / YUV_CONV / H264_ENC` 全部为 `0.00`——**整条编解码链路被关掉**，
而 UVC、WiFi、LVGL/GIF 轮播、回调 memcpy 全都照常。

| 时刻 | 链路 | UVC complete | UVC drop | encoded | JPEG/YUV/H264 | gap_max |
| --- | --- | ---: | ---: | ---: | --- | ---: |
| t=872.8 s | 运行 | 22.5 | 24.7% | 15.7 | 6.6/15.3/8.3 | 9 ms |
| t=897.9 s | 运行 | 23.1 | 22.7% | 15.9 | 6.8/15.5/8.3 | 9 ms |
| t=943.2 s | 运行 | 24.1 | 19.9% | 16.3 | 6.7/15.3/8.4 | 9 ms |
| t=988.4 s | 运行 | 23.5 | 21.9% | 16.3 | 6.8/15.3/8.3 | 9 ms |
| **t=998.5 s** | **关闭** | **30.0** | **0.0%** | **0.0** | **0/0/0** | 9 ms |
| t=1003.5 s | 关闭 | 28.3 | 6.0% | 2.2 | 6.7/15.4/8.4 | 9 ms |
| **t=1008.5 s** | **关闭** | **30.0** | **0.0%** | **0.0** | **0/0/0** | 9 ms |
| t=1013.6 s | 关闭 | 28.5 | 4.7% | 1.8 | 6.7/15.3/8.3 | 9 ms |
| **t=1018.6 s** | **关闭** | **30.0** | **0.0%** | **0.0** | **0/0/0** | 9 ms |
| t=1023.6 s | 关闭 | 29.3 | 2.6% | 0.0 | 0/0/0 | 9 ms |
| t=1028.6 s | 关闭 | 29.1 | 3.3% | 2.0 | 6.7/15.6/8.2 | 9 ms |
| t=1033.7 s | 关闭 | 29.9 | 0.0% | 0.0 | 0/0/0 | 9 ms |
| t=1038.7 s | 关闭 | 28.9 | 4.0% | 3.0 | 6.7/15.2/8.4 | 9 ms |
| t=1043.7 s | 关闭 | 30.0 | 0.0% | 0.0 | 0/0/0 | 9 ms |
| t=1048.7 s | 恢复 | 23.7 | 21.2% | 15.3 | 6.7/15.3/8.3 | 9 ms |
| t=1053.8 s | 恢复 | 23.5 | 21.3% | 15.5 | 6.9/15.3/8.3 | 9 ms |

- 运行段 24 个窗口均值：complete 22.9、drop **23.8%**；关闭段 10 个窗口均值：complete
  **29.4**、drop **2.1%**。
- 关闭段里那几个 `encoded=2.0~3.0` 的窗口是重连握手中链路短暂恢复的一瞬，对应 UVC drop
  回升到 2.6~6.0%——**方向和幅度都一致，进一步支持这个对照**。
- 三重控制证据：① WiFi 通（同一 56 秒内 TCP 对 `192.168.1.66:8001` 重连成功 5 次）；
  ② `CAROUSEL: GIF 预读完成 [n/8]` 全程照常循环；③ `camera_frame_cb` 里那次 256 KB
  `memcpy` 与 WS 状态无关，关闭段照常执行。

**`invalid` 通道**：同一轮日志里 `invalid` 以约 2.2 帧/秒稳定增长（≈ 完整帧的 10%），而
camera_driver 报 `SOI缺失=0 EOI缺失=0`。原因是两边检查深度不同：camera_driver 只查
SOI/EOI 是否在场，`video_jpeg_structurally_valid()` 会逐段走 marker 结构。**推断**是 ISOC
丢包打坏帧头段长度、头尾标记却仍然完好，正好落在两者之间——尚未验证，已给 `invalid` 加了
按原因拆分的日志（`head/marker/segment/sof/no_sos/scan/no_eoi`）用于确认。

**对账**：`rate_limit` 已加入 `[VIDEO] Queue` 行。此前 `drop_input=0 drop_output=0` 与
`encoded < complete` 并存时无法对账，现在四个计数器（`drop_input` / `drop_output` /
`rate_limit` / `invalid`）加 `send_fail` 能把 complete → encoded 的差额补平。

### A.6 WebSocket 断线恶化成 56 秒黑屏（2026-09-12）

```
993316  W transport_ws: Error transport_poll_write(0)        ← 首次断开
1001345 I WebSocket 已连接                                     ← 等了 8.03 s
1002027 W transport_poll_write(0)                             ← 只活了 682 ms
1010057 已连接 → 1010591 断开                                     534 ms
1018631 已连接 → 1019258 断开                                     627 ms
1027300 已连接 → 1027949 断开                                     649 ms
1035975 已连接 → 1036923 断开                                     948 ms
1044852 已连接 → 存活（t≈1048 s 起链路与编码恢复正常）
```

**机理**（组件源码，非本工程代码）：`esp_websocket_client_send_bin(timeout)` 把 timeout
换算成 `transport_ws.c` 里**一次** `select` 可写等待；等不到返回 ≤0，被
`esp_websocket_client.c` 判为**致命**错误，直接 `abort_connection()`，再等
`reconnect_timeout_ms` 才重连。公开 API 关不掉这条失败路径。

**为什么 100 ms 不够**：单帧 H.264 约 30 KB 以上，而 lwIP 判 socket 可写的门槛
`TCP_SNDLOWAT = TCP_SND_BUF/2 = 32767 字节`——**"poll 说可写"不保证装得下一帧**。日志里
`max_SEND` 反复出现 122 / 146 / 149 ms，全部超过 100 ms 预算。

**已做的修改**（`components/video_streamer/video_streamer.c`）：

| 宏 | 原值 | 新值 | 理由 |
| --- | ---: | ---: | --- |
| `VIDEO_WS_SEND_TIMEOUT_MS` | 100（原名 `VIDEO_WS_TIMEOUT_MS`） | 2000 | 这是决定连接生死的超时，必须大于最坏写窗口 |
| `VIDEO_WS_NETWORK_TIMEOUT_MS` | 与上者共用 100 | 100 | 拆出来，只做客户端读写轮询，不参与致命判定 |
| `VIDEO_WS_RECONNECT_MS` | 8000 | 2000 | 与它自己的注释对齐——注释写的是"断线后 **2 秒**重试"，值却是 8000，多出 4 倍黑屏时间 |

放宽写超时的代价是阻塞期间占住输出槽，但断线代价是重连 2 秒起，取小的那个。这个修法在
已删除的 `components/mjpeg_streamer/` 里实测过：改成 2000 ms 后 `transport_poll_write(0)`
归零。**本次改动尚未实机复测。**

⚠️ 写超时只约束那次 `select`，不约束随后的 `send()`（socket 是阻塞的，没设 `SO_SNDTIMEO`）。
链路彻底卡死时 `send()` 要等 TCP 重传耗尽（`CONFIG_LWIP_TCP_MAXRTX=12`）才返回。若复测后
仍出现 `transport_poll_write(0)`，下一步就是给 socket 加 `SO_SNDTIMEO`。
