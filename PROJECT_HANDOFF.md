# Lummiss 盆栽陪伴机器人项目交接说明

更新时间：2026-09-18

## 1. 当前结论

项目目前处于“基础工程 + 硬件可行性验证”阶段，还没有形成产品最小闭环。

- 阶段 1 已建立正式基础入口：`main.c` 使用 FreeRTOS 创建 UI Task 和 Camera Task，Network Manager 已负责 NVS 基础初始化。
- 阶段 2 已完成 ST7789、LVGL 8.4 和动态时间首页代码及构建。首页使用网络校时、IP 自动定位与 Open-Meteo 天气 API，显示日期、星期、天气、温度和大号时间；电量留到电池管理阶段。镜像参数已修正，动态数据和屏幕效果仍待实机验证。
- 阶段 6 的 BLE + WiFi 配网链路已经实机通过：ESP32-P4 经 ESP-Hosted/SDIO 使用板载 ESP32-C6 的 BLE Controller 和 WiFi。首次启动以 `LUMMISS_XXXXXX` 广播，App 通过官方 Unified Provisioning Security 2 下发凭据；成功后停止广播并启动业务。已有凭据时直接联网，断线自动重连。
- 阶段 8 的历史完整链路基线为 `800×600 MJPEG(YUV422) → P4 硬件 JPEG → CPU YUV 转换 → H.264`，曾测得 UVC drop 19%～29%、H.264 约 15～20 FPS。2026-09-17 使用 HBVCAM 完成了新的 profile 5 分辨率对照：640×480 短时窗口约 30 FPS 且未见丢帧，1280×720 约 14～16.7 FPS 并有 UVC/JPEG 异常，细节见下方记录。当前 PC 预览宏已关闭；640×480 尚待长时间稳定性验收。profile 8 的 YUV 算法 A/B 是独立测试，不能与完整链路结果混为一谈。
- 2026-09-11 已确认：当前 P4 v1.3 + ESP-IDF 5.5.5 不能可靠使用 PPA/DMA2D 做 YUV422→YUV420；工程保留能力探测并回退 CPU。CPU 转换改为 8 个 2 行宏块一段，块间调度让出并插入 20 µs 短空隙，当前转换耗时约 15.0～15.4 ms。该改动未使短测 UVC 丢帧明显下降，剩余瓶颈仍是 H.264/PSRAM 与 USB ISOC 的长期争用或调度。
- **2026-09-15 视频上传目标变更**：正式链路使用 OTA 下发的 `wss://` WebSocket 地址和动态 Token、`Device-Id`/`Client-Id` 头；小智音频与视频共用该连接。2026-09-17 为浏览器预览测试临时启用了 PC `ws://192.168.1.14:8001/ws` 覆盖，测试结束后已将 `VIDEO_STREAM_PC_PREVIEW_ENABLED` 设回 `0`。当前构建不再主动覆盖 OTA 地址；若 OTA 响应缺少 `websocket.url`，云端视频连接不会因此自动恢复。仓库内 `README.md` 与 `CAMERA_UPLOAD_TEST.md` 仍有旧 PC 地址描述。
- **2026-09-15 音频与小智**：新增 `components/xiaozhi_audio`（板载 ES8311 采集/播放 + Opus 编解码 + 小智协议）。语音链路**已端到端打通**（上行 Opus → 服务端 ASR → LLM → TTS → 下行 Opus 播放），且**摄像头推流时同样可用**——同日稍早那份"与视频共存时失效"的 A/B 结论**已被推翻**（更正见附录 A.7）。当前第一优先级问题改为**内部 DMA 池被挤碎**、ESP-Hosted 收包路径取不到缓冲就 assert 重启，见第 10 节第 1 条与附录 A.9。
- **2026-09-16 UVC 新摄像头固定冷启动诊断**：针对 HBVCAM（VID `058F`、PID `3822`）增加两个编译期固定测试档：`640×480 MJPEG@30` 和 `1280×720 MJPEG@30`。纯 UVC 档位一次运行只请求一个分辨率，失败后不轮转格式，必须重新构建并冷启动/重新枚举摄像头。保持 `alt=1`、`effective_MPS=3072`、`8×16 KB URB`、6 packets/transfer 和 26 字节 Probe/Commit 不变。
- **2026-09-17 HBVCAM 完整预览分辨率对照测试**：profile 5 开启 UVC、JPEG 解码、YUV 转换、H.264、WiFi/WebSocket 和 UI/SD；分别对 `1280×720 MJPEG@30` 与 `640×480 MJPEG@30` 独立构建/启动。`1280×720` 观察到 UVC complete 约 `23.4～27.0 FPS`、drop `11.8%～25.9%`、handoff rejected `33～46`/统计窗，H.264/发送约 `14～16.7 FPS`；YUV 平均约 `30.8～31.3 ms`，出现 JPEG 解码错误 259、缺 EOI 无效帧和输出丢帧。`640×480` 的日志窗口观察到 UVC complete `29.8～30.0 FPS`、drop `0%`、handoff rejected `0`，稳态 H.264/发送约 `30.2 FPS`；JPEG/YUV/H.264 平均约 `4.46/10.57/5.41 ms`，统计窗内无无效帧、队列丢弃或发送失败。结论：当前浏览器预览优先使用 `640×480`；640×480 这轮只记录了短时稳态窗口，尚非长时间稳定性验收。测试时本地 PC 地址覆盖使 WebSocket 成功连接；OTA 响应自身缺少 `websocket.url`。测试后已关闭本地 PC 覆盖地址（`VIDEO_STREAM_PC_PREVIEW_ENABLED=0`）；H.264/上传代码仍保留，OTA 若提供有效 WebSocket 配置仍可使用云端地址。COM17 的 ClearCommError/重连没有伴随固件 panic 记录。
- **2026-09-16 UVC 组帧根因定位**：640×480 日志中 SOI/EOI、FID、EOF 均存在，但 `frame_len` 在约 2136 字节后停止。原因是 `uvc_isoc.c` 对 `actual_num_bytes==0` 的普通零长度 ISOC 包统计后错误设置 `skip_current_frame`；frame buffer 没有释放，后续同 FID/PTS payload 因 skip 标志无法继续追加。现已改为零长度包只计数并忽略，并加入 `empty_inside_active_frame`、`frame_abort_by_empty`、`header_only_inside_active_frame`、`frame_abort_by_header_only` 及事件中的 `active=before→after` 诊断。该修复已构建通过，尚待开发板重新烧录后的 640×480 冷启动实测确认。
- **2026-09-17 YUV 转换独立 A/B**：在 640×480 MJPEG@30、profile 8（UVC + JPEG 硬解 + YUV 转换）下，分别独立运行 ref（约 69 秒）和 sample_even（约 102 秒）；H.264、WiFi/WebSocket、UI/LVGL、SD 均关闭，DMA2D 不支持并回退 CPU。排除首个部分统计窗口后，sample_even 的 YUV 平均耗时 8.72 ms，ref 为 10.31 ms，约快 15.4%；两轮 UVC complete 均约 30.03 FPS、drop 均为 0%，handoff rejected 均为 0，rate_limit 均约 50.6 次/5 秒。该结果只说明隔离链路下转换更快且未观察到 UVC 回归；H.264 开启时的 PSRAM/USB 争用和浏览器端帧率仍未验证。sample_even 保持 Y 不变、U/V 直接取偶数行，颜色精度尚未评估。当前源码 `video_streamer.h` 的 `VIDEO_YUV_CONVERSION_TEST_MODE` 设为 `REF`，复测 sample_even 时需切换为 `VIDEO_YUV_CONVERSION_MODE_SAMPLE_EVEN`。
- **2026-09-18 人机交互外设驱动层落地**：新增 `components/mech_button`（机械按键 GPIO22）、`components/touch_key`（两路 TTP223 触摸 GPIO52/51，当普通数字 GPIO 读，不走 P4 内部触摸外设）、`components/ambient_led`（两路 WS2812 氛围灯 GPIO20/21，RMT 后端），并新增档位 9 `CAMERA_TEST_PERIPH_ONLY`（UVC + 外设自检）与 `main/peripheral_test.c` 自检入口。构建通过、档位 9 已在开发板实机运行：**未干扰 UVC**（complete 29.8～30.2 FPS、drop 0.0%、handoff rejected 0），开机静默期自检**无幽灵输入事件**。**这三组引脚的物理接线只属于将来要打的新 PCB**，开发板上并无对应接线（详见第 6A 节）。
- **2026-09-18 视频上传改为按需开启**：删掉了两处「开机自动开视频」的默认值 —— `video_streamer.c` 的 `s_stream_enabled = true`（编译期就开着）和 `camera_driver.c` 里基于 `CAMERA_VIDEO_STREAM_ENABLED = 1` 的强制启用（摄像头初始化完就调 `set_enabled`）。现在开机默认 `IDLE`：WiFi、WebSocket、Hello、小智音频全部照常，但**不上传任何 H.264**；必须等服务端下发 `{"type":"video","state":"start"}` 才进入 `STREAMING`。停止、断线自动停、重连不自动恢复，详见第 7A 节。
- **2026-09-18 云端通信迁移到三通道协议（阶段一、二已实机打通）**：后端已从「OTA + 单条 Agent WebSocket」迁到 MQTT 控制 + UDP Opus 音频 + WebRTC 视频。固件按编译开关 `CONFIG_CLOUD_PROTOCOL_V3`（默认，`main/Kconfig.projbuild`）切换：OTA 取 MQTT 六字段 → MQTT hello v3 → Server Hello（session_id + UDP 参数）→ **UDP + AES-128-CTR 双向 Opus**。旧 Agent WebSocket 在 V3 下完全不建立；`CONFIG_CLOUD_PROTOCOL_LEGACY_WS` 保留旧协议仅作回滚。实机已验证：全双工对话（`tx=258 rx=213` 零错、`SPK frames=213`）、MCP 握手 + 工具调用（音量控制）、多轮自动对话。**WebRTC（阶段四）未实现**；协议细节与踩坑见第 8A 节。
- UI 状态机、传感器、电机、专注模式、电源管理和整机联调均未进入实现阶段；触摸与氛围灯已有驱动层，但尚未接 App Event Bus / 业务状态，也缺少实物（按键 / TTP223 模块 / 灯带）验收。

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
| 唤醒词/语音前端 | `espressif/esp-sr` ~2.3.0（AFE + Hi-Max 唤醒模型） |
| 氛围灯组件 | `espressif/led_strip` 3.0.3（RMT 后端，**显式关闭 DMA**） |
| 默认串口 | COM17 |

工程根目录必须保持英文路径。旧中文路径曾导致 Python/Kconfig 的 GBK 解码错误和 Ninja 乱码路径错误。

## 3. 当前基础工程结构

`src/demo` 已从两个独立测试入口改为一个联合基础工程：

| 文件 | 职责 |
| --- | --- |
| `main/main.c` | 唯一 `app_main()`，按档位启动 Network / 首页信息 / UI Task / Camera Task |
| `main/test_profile.h` | **启动组合测试档位的唯一定义点**：档位号 0~9、档位→功能位映射、`TP_HAS()` 和 `test_profile_name()`；档位 9 = `CAMERA_TEST_PERIPH_ONLY`（UVC + 外设自检） |
| `main/display_driver.c/.h` | ST7789、LVGL 和动态时间天气首页 |
| `main/screen_carousel.c/.h` | TF 卡 GIF 轮播：挂载 SD、预读任务、LVGL 定时器每 5 秒换 `lv_gif` 的 `src` |
| `main/camera_driver.c/.h` | USB Host、UVC 枚举、格式轮转、卡死恢复、帧回调和视频流统计 |
| `components/home_info/home_info.c/.h` | IP 定位、SNTP 校时、Open-Meteo 天气和首页数据快照 |
| `components/network/network_manager.c/.h` | 网络总入口、BLE 配网状态、NVS/netif 和 DHCP 结果 |
| `components/network/wifi_manager.c/.h` | WiFi 事件、C6 已保存凭据连接和自动重连 |
| `components/provisioning/provisioning_manager.c/.h` | BLE 配网、Security 2、设备名、每设备 PoP 和配网生命周期 |
| `BLE_WIFI_PROVISIONING.md` | App 对接参数、二维码格式、配网流程和重新配网接口 |
| `components/sd_card/sd_card.c/.h` | SDMMC Slot 0 挂载，含片上 LDO ch.4 供电和重试 |
| `components/video_streamer/video_streamer.c/.h` | MJPEG 队列、硬件 JPEG 解码、CPU 分块 YUV422→YUV420、H.264 编码；**并持有唯一的 Agent WebSocket**（视频二进制帧、小智文本/Opus 都走它），对小智暴露 `video_streamer_set_agent_callbacks()` / `..._agent_send_text()` / `..._agent_send_audio()`。**视频上传默认关闭（`VIDEO_STATE_IDLE`）**，由服务端 `video` 命令经内部事件队列启停，见第 7A 节 |
| `components/xiaozhi_audio/xiaozhi_audio.c/.h` | ES8311 采集/播放、Opus 编解码、小智协议（hello/listen/tts/stt/ping/pong）；不自己建连接，全部经 video_streamer 的 Agent WebSocket |
| `components/xiaozhi_audio/wake_word.c/.h` | AFE 唤醒词检测（Hi-Max 模型，16 kHz 单声道），从 SPIFFS "model" 分区加载，在 `xiaozhi_audio_start()`（主任务上下文）初始化 |
| `components/ota_client/ota_client.c/.h` | 取 OTA 结果里的 `websocket_url` + 动态 Token，校验必须是 `wss://` 且 Token 非空；串口只打长度不打正文 |
| `components/board/include/board_pins.h` | **全板 GPIO 宏的唯一来源**，驱动里不许硬编码 GPIO 号；人机交互段按 `BOARD_USE_NEW_PCB` 分板型 |
| `components/mech_button/mech_button.c/.h` | 机械按键（GPIO22，外部上拉、按下为低）：40 ms 消抖、900 ms 长按、5 ms 轮询任务；对外 `button_init()` / `button_is_pressed()` / `button_register_callback()` |
| `components/touch_key/touch_key.c/.h` | 两路 TTP223 触摸（GPIO52/51，**当普通数字 GPIO 读**）：单击 / 长按 / 双击；命名 `TOUCH_KEY_1/2` 而非左右 |
| `components/ambient_led/ambient_led.c/.h` | 两路 WS2812 氛围灯（GPIO20/21，RMT 后端）：STATIC / BREATH / BLINK / FLOW，65 项呼吸 LUT（半余弦 + gamma 2.2），20 ms 渲染任务 |
| `main/peripheral_test.c/.h` | 档位 9 的自检入口：17 步灯效序列 + 输入事件回调计数 + 开机静默期结论行 |
| `components/video_streamer/dma2d_yuv.c/.h` | DMA2D 硬件 YUV422→YUV420（**本板 v1.3 不可用**，仅 codec-only 档位以外永不生效） |
| `partitions.csv` | 分区表：nvs 24K + phy_init 4K + factory 8M + **model 1M（SPIFFS，存放 ESP-SR 唤醒词模型）** + storage 6M |
| `tools/pc_camera_server.py` | PC 端 H.264 接收、保存、持久 PyAV 解码和 MJPEG 网页预览 |
| `tools/capture_camera_serial.py` | 采集串口日志到文件，供实机对照测试复盘 |
| `tools/summarize_camera_serial.py` | 汇总串口日志：跳过启动前 15 秒，报窗口均值和累计数增量 |
| `tools/capture_to_mp4.py` | 把接收到的 H.264 转存为 mp4 |
| `tools/gif_resize_for_sd.py` | 把表情 GIF 缩放到屏幕尺寸并写入 TF 卡目录 |
| `tools/gif2c.py` | 把 GIF 转成 C 数组（**当前产物 `main/gif_assets.c` 已删除**，改走 TF 卡运行时解码） |

当前统一构建目录为 `build`，使用 `sdkconfig.bletest`。该配置已关闭强制 BLE 配网并启用开发板实际使用的 ESP-Hosted SDIO。同目录下的 `sdkconfig` 是历史游离文件，不要用于当前构建（2026-09-18 已把它的 ESP-Hosted 段也改回 SDIO，但它与 `sdkconfig.bletest` 并非同一份配置，直接拿它构建会丢档位与 BLE 设置）。

**这条不能只靠自觉遵守**（2026-09-18 已因此挂过一次）：`8c3901c` 提交在改步进电机的同时，把 `sdkconfig.bletest` 的 ESP-Hosted 传输从 SDIO 翻成了 SPI，Wi-Fi 整条链路随之失效，而 commit message 对此只字未提。判断方法与修复见第 9 节「ESP-Hosted 必须是 SDIO」。

**2026-09-12 清理**：删除无调用者的 `components/mjpeg_streamer/`（2026-09-10 直通预览的遗留，全树零引用）和未编译进固件的 `main/gif_assets.c/.h`（22k 行生成资源，已被 TF 卡运行时解码取代）。测试档位宏从 `camera_driver.h` + `main.c` 两处 6 个手写布尔宏收敛到 `main/test_profile.h` 一处。

**2026-09-18 新增组件**：`components/mech_button`（按键）、`components/touch_key`（TTP223 触摸）、`components/ambient_led`（WS2812 氛围灯）三个独立驱动组件，以及 `main/peripheral_test.c/.h` 自检入口；全板 GPIO 宏统一收在 `components/board/include/board_pins.h`。详见第 6A 节。

**2026-09-18 新增组件（三通道协议迁移）**：`components/cloud_mqtt`（MQTT 客户端 + MCP 层 `cloud_mcp.c` + 工具注册表）、`components/cloud_udp`（UDP + AES-128-CTR 音频通道）、`components/network/time_service.c`（全工程唯一 SNTP 实例）。`main/Kconfig.projbuild` 新增 `CLOUD_PROTOCOL` choice（V3 默认 / LEGACY_WS 回滚）。**注意：V3 下 OTA 不再返回 `websocket` 段而返回 `mqtt` 六字段，旧 WS 链路完全不建立**；`ota_client` 的 websocket 解析仅在 LEGACY_WS 下生效。详见第 8A 节。

## 4. 按开发文档阶段审计

状态含义：完成表示代码、构建和已有验收证据均满足当前阶段；部分完成表示只完成其中一部分或缺少真机确认。

| 阶段 | 状态 | 已完成 | 仍缺少 |
| --- | --- | --- | --- |
| 阶段 0：需求冻结 | 部分完成 | 已有总体系统框图；屏幕、USB 摄像头接口已明确 | 完整 BOM；音频、触摸、传感器、LED、C6、电机、电源 GPIO 表；统一接口定义 |
| 阶段 1：基础工程 | 基本完成 | ESP-IDF 工程、串口日志、UI/Camera FreeRTOS 任务、NVS 初始化、PSRAM、16 MB Flash、分区表、ESP32-P4 v1.x 镜像 | NVS 的产品数据结构及读写验证 |
| 阶段 2：屏幕 | 部分完成 | ST7789 驱动、LVGL 8.4、320×240 动态时间首页、IP 定位、网络校时、天气 API、镜像修正、8 组表情资源保留、源码构建通过 | 真机颜色/方向/API/稳定性验收；LISTEN/THINK/REPLY/SLEEP/FAULT 页面；页面切换接口；电池管理完成后接入电量 |
| 阶段 3：UI 状态机 | 未开始 | 当前只有静态时间首页 | Event Bus、状态请求、优先级、覆盖、恢复、超时和异常 |
| 阶段 4：传感器/触摸/LED | 部分完成 | 按键（GPIO22）、两路 TTP223 触摸（GPIO52/51）、两路 WS2812 氛围灯（GPIO20/21）的驱动层与档位 9 自检入口已实现、构建通过；开发板实测未干扰 UVC 且无幽灵输入事件 | 土壤、光照、温湿度传感器；触摸到左右语义与 App Event Bus 的映射；灯效与业务状态绑定；实物（按键 / TTP223 模块 / 灯带）功能验收 |
| 阶段 5：音频 | 部分完成 | `components/xiaozhi_audio`：板载 ES8311 I2S 采集与播放、Opus 编解码、16 kHz 单声道 60 ms 分帧、上行/下行实时通路已实机工作；**唤醒词检测已集成**（ESP-SR AFE + Hi-Max 模型，代码已就绪，待实机复测） | AEC、全双工、产品化增益/音量、长时间稳定性 |
| 阶段 6：Wi-Fi | 部分完成 | P4-C6 ESP-Hosted/SDIO、独立 Network Manager、固定凭据、DHCP 实机成功、2 秒自动重连 | NVS 凭据、BLE 配网、HTTP 通用封装 |
| 阶段 7：小智 | 部分完成 | 客户端 hello、`listen` 启停、`tts`/`stt` 下行文本、二进制 Opus 下行、`ping`/`pong` 已实现；端到端语音链路实机打通；**唤醒词检测已接入**（ESP-SR AFE，WebSocket 连接后自动启用，检测到唤醒词后停止 MIC 上行并触发聊天，回答结束后恢复监听） | `abort` 与鉴权失败后的清 Token/重 OTA 未接；UI 状态映射、MCP |
| 阶段 8：摄像头 | 部分完成 | USB Host/UVC 枚举；800×600 MJPEG；JPEG 完整性门控；硬件 JPEG 解码；CPU 分块 YUV 转换；双任务 H.264 编码与 WebSocket 链路；16 字节帧头自描述分辨率；下行命令 ping/status；断线自动重连；PC 保存/持久解码/MJPEG 预览；首帧超时后的完整 USB Host/UVC 重建 | 10 分钟以上稳定性；浏览器实时画面长期验收；C6 固件版本对齐；正式 Camera API；运行期改分辨率 |
| 阶段 9：旋转底座 | 未开始 | 无 | 电机、编码器、回零、角度、启停和堵转保护 |
| 阶段 10：专注模式 | 未开始 | 无 | 全部功能 |
| 阶段 11：电源管理 | 未开始 | 无 | 电量、充电、休眠和各外设降功耗 |
| 阶段 12：整机联调 | 未开始 | 无 | 全部联调组合 |
| 阶段 13：稳定性 | 未开始 | 只有摄像头短期稳定收帧证据 | 72 小时、断网、热插拔、泄漏、温升和功耗测试 |

## 5. 第一阶段执行清单的实际状态

```text
[x] 建立 ESP-IDF 工程
[~] 确认 GPIO 分配：屏幕、高速 USB、P4-C6 SDIO 和新 PCB 的人机交互三组（按键 22 / 触摸 52、51 / 氛围灯 20、21）已固定；传感器、电机、电源仍未冻结
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

屏幕是**外接**的 GMT020-02-8P 小屏（ST7789），不是板载屏，**与 GPIO20 无关**。
下表与 `src/demo/components/board/include/board_pins.h` 的 `BOARD_LCD_*` 保持一致
（2026-09-17 提交 a908e83 改线后的结果）。更早那版 SCLK=GPIO20 / MOSI=GPIO32 /
RST=GPIO3 / DC=GPIO2 / CS=GPIO1 / BL=3V3 已失效，不要再照它接线：

| GMT020-02-8P | ESP32-P4 |
| --- | --- |
| GND | GND |
| VCC | 3V3 |
| SCL/SCLK | GPIO3 |
| SDA/MOSI | GPIO2 |
| RST/RES | GPIO1 |
| DC | GPIO5 |
| CS | GPIO4 |
| BL/BLK | GPIO47（程序控制） |

当前显示参数：

- ST7789 面板原生 240×320，LVGL 逻辑分辨率为横屏 320×240。
- SPI2，40 MHz，RGB565，颜色反相开启，显存偏移 `(0, 0)`。
- 背光 BL 走 GPIO47，`display_driver.c` 初始化时拉高点亮。若实物 BL 仍直接接 3V3，这一步只是空操作。
- 当前使用 LVGL 控件绘制 320×240 首页，联网前显示占位符，联网成功后更新真实数据。
- 时间由 NTP 网络校时维护；时区默认按中国 UTC+8 成立，定位成功后再按实际时区修正（见下方 2026-09-18 小节）。
- IP 定位服务（ipwho.is）提供城市坐标，Open-Meteo 返回当前温度与 WMO 天气码；两者都不通时时间照常走，只有天气停在「获取中」。
- 实机反馈原显示左右镜像，横屏旋转配置已改为 `swap_xy=true, mirror_x=true, mirror_y=false`。
- 电量区域已按当前阶段要求隐藏，待阶段 11 电池管理具备可靠数据后再加入。
- 天气每 30 分钟更新一次；定位每 6 小时更新一次；断线时保留最近一次成功结果。

原动画源文件已删除（2026-09-12）：`main/gif_assets.c/.h` 是 `tools/gif2c.py` 生成的
22k 行内嵌资源，从未编译进固件，且已被 `screen_carousel` 的 TF 卡运行时解码取代。
表情素材本身保留在 TF 卡和 `项目文档/` 里，需要重新生成时跑 `tools/gif2c.py`。

### 时钟曾被天气接口“卡死”（2026-09-18 定位并修复）

**现象**：屏幕停在 `--:--` / `--月--日 星期-` / `获取中`，但串口里
`HOME_INFO: 网络时间同步成功` 明明已经打印。

**根因是数据层的依赖链，不是显示驱动**：`home_info_get_snapshot()` 原来用

```c
snapshot->time_valid = s_clock_synced && s_timezone_valid;
```

而 `s_timezone_valid` **只在 IP 定位或天气接口成功时才被置位**。于是 ipwho.is 一旦不通，
「NTP 已同步」也照样不出时间 —— 校时和时区本来是两件事，被串成了一条链。

**修复**

| 位置 | 改动 |
| --- | --- |
| `components/home_info/home_info.c` | 启动时即把时区置为默认 UTC+8（`HOME_INFO_DEFAULT_UTC_OFFSET_SECONDS`）并标记有效：NTP 一成功就出时间；接口返回真实偏移后再覆盖 |
| 同上 | 定位 / 天气未就绪时的重试由 60 s 缩短为 20 s（`HOME_INFO_FAST_RETRY_SECONDS`），两者都就绪后回到 60 s 常规轮询 |
| 同上 | `http_get_json()` 增加 `label` 参数，日志可区分是「定位接口」还是「天气接口」失败 |
| `main/display_driver.c` | 首页时间 / 天气的有效性翻转时各打一行日志，串口可直接判断 UI 有没有拿到新数据 |

**排查这类问题先看新增的两行**

```text
DISPLAY: 首页时间：等待网络校时，暂显示占位符    ← 说明 time_valid 仍为 false
DISPLAY: 首页天气：等待天气接口，暂显示「获取中」
HOME_INFO: 定位接口请求失败：ESP_ERR_HTTP_CONNECT
```

若定位 / 天气持续报 `ESP_ERR_HTTP_CONNECT` 或连接超时，是该 WiFi 到
`ipwho.is`、`api.open-meteo.com` 的链路不通（境外 Cloudflare 线路常见），属网络问题而非固件问题；
**此时时间仍应正常显示**。换网络（如手机热点）可作快速对照。

2026-09-10 接入动态时间、天气 API 和镜像修正后的构建结果：

```text
lummiss_main.bin：0x19afb0，8 MB 应用分区剩余 80%
bootloader.bin：0x5310，Bootloader 分区剩余 13%
构建结果：通过
```

烧录后正常现象应为：屏幕先显示时间和天气占位符；WiFi 联网后串口依次输出“网络时间同步成功”“自动定位成功”和“天气更新”，页面自动替换为当前日期、星期、时间、天气和温度，不显示电量。文字应正常朝向且不再左右镜像。

## 6A. 人机交互外设（按键 / TTP223 触摸 / WS2812 氛围灯）

2026-09-18 新增，属阶段 4 的驱动层。要求是在现有组件化结构下新增三类外设并自带自检，
不改动已稳定的 UVC / JPEG-H264 / WiFi / ESP-Hosted / BLE 配网 / SD / 音频 / LVGL / 电机。

### 引脚（唯一定义点：`components/board/include/board_pins.h`）

| 外设 | 宏 | 引脚 | 说明 |
| --- | --- | --- | --- |
| WS2812 灯带 A | `BOARD_WS2812_A_GPIO` | GPIO20 | 12 颗，RMT |
| WS2812 灯带 B | `BOARD_WS2812_B_GPIO` | GPIO21 | 12 颗，RMT |
| 机械按键 | `BOARD_KEY_GPIO` | GPIO22 | 外部上拉，按下为低 |
| TTP223 触摸 1 | `BOARD_TOUCH_1_GPIO` | GPIO52 | 模块自带上/下拉，输出数字电平 |
| TTP223 触摸 2 | `BOARD_TOUCH_2_GPIO` | GPIO51 | 同上 |

`BOARD_TOUCH_ACTIVE_LOW`（当前 `0`，即触摸时输出高电平）是 TTP223 极性的唯一开关：
模块背面 A 焊盘短接后改成 `1` 即可，驱动只认这个宏。灯珠数（`BOARD_WS2812_*_LED_COUNT`）
同样在 board_pins.h 里改，不动驱动代码（写多了只是多发一段无效数据，无害；写少了末尾灯珠不亮）。

**这些脚只属于将来要打的新 PCB。** 开发板上并没有这三处接线；`board_pins.h` 已按
`BOARD_USE_NEW_PCB` 拆成两支，开发板支目前是与 PCB 相同的占位值 ——
**不能拿它当“开发板上这三组外设可用”的证据**。

### 驱动组件

| 组件 | 关键设计 |
| --- | --- |
| `mech_button` | 5 ms 轮询 + 40 ms 消抖（连续相同采样才提交状态）；900 ms 长按只报一次；未长按时抬起报单击。对外 API 为 `button_init()` / `button_is_pressed()` / `button_register_callback()`，**不直接驱动 UI 或音频** |
| `touch_key` | 同上的状态机，另加 350 ms 双击窗口（单击挂起、等窗口过期再补发，避免把双击误报成两次单击）。命名为 `TOUCH_KEY_1/2`，**不假设左右** —— GPIO52/51 与左右的对应关系未定，映射到 `APP_EVT_TOUCH_LEFT/RIGHT` 属于后续工作 |
| `ambient_led` | `espressif/led_strip` 3.0.3 的 RMT 后端（**不是 bit-bang**），两路独立控制；呼吸用 65 项查表（半余弦 + gamma 2.2）线性插值，避免线性变化看起来“不平滑”；20 ms 渲染任务，效果代码内不出现阻塞延时 |

`ambient_led` API：`ambient_led_init()` / `set_rgb_a()` / `set_rgb_b()` / `set_rgb(id, …)` /
`set_brightness()` / `set_effect()` / `all_off()` / `count()`。

**RMT 显式 `with_dma = false`**：本机内部 DMA 池只有 146 KiB 且已被视频/语音挤到碎片化
（见第 10 节第 1 条与附录 A.9），WS2812 刷新率极低，绝不能让它再去抢 DMA。

### 档位 9 与自检入口

`main/test_profile.h` 新增 `CAMERA_TEST_PERIPH_ONLY 9`（`TP_BITS = TP_UVC | TP_PERIPH`）：
保留 UVC 取流，关闭 WiFi / WebSocket / UI / SD / 音频 / 天气，用于隔离地跑外设自检。
`main/main.c` 在 `#if TP_HAS(PERIPH)` 处调用 `peripheral_test_start()`。

`peripheral_test.c` 做两件事：

1. 17 步灯效序列循环打印（A 红/绿/蓝 → B 红/绿/蓝 → A+B 白/红 → 亮度 50%/12% →
   呼吸 → 流水 → 闪烁 → 全灭），按日志即可核对时序；
2. **开机静默期自检**：注册输入回调累加事件计数，3000 ms 后打印
   “各输入源事件计数 + 当前电平态 + 结论”。窗口长度 3000 ms **必须大于长按阈值 900 ms**，
   否则会把幽灵事件漏在窗口外 —— 这正是前两次人工看日志失败的原因（串口输出都被截断在判定点之前）。

**档位 0（完整系统）刻意不含 `TP_PERIPH`**，避免在 DMA 池紧张时再加三个任务与两个 RMT 通道。
`CAMERA_TEST_PROFILE` 当前设为 9 用于自检；要回到完整系统，改回 `CAMERA_TEST_FULL`。

### 开发板实测结论（2026-09-18，档位 9）

已验证：

- 三组驱动全部初始化成功；两路 `led_strip_new_rmt_device` 成功，长时间运行**没有出现
  “刷新灯带失败”告警**（驱动对 `led_strip_refresh()` 失败有一次性告警）—— RMT 通道、
  编码器、数据输出全程正常；
- 17 步序列一个循环实测 1259 ms → 41266 ms = 40007 ms，与设计 40000 ms 差 7 ms（0.018%）；
- **UVC 无回归**：complete 29.8～30.2 FPS、drop 0.0%、handoff rejected 0。新增的三个轮询
  任务挂在 CPU0，没有干扰 USB 摄像头通路；
- 开机静默期自检：按键 / touch1 / touch2 事件计数均为 0，结论“通过（无幽灵事件）”。

**未验证**：灯是否真的亮、颜色与灯珠方向是否正确、按键与 TTP223 的真实响应 ——
这些都需要实物。

### 开发板上已确认的硬件事实（写下来免得重复排查）

| 脚 | 状态 | 含义 |
| --- | --- | --- |
| GPIO22 | 接 **RST 复位键** | 按下直接复位芯片，**不能**当普通按键输入验证；驱动只做输入 + 上拉，不会误触发 |
| GPIO52 | 上电读高 | 驱动内部下拉已使能（`BOARD_TOUCH_ACTIVE_LOW=0`）却仍读到高 → **板上某外部源强于内部下拉把它拉高**。厂商资料包全树搜 51/52 零命中，需查原理图或断电量对 3V3/GND 电阻 |
| GPIO51 | 上电读低（空闲） | 开发板上**唯一**能做真实输入链路验证的脚 |

**唯一可做的无器件实测**：杜邦线把 **GPIO51 短到 3V3**，应打出 `TOUCH: touch2 pressed`，
松开发 `released` + `click`，按住 900 ms 发 `long press`。这能真实走完
“电平→去抖→事件→回调”整条链路，只是验证的仍是 PCB 版驱动的代码路径。

### 一个已修掉的驱动缺陷：幽灵长按

早期日志里出现过**只有 `long press`、没有前置 `pressed`** 的事件。根因是轮询式输入的
计时基准：`press_started_us` 留静态 0（= `esp_timer_get_time()` 的开机原点），而
`stable_pressed` 在 init 时被直接置为上电读数，于是一个上电就处于激活态的脚在第一次采样时
立刻满足“已按住 900 ms”。

修法不是把 0 换成一个当前时刻（那只是把幽灵推迟一个阈值），而是新增
**“上电即激活 ⇒ 该段按住整段作废”** 的语义（`boot_hold`）：既不补发 `pressed`，
抬起时也不发 `released`，否则会冒出“没有 pressed 的 released”同样带乱下游状态机。
另外两个驱动的 init 都会打印**上电初值电平**并翻译成“按下/松开”“触摸态/空闲”，
这是没有实物时唯一能判断接线的手段。

### 与 GPIO20 的关系（旧文档矛盾已结案）

`PROJECT_HANDOFF.md` §6 与 `src/demo/README.md` 的**旧版**接线表曾写屏幕 SCLK=GPIO20，
与 `board_pins.h` 的 `BOARD_LCD_SCLK=GPIO3` 矛盾。真相是屏幕为**外接** ST7789
（GMT020-02-8P），接在 1~6/47 上、**与 GPIO20 无关**；两份文档已按代码更正。
→ **WS2812_A 用 GPIO20 不存在冲突。**

### 尚未做（本阶段刻意不碰）

- 把按键 / 触摸事件接入 App Event Bus（`APP_EVT_TOUCH_LEFT/RIGHT/RIGHT_DOUBLE` 等）；
- 把 WS2812 绑定到业务状态（HOME 慢呼吸 / LISTEN 蓝呼吸 / THINK / REPLY / SLEEP …）；
- 确定触摸 GPIO52/51 与“左/右”的对应关系。

## 7. 摄像头当前验证结论

### 2026-09-17 YUV 转换 ref / sample_even 独立对照

两轮日志使用相同测试链路和摄像头模式，但分别以不同转换算法独立运行；没有在同一帧上双跑或做 `memcmp`：

```text
640×480 MJPEG@30
UVC → JPEG 硬件解码 → CPU YUV422→YUV420
profile 8；H.264 / WiFi / WebSocket / UI / SD 关闭
DMA2D/PPA 返回 ESP_ERR_NOT_SUPPORTED，使用 CPU 转换
sample_even 约 102 秒；ref 约 69 秒
```

丢弃首个启动期部分统计窗口后，稳态结果如下。`min/max` 为各 5 秒窗口中记录到的最小/最大转换耗时范围；FPS 与 drop 为稳态窗口统计：

| 指标 | ref | sample_even | 对比 |
| --- | ---: | ---: | ---: |
| YUV 平均耗时 | 10.31 ms | 8.72 ms | sample_even 快约 15.4% |
| YUV 窗口 min 范围 | 10.08～10.13 ms | 8.50～8.54 ms | sample_even 更低 |
| YUV 窗口 max 范围 | 10.33～10.40 ms | 8.74～8.81 ms | sample_even 更低 |
| UVC complete | 平均 30.03 FPS，范围 29.8～30.2 | 平均 30.03 FPS，范围 29.8～30.2 | 基本相同 |
| UVC drop | 0% | 0% | 未观察到回归 |
| handoff rejected | 0 | 0 | 相同 |
| rate_limit | 平均 50.58 次/5 秒，范围 49～52 | 平均 50.68 次/5 秒，范围 49～52 | 相同；处理门控仍为 20 FPS |
| JPEG/YUV 处理帧率 | 约 19.96 FPS | 约 19.94 FPS | 接近 20 FPS 上限 |

回调耗时平均值约为 ref 0.313 ms、sample_even 0.287 ms，最大值分别约 0.46 ms、0.42 ms；这是回调执行耗时，不应与 `callback_gap_max` 混为一谈。

**结论边界：** sample_even 在隔离配置下比 ref 快约 15.4%，且两轮 UVC 均无丢帧；但 2026-09-17 完整链路分辨率对照使用的是 ref 路径，因此 sample_even 在 H.264/WebSocket 开启时的收益仍未知。若继续评估该算法，需在 640×480 完整链路下与 ref 做独立同条件对照；当前应先完成 640×480 长时间稳定性观察。

当前工作区 `src/demo/components/video_streamer/video_streamer.h` 把 `VIDEO_YUV_CONVERSION_TEST_MODE` 设为 `VIDEO_YUV_CONVERSION_MODE_REF`；重跑 sample_even 需显式改为 `VIDEO_YUV_CONVERSION_MODE_SAMPLE_EVEN`，并保持其余测试配置一致。

### 2026-09-16 HBVCAM 640×480/1280×720 固定冷启动诊断

这次测试使用新摄像头 HBVCAM（Windows 设备名 `HBVCAM CAMERA`，VID `058F`、PID
`3822`）。电脑端已确认它可以输出 `MJPEG 640×480@30`。测试固件当前默认在
`src/demo/main/test_profile.h` 中选择：

```c
#define CAMERA_UVC_COLD_TEST_MODE CAMERA_UVC_COLD_TEST_640X480
```

切换到 `CAMERA_UVC_COLD_TEST_1280X720` 后必须重新构建，并让开发板和摄像头重新上电/枚举。
纯 UVC 测试不会在同一次运行里从一个分辨率切换到另一个分辨率。当前测试仍保持
`alt=1`、`effective_MPS=3072`、`8×16 KB URB`、每个 URB 6 个 ISOC 包和 26 字节
Probe/Commit。

已采集的 640×480 日志显示：

```text
PTS change/same = 174/33818
SOI/EOI = 175/174
FID start = 175
EOF=1、ERR=0、EOI 存在
```

这些数据证明摄像头在持续产生 JPEG 数据，问题不是普通 SOI/EOI 缺失。异常表现为一帧开始后
`frame_len` 依次到达 `620、1208、1796、2136`，之后虽然仍收到非空 payload，长度不再增加。
对应 packet 序号中出现疑似空 ISOC 包（例如 `39970`）。

根因是旧的 `actual_num_bytes < sizeof(uvc_payload_header_t)` 分支把普通零长度包也标记为
`skip_current_frame=true`。该操作不会释放 frame buffer，却会阻止之后所有同 FID/PTS 数据追加，
最终造成“buffer 仍在、长度停住、EOF/EOI 也无法交帧”。1280×720 的数据更集中，空包大多落在帧
间，因此此前仍能观察到部分 EOF 完成帧。

当前修复和诊断：

- `actual_num_bytes==0` 只增加 `empty_packet`，若发生在活动帧内再增加
  `empty_inside_active_frame`，不修改 frame、FID 或 skip 状态。
- 仅含 UVC header 的包继续处理 FID/EOF/ERR，并统计 `header_only_inside_active_frame`。
- 增加 `frame_abort_by_empty`、`frame_abort_by_header_only`，用于确认空包是否意外令活动帧失活。
- 关键事件 ring 保留最近 128 条，事件输出增加 `active=before->after`，不在 ISOC 回调中逐包打印。
- 保留 PTS change/same、SOI/EOI、FID/EOF/EOI 完成原因、丢弃原因、当前/最大帧长和 buffer 容量统计。
- SET_INTERFACE/GET_INTERFACE 完成后开始记录到首个非空包、首个 SOI、首个完整帧的延迟。

本修复已在 `build` 构建通过，但尚未替代实机验证。下一次 640×480 冷启动应重点确认：

```text
frame_len 在空包后继续增长
empty_inside_active > 0 时 abort_by_empty = 0
header_only_inside_active 可有数值，但 abort_by_header_only = 0
finish EOF/FID/EOI 出现有效计数
UVC 本次启动成功
```

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

## 7A. 视频上传：按需开启（2026-09-18）

**改造前为什么会自动上传**：不是某处显式调用，而是两个**编译期默认值**叠加的结果 ——

| 位置 | 改前 | 改后 |
| --- | --- | --- |
| `components/video_streamer/video_streamer.c` | `static bool s_stream_enabled = true;`（上电门控就是开的） | `= false;`（默认 IDLE） |
| `main/camera_driver.c`（原 889 行） | `video_streamer_set_enabled(CAMERA_VIDEO_STREAM_ENABLED != 0)` —— 摄像头初始化完成即强制开 | **删除**该自动调用，改为只打一行"关闭，等待服务端 VIDEO_START" |
| `main/test_profile.h` | `CAMERA_VIDEO_STREAM_ENABLED 1` | 改名 `CAMERA_VIDEO_STREAM_AUTOSTART 0`，降级为纯调试开关 |

**新链路**

```text
开机 → video_streamer_init()（只建队列/任务/缓冲，state = IDLE）
     → WiFi → WebSocket → Hello → 小智音频      全部正常，但不上传视频
     → 服务端 VIDEO_START → 事件 → 控制任务 → start() → STREAMING
     → 服务端 VIDEO_STOP  → 事件 → 控制任务 → stop()  → IDLE
```

`video_streamer_init()` 只创建必要的状态/锁/队列/PSRAM 缓冲，**不等于开启上传**；开启只能走 `video_streamer_start()`，而它只由服务端命令（或调试开关）触发。

> **前置条件：Agent WebSocket 必须真的建起来。** `VIDEO_START` 只能从这条 WS 进来，而 WS
> 地址与 Token 来自 OTA 响应的 `websocket.url` / `websocket.token`。OTA 一失败就没有控制
> 通道，"按需开启"也就永远等不到命令 —— 2026-09-18 档位 0 的日志正是这种状态（同时
> `XIAOZHI_AUDIO: MIC packets=0`，因为小智音频也挂在这条 WS 上）。见 §8 末尾的 OTA 地址回归。

**状态机与 API**（`components/video_streamer/video_streamer.h`）

| 项 | 说明 |
| --- | --- |
| `video_state_t` | `VIDEO_STATE_IDLE / STARTING / STREAMING / STOPPING`，默认 `IDLE` |
| `video_streamer_start()` / `stop()` | 幂等执行体：已 `STREAMING` 再 start、已 `IDLE` 再 stop 都直接返回，不重复建任务或队列 |
| `video_streamer_is_active()` / `get_state()` / `state_name()` | 只读查询 |
| `video_streamer_request_start()` / `request_stop()` | **非阻塞投递**，专供 WebSocket / MCP 接收回调使用 |
| `video_streamer_set_enabled()` | 兼容旧调用，等价于 `start()` / `stop()` |

**事件解耦**：WebSocket 接收回调只解析 JSON 并把命令丢进 `s_ctrl_queue`（深度 4），由 `video_ctrl` 任务（核 0、优先级 5、栈 3072）串行执行启停；回调里不做任何清理动作，因此**不会阻塞 WS 接收任务**。`start()/stop()` 另有互斥锁包住「读状态 → 清队列 → 改状态」整段，保证并发下幂等仍成立。

**服务端协议**（两条都支持；字段最终以服务端实际定义为准）

```json
{"type": "video", "state": "start"}      // 云端写法，新增支持
{"type": "video", "state": "stop"}
{"type": "video_on"} / {"type": "video_off"}   // 旧 PC 调试命令，保留
```

识别成功即回 `{"type":"video_state","ok":true,"video_enabled":true|false}`（回的是**目标**状态，不是当前状态；实际动作在控制任务里完成）。

**停止时的清理顺序（不可颠倒）**
1. 门控置 0 —— 此后 `video_streamer_submit_jpeg_owned()` 直接拒收，摄像头新帧不再进入流水线；
2. 清空 MJPEG 输入队列（未解码帧，连同 `release_cb` 一起归还摄像头池）；
3. 清空 H.264 输出队列（已编码、尚未发送的帧）；
4. 置 `IDLE`。

**重新开启从新帧开始**：`start()` 会置 `s_force_idr`，编解码任务在下一帧编码前调用 `esp_h264_enc_force_idr()`。否则停止期间编码器参考帧已失效，恢复后继续发 P 帧，PC 端会花屏到下一个 GOP 边界（GOP=30，最长 1 秒）。

**WebSocket 断线**：`DISCONNECTED` / `CLOSED` 时若状态不是 `IDLE`，投递一次 STOP，日志 `VIDEO_CTRL: WebSocket disconnected -> VIDEO_STOP (no auto resume)`。**重连不自动恢复**，必须等服务端重新下发 `VIDEO_START`。

**VIDEO_STOP 不停 UVC**：只关「编码 + 上传」。UVC 采集与 WebSocket 控制通道都保持运行，摄像头仍可供拍照 / AI 识别等后续业务使用；停 UVC 会牵动 USB 重新枚举，代价大于收益，故不做。

**预期日志**

```text
VIDEO_CTRL: initial state = IDLE
VIDEO_STREAM: video upload disabled, waiting for remote command
CAMERA: 视频编码与上传：关闭，等待服务端 VIDEO_START（UVC 采集保持运行）
VIDEO_STREAM: WebSocket 已连接：wss://...
VIDEO_CTRL: WS connected, video state = IDLE (keep IDLE unless server sends VIDEO_START)
——— 收到开启 ———
VIDEO_CTRL: VIDEO_START received
VIDEO_CTRL: IDLE -> STARTING
VIDEO_STREAM: 视频上传流已开启
VIDEO_STREAM: H264 pipeline started
VIDEO_CTRL: STARTING -> STREAMING
——— 收到停止 ———
VIDEO_CTRL: VIDEO_STOP received
VIDEO_CTRL: STREAMING -> STOPPING
VIDEO_STREAM: 视频上传流已关闭
VIDEO_STREAM: queues flushed
VIDEO_CTRL: STOPPING -> IDLE
```

**调试开关**：`main/test_profile.h` 的 `CAMERA_VIDEO_STREAM_AUTOSTART` 设 1，会在摄像头初始化后直接 `start()`，用于手边没有服务端时单独看画面。**生产与正常联调必须保持 0。**

**注意**：App Event Bus 目前**并不存在**（只在 `mech_button.h` / `touch_key.h` 的注释里被列为将来计划），所以视频控制用的是组件内部的专用事件队列，没有引入 `APP_EVT_VIDEO_STREAM_START/STOP`。

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

若在 STA 连接日志之前发生 Hosted/SDIO 握手失败：**先按第 9 节「ESP-Hosted 必须是 SDIO」确认传输没被改成 SPI**，再确认板载 C6 从机固件。厂商提供的参考文件为：

```text
E:\Lummiss_Plant_Robot\开发板示例\JC1060P470C_I_W_Y\8-Burn operation\Burn files\JC-C6-slave_v2.3.2.bin
```

### OTA 地址必须是项目自己的服务端（2026-09-18 回归）

`components/ota_client/ota_client.c` 的 `OTA_URL` 曾在小智官方地址
`https://api.tenclass.net/xiaozhi/ota/` 上（工作区里的未提交改动；HEAD 版本是
`https://www.lummiss.com/lummiss/ota/`）。官方地址对这台设备只返回 `server_time` 和
`firmware`，**没有 `websocket` 段**，于是出现：

```text
E OTA: OTA 响应中缺少 websocket.url
E VIDEO_STREAM: 缺少 OTA WSS 地址、Token 或设备身份，视频不建立 WebSocket
```

后果是 Agent WebSocket 压根不建立 —— 小智音频一直 `MIC packets=0`，服务端的
`VIDEO_START` 也无从下发（见 §7A 的前置条件）。2026-09-18 已还原为项目自己的地址。

判断只看这一行，`websocket` 必须是「存在」：

```text
OTA: OTA 响应结构：server_time=存在 firmware=存在 websocket=存在 activation=缺少 error=缺少
```

### 临时切到小智官方服务器：编译开关 `XIAOZHI_USE_OFFICIAL_SERVER`（2026-09-18 新增）

公司 Lummiss 后端维修期间，需要借官方服务器验证「小智回复的情绪 → 屏幕表情」
这条链路。为此加了编译开关，**原有 Lummiss 代码一行没删**，两套逻辑靠宏隔离。

开关定义在 `src/demo/main/Kconfig.projbuild`（默认 `n`）：

```text
menu "小智官方服务器测试模式"
    config XIAOZHI_USE_OFFICIAL_SERVER    bool, default n
endmenu
```

打开后各处行为：

| 位置 | 变化 |
| --- | --- |
| `components/ota_client/ota_client.c` | `OTA_URL` → `https://api.tenclass.net/xiaozhi/ota/` |
| `components/video_streamer/video_streamer.c` | 握手补 `Protocol-Version: 1`；Hello 换成官方 v1 格式（`audio_params`，去掉 `capability_manifest`）；`video_streamer_start()` 直接拒绝启动，发送侧另有编译期拦截 —— **官方 WS 的二进制帧只装 Opus，混发 H.264 会让服务端按音频解码** |
| `components/xiaozhi_audio/xiaozhi_audio.c` | `type=="llm"` 时打 `XIAOZHI: LLM emotion=… text=…`；缺 `emotion` 字段回退 `neutral` |
| `main/expression_manager.c` | 按官方词表查表，打 `EXPRESSION: emotion happy -> EXP_HAPPY` |

**WebSocket 地址仍然只从 OTA 响应的 `websocket.url` / `websocket.token` 读**，
代码里没有硬编码 WS 地址。音频上行不受开关影响（`video_streamer` 里音频入队只
看 `s_ws_connected`，不看视频门控），所以关掉视频不影响说话。

#### ⚠️ 只换 OTA 地址拿不到 Token，必须先激活

官方服务器对**未激活**设备只回 `server_time` / `firmware` / `activation`，
**没有 `websocket` 段** —— 这正是本节开头记的那次回归。打开开关后若看到：

```text
W OTA: 设备未激活。激活码：XXXXXX，提示：…
E OTA: 官方服务器未下发 websocket.url（设备未激活）
E VIDEO_STREAM: 缺少 OTA WSS 地址、Token 或设备身份，视频不建立 WebSocket
```

这不是代码问题，是设备还没绑定：去官方控制台用激活码把设备加上，再重启复检。

为此把 `ota_client.c` 里 activation 段的解析**提到了 websocket 校验之前**：原来
「缺 websocket.url 就 return」会把激活码这条最关键的信息吞掉，只剩一句没有指向性
的错误。

若响应里连 `activation` 段都没有，说明官方侧不认这台设备。官方激活还带
`challenge` + HMAC-SHA256（密钥取 eFuse `USER_DATA` 里的序列号，见参考实现
`xiaozhi-esp32-main/main/ota.cc` 的 `Ota::Activate()`），本工程**没有实现**
`/ota/activate` 这一步；需要的话再补。

#### 表情素材对应关系（2026-09-18 逐帧比对确认）

`src/demo/tools/gif/exp_0N.bin` 与源图 `项目文档/GIF/*.gif` 的对应关系。注意
exp_01/03/04/06/07 五张**帧数、帧延时、首帧全都一样**，按文件属性区分不开，
只能解出全帧平均画面来比：

| 素材 | 画面 | 帧数 | 映射到的官方 emotion |
| --- | --- | --- | --- |
| `exp_01.bin` | 乐（张嘴大笑） | 12 | `laughing` / `funny` / `silly` |
| `exp_02.bin` | 哀（垂眼撇嘴） | 22 | `sad` / `crying` |
| `exp_03.bin` | 喜（眯眼吐舌） | 12 | `happy` / `loving` / `delicious` |
| `exp_04.bin` | 怒（斜眉瞪眼） | 12 | `angry` |
| `exp_05.bin` | 思考（眼珠游移） | 8 | `thinking` |
| `exp_06.bin` | 惊讶（圆眼小嘴） | 12 | `surprised` / `shocked` |
| `exp_07.bin` | 疑惑（斜视撇嘴） | 12 | `confused` / `embarrassed` |
| `exp_08.bin` | 眨眼（中性） | 14 | `neutral` 及所有未识别词的兜底 |

映射表在 `expression_manager.c` 的 `k_emotions[]`，**词表外的取值统一回退
`neutral`（exp_08）**。切屏仍然发生在 LVGL 定时器里，WebSocket 回调只往队列投词，
不碰 LVGL。

### 两个踩过的构建坑（2026-09-18）

1. **`main/Kconfig.projbuild` 新加后会「看不见」。** ESP-IDF 在 CMake configure
   阶段用 `file(GLOB)` 收集各组件的 Kconfig，新建文件后不重跑 configure 就不会被
   收进去，`sdkconfig` 里也不会出现这一项。新增/改名 Kconfig 后必须：

   ```powershell
   idf.py -B build reconfigure
   ```

   验证办法：看 `build/kconfigs_projbuild.in` 里有没有你的文件。

2. **本工程实际生效的配置文件是 `sdkconfig.bletest`，不是 `sdkconfig`。**
   `SDKCONFIG` 缓存在 `build/CMakeCache.txt` 里：

   ```text
   SDKCONFIG:UNINITIALIZED=e:\Lummiss_Plant_Robot\src\demo/sdkconfig.bletest
   ```

   改 `sdkconfig` 完全不生效（`sdkconfig` 也在 `.gitignore` 里）。判断标准是构建
   开头那行：

   ```text
   -- Project sdkconfig file E:/Lummiss_Plant_Robot/src/demo/sdkconfig.bletest
   ```

   所以开关要写进 `sdkconfig.bletest`；注意这个文件是**被 git 跟踪**的。

## 8A. 云端通信迁移：三通道协议（MQTT 控制 + UDP Opus + WebRTC）

> 2026-09-18 起实施。权威文档：《ESP32-嵌入式三通道接入实施文档》v1.0（后端下发，
> 微信文件 `ESP32-嵌入式三通道接入实施文档.md`），以及参考工程
> `xiaozhi-esp32-main/docs/mqtt-udp.md` + 上游 `78/xiaozhi-esp32`
> `main/protocols/mqtt_protocol.cc`（协议实现逐行对照的依据）。

### 8A.1 架构与状态

后端已从「OTA + 单条 Agent WebSocket（JSON/MCP/Opus/H.264 混跑）」迁到三通道：

| 通道 | 承载 | 实机状态 |
| --- | --- | --- |
| HTTPS OTA | 配置、MQTT 六字段、固件 | ✅ 验证通过 |
| MQTT/TCP 1883 | AI hello、MCP、控制、状态、RTC 信令（**不传音视频帧**） | ✅ 验证通过 |
| UDP 8884 + AES-128-CTR | AI 对话 Opus 双向 | ✅ 验证通过（喇叭出声） |
| WebRTC/DTLS-SRTP | H.264 视频 | ❌ 阶段四未实现 |

固件协议选择：`main/Kconfig.projbuild` 的 `choice CLOUD_PROTOCOL` ——
`CONFIG_CLOUD_PROTOCOL_V3`（默认）/ `CONFIG_CLOUD_PROTOCOL_LEGACY_WS`（仅回滚）。
V3 下**旧 Agent WebSocket 完全不建立**（不注入 WS 地址，video_streamer 按设计保持 IDLE）；
LEGACY_WS 保留旧协议代码仅作回滚，两套不允许同时运行。

启动状态机（日志按此顺序打印 `STATE -> …`）：
`BOOT → NTP_TIME_READY → OTA_CONFIGURED → MQTT_CONNECTED → MQTT_SUBSCRIBED →
AI_HELLO_SENT → AI_SESSION_READY → IDLE`。

**2026-09-18 实机验收已通过**：双向音频 `tx=258(错0) rx=213(错0)`、`SPK frames=213`；
多轮连续对话（唤醒 → STT → LLM → TTS → 自动恢复监听）；MCP 握手 + tools/list +
tools/call（音量控制实测可用）；LVGL 不再因内存不足重启。

### 8A.2 组件与文件

| 文件 | 职责 |
| --- | --- |
| `components/cloud_mqtt/` | MQTT 客户端（封装 esp-mqtt，收 4 KiB/发 1 KiB 缓冲，keepalive 120 s）；MCP 层 `cloud_mcp.c`（队列 + mcp_task，工具注册表） |
| `components/cloud_udp/` | UDP 音频通道：16 字节头 + AES-128-CTR 加解密、收发任务（栈 8192，PSRAM）、下行回调注入 xiaozhi_audio |
| `components/network/time_service.c/.h` | **全工程唯一 SNTP 实例**，粘性事件位广播同步状态 |
| `main/Kconfig.projbuild` | `CLOUD_PROTOCOL` choice + 小智官方测试开关 |
| `main/main.c` | 注册 MCP 工具（音量等）、NTP 前移到 OTA 之前 |

### 8A.3 协议关键事实（踩坑记录，改代码前先读）

1. **UDP 包头 = 服务端下发的 16 字节 nonce 作模板**，设备只覆写 2-3（payload_len）、
   8-11（timestamp）、12-15（sequence）三个字段；nonce 的 0-1 即 type/flags、
   **4-7 即 ssrc(connection_id)**——文档说"由网关分配"，实际上它就藏在 nonce 里，
   设备原样回显，**不自行编造**。手搓包头写 `connection_id=0` 会导致
   「上行正常（STT 能识别）但下行零包」——2026-09-18 实机定位并以 nonce 模板修复。
2. **首包序列号必须为 1**（上游 `++local_sequence_`）；服务端接收侧拒绝
   `sequence <= 期望值`（初值 0）的包，首包发 0 会被当重放丢弃。
3. AES-128-CTR 的 **IV 就是本包 16 字节头本身**（加密与解密同一构造），
   不是 nonce 原文；key 由 32 个 hex 字符解码为 16 字节。每次重新 hello 换新 key。
4. **MQTT 回调只做解析与入队**（文档 §6.2 禁止回调里控电机/发 HTTP/建 PeerConnection）：
   MCP 消息由 `cloud_mcp_submit()` 拷入队列，工具在 `cloud_mcp_task`（PSRAM 栈）执行。
5. **SNTP 全工程只能初始化一次**：`esp_netif_sntp_sync_wait()` 内部是二值信号量，
   只能被消费一次，第二个调用方必超时——统一走 `time_service_*`（粘性事件位），
   home_info 只等不初始化。
6. **内部 RAM 纪律**：esp-mqtt 收发**各**一份缓冲（曾 16 KiB×2 把 LVGL 显示缓冲
   挤到分配失败 abort）。当前收 4096/发 1024；**阶段四 WebRTC 前必须把收缓冲调回
   ≥16 KiB**（SDP 最大 64 KiB，文档 §2.3）。mcp/udp 任务栈在 PSRAM（WithCaps），
   **WithCaps 建的任务必须 `vTaskDeleteWithCaps` 删除**。
7. `cloud_udp_rx` 任务栈 8192（PSRAM）：4096 在第一个下行音频包上栈保护越界
   （本地缓冲 ~1 KB + mbedtls + 回调链），已加大，**别再改小**。
8. 工具采用注册表（`cloud_mcp_register_tool`）：`tools/list` 与 `tools/call`
   都按注册表生成；**注册必须早于 `cloud_mqtt_start()`**（服务端握手后立刻拉
   tools/list）。未注册的名字回 -32602，不假装受理。

### 8A.3A MCP 工具清单（tools/list 实际内容）

| 工具 | 来源 | 说明 |
| --- | --- | --- |
| `self.get_device_status` | 官方 AddCommonTools | 返回 `{"audio_speaker":{"volume":N}}`；本设备无电池/背光可调，官方其余字段省略不编造；官方约定 LLM 调控制类工具前先查这里 |
| `self.audio_speaker.set_volume` | 官方 AddCommonTools | volume 0-100 → `xiaozhi_audio_set_volume()`；codec 未就绪时如实回 `NOT_AVAILABLE` |
| `motion.stop` / `motion.get_state` | Lummiss 自定义 | 开发板无 DRV8833，get_state **无绝对角度**（无编码器/限位），tools/list 描述里已写明 |
| `light.pulse` | Lummiss 自定义 | 档位 0 未初始化氛围灯时如实回 `NOT_AVAILABLE` |

未实现的能力（`media.webrtc.*`、`self.camera.take_photo`、`motion.rotate_to`）
**不注册** —— tools/list 只公布真实可调用的工具。新工具用
`cloud_mcp_register_tool()` 注册（处理函数放能力所属组件，勿硬编码进 cloud_mcp）。

### 8A.4 遗留 / 待办

1. **WebRTC（阶段四）**：H264 → WebRTC Track（Constrained Baseline、1280×720、
   20 fps、IDR ≤2 s）、RTC 信令 `rtc_signal`/`rtc_signal_ack`/`rtc_event`（1 秒 ACK
   超时、重发 2 次、`message_id` 幂等）、10 s HEARTBEAT、凭据走 HTTPS + 一次性
   `deviceTicket`（90 s）。视频启动来源改为 MQTT 的 `media.webrtc.start`（2 秒内回
   `accepted=true`，耗时初始化放独立任务）；视频默认 IDLE 的原则不变。
2. **阶段四前置**：把 MQTT 收缓冲调回 ≥16 KiB；向后端确认 ssrc/connection_id
   的分配与校验规则（本固件已按 nonce 模板回显）。
3. **表情素材仍未拷入 SD 卡**：`/sdcard/expressions/exp_01..08.bin`
   （源文件在 `src/demo/tools/gif/`）。emotion 触发已验证，只差素材文件。
4. 旧协议回滚：Kconfig 选 `LEGACY_WS` 重建即可；相关代码全部保留未删。

## 9. 构建方法

```powershell
Set-Location E:\Lummiss_Plant_Robot\src\demo
cmd /c _build_main.bat
idf.py -B build -p COM17 flash monitor
```

`_build_main.bat` 可在普通终端激活本机 ESP-IDF 5.5.5 并完成联合构建。由于本机安装器把 Python 约束文件放在特殊位置，脚本设置了 `IDF_PYTHON_CHECK_CONSTRAINTS=no`；实际 Python 依赖检查仍在激活阶段显示为 OK。

**注意：`_build_main.bat` 只做 `build`，不含烧录**（内部是 `idf.py -B build -DSDKCONFIG=sdkconfig.bletest build`）。只跑它不会更新芯片里的固件，必须另有一步 flash。

### ESP-Hosted 必须是 SDIO（2026-09-18 故障复查）

`sdkconfig.bletest` 里这几项必须是下表的值。只要传输被翻成 SPI，P4 就会去 GPIO6~12 上打 SPI —— **那六个脚正是板载 ES8311 音频的 I2C/I2S 引脚（7/8 I2C、9 DOUT、10 WS、11 PA_EN、12 BCLK）** —— 对 C6 既不复位也不通信。

| 项 | 必须值 |
| --- | --- |
| 传输 | `CONFIG_ESP_HOSTED_SDIO_HOST_INTERFACE=y`，且 `# CONFIG_ESP_HOSTED_SPI_HOST_INTERFACE is not set` |
| 从机目标 | `CONFIG_ESP_HOSTED_IDF_SLAVE_TARGET="esp32c6"`（`"invalid"` 同样是坏的） |
| 引脚 | CLK=18 / CMD=19 / D0=14 / D1=15 / D2=16 / D3=17 / RESET=54 |
| 总线与复位 | `SDIO_SLOT_1` + `SDIO_4_BIT_BUS` + `SDIO_CLOCK_FREQ_KHZ=40000`，`ESP_HOSTED_GPIO_SLAVE_RESET_SLAVE=54` |

**症状识别**：串口出现 `spi: Resetting slave on SPI bus with pin 12`，随后每约 10.3 s 重试一次、共三次，最后报：

```text
W (22974) transport: Failed to get ESP_Hosted slave transport up
func: esp_hosted_reconfigure   expression: transport_drv_reconfigure()
E (22994) WIFI_MANAGER: wifi_manager_init(141): 初始化 esp_wifi_remote 失败
```

正常时这里应当出现 SDIO 初始化与 C6 建链日志。修复即把上表几项改回正确值后重建；厂商包 `开发板示例/JC1060P470C_I_W_Y/1-Demo/Demo_IDF/ESP-IDF_5.5.3/xiaozhi-esp32-main/sdkconfig.old` 保留了这块板正确的 SDIO 段，可直接对照。

**自己动手改的步骤（2026-09-18 补）**

1. 先认清是哪份文件被改了。工程里同时存在三份名字相近的配置，只有第一份参与构建：

| 文件 | 角色 |
| --- | --- |
| `src/demo/sdkconfig.bletest` | **构建实际读取的**，要改就改它 |
| `src/demo/sdkconfig.defaults` | 首次生成配置的种子，其第 101–121 行就是正确的 SDIO 段，可当标准答案照抄 |
| `src/demo/sdkconfig` | 历史游离文件，只在构建命令没带 `-DSDKCONFIG` 时才被读到 |

2. 定位。VS Code 打开 `sdkconfig.bletest`，Ctrl+F 搜 `HOST_INTERFACE`；或命令行：

```bat
cd /d E:\Lummiss_Plant_Robot\src\demo
findstr /N "HOST_INTERFACE SLAVE_RESET_SLAVE PIN_CLK" sdkconfig.bletest
```

3. 改，而且要改三处，**只改第一处不算改完**：主段（约 2883 行）、从机复位脚（约 2946 行）、文件尾部那段"改名别名镜像"（约 3620 行）。最后一段最容易被漏，它同时带着 `CONFIG_ESP_SPI_HOST_INTERFACE` 和 `CONFIG_ESP_GPIO_SLAVE_RESET_SLAVE=12`，两处都会独立生效。

4. 自检。输出里只允许出现 SDIO：

```bat
findstr /C:"HOST_INTERFACE=y" sdkconfig.bletest
```

正确结果是恰好两行 `CONFIG_ESP_HOSTED_SDIO_HOST_INTERFACE=y` 与 `CONFIG_ESP_SDIO_HOST_INTERFACE=y`；出现任何 `SPI` 都说明没改干净。

5. 重编，然后单独烧录（`_build_main.bat` 不含 flash）。不必 `fullclean`。

**更省事的改法**：别手改文本，用 menuconfig：

```bat
cd /d E:\Lummiss_Plant_Robot\src\demo
idf.py -B build -DSDKCONFIG=sdkconfig.bletest menuconfig
```

路径是 `Component config → ESP-Hosted → Host interface`，选 SDIO，再进 SDIO 段确认 Slot 1 / 4-bit / 40 MHz / CLK18 CMD19 D0-D3 14-17 / RESET 54。**这条命令必须带 `-DSDKCONFIG=sdkconfig.bletest`**：漏掉它，menuconfig 打开并写回的就是另一份 `sdkconfig`，界面里看着改对了，实际构建配置没动，下次照样挂。


**这条复发路径已经修复**：`_build_main.bat` 已固定传入 `-DSDKCONFIG=sdkconfig.bletest`，因此删除 `build/` 后也不会静默切换到历史 `sdkconfig`。工程入口同时固定 `ESP_IDF_VERSION=5.5`，确保 `esp_wifi_remote` 加载 ESP-IDF 5.5 的 C6 从机选择配置；否则 Kconfig 会把 SDIO 选择回退成 SPI。

**2026-09-19 SDIO 构建复核**：重新配置后 `build/config/sdkconfig.h` 已确认只启用 `CONFIG_ESP_HOSTED_SDIO_HOST_INTERFACE`，从机为 `esp32c6`，SDIO Slot 1、4-bit、40 MHz、CLK/CMD=18/19、D0-D3=14/15/16/17、RESET=54。完整 Ninja 构建已通过并生成 `build/lummiss_main.bin`；构建期间仅有 ESP-ROM GDB 初始化目录未设置的非致命警告。

**还有一条更容易被忽略的复发路径**：`sdkconfig.bletest` 受 git 跟踪，而 `8c3901c` 这个坏提交至今仍是 HEAD。修复若只停在工作区、不提交，一旦 `git reset`、换机器或队友 clone，SPI 配置就会原样回来。修好后应尽快提交。

**2026-09-19 二次复发（含最小修复清单）**：本次只翻了主段（约 2912 行起），尾部镜像段（约 3648 行起）未被动过。最小修复就是三处，行号每次会漂，以 `findstr` 定位为准：

1. **传输选择**：
   - `CONFIG_ESP_HOSTED_SPI_HOST_INTERFACE=y` → `# CONFIG_ESP_HOSTED_SPI_HOST_INTERFACE is not set`
   - `# CONFIG_ESP_HOSTED_SDIO_HOST_INTERFACE is not set` → `CONFIG_ESP_HOSTED_SDIO_HOST_INTERFACE=y`
2. **在 SDIO 选择行之后补回 14 行参数块**（SPI 模式会把整块 SDIO 参数顶掉）：

```text
CONFIG_ESP_HOSTED_SDIO_RESET_ACTIVE_HIGH=y
CONFIG_ESP_HOSTED_SDIO_OPTIMIZATION_RX_STREAMING_MODE=y
CONFIG_ESP_HOSTED_SDIO_SLOT_1=y
CONFIG_ESP_HOSTED_SDIO_SLOT=1
CONFIG_ESP_HOSTED_SDIO_4_BIT_BUS=y
CONFIG_ESP_HOSTED_SDIO_BUS_WIDTH=4
CONFIG_ESP_HOSTED_SDIO_CLOCK_FREQ_KHZ=40000
CONFIG_ESP_HOSTED_SDIO_GPIO_RESET_SLAVE=54
CONFIG_ESP_HOSTED_SDIO_PIN_CMD=19
CONFIG_ESP_HOSTED_SDIO_PIN_CLK=18
CONFIG_ESP_HOSTED_SDIO_PIN_D0=14
CONFIG_ESP_HOSTED_SDIO_PIN_D1=15
CONFIG_ESP_HOSTED_SDIO_PIN_D2=16
CONFIG_ESP_HOSTED_SDIO_PIN_D3=17
```

3. **从机复位**：`CONFIG_ESP_HOSTED_GPIO_SLAVE_RESET_SLAVE=12` → `=54`（同时检查镜像段里不带 HOSTED 字样的 `CONFIG_ESP_GPIO_SLAVE_RESET_SLAVE`，本次它未被翻，保持 54）。

改完自检：主段与镜像段都必须是 SDIO（`findstr /C:"SDIO_HOST_INTERFACE=y"` 应至少两行，且不允许出现 `ESP_HOSTED_SPI_HOST_INTERFACE=y`）。

**遗留的误触发源排查**：本次复发发生在两次正常构建之间，期间无人使用 menuconfig。嫌疑最大的是 ESP-IDF 插件的"SDK Configuration Editor"（GUI）——打开即按当前 Kconfig 重新渲染并保存，会把未选中的 SPI 参数块展开顶掉 SDIO。下次复发时先 `git diff sdkconfig.bletest` 看是不是只有这一块变化，并留意是否在 GUI 里打开过配置页。

### 判断“烧进去的是不是新固件”

**不要看 `Compile time`**：app_desc 里那个时间戳只由定义应用描述符的那个 TU 的 `__DATE__/__TIME__` 决定，增量编译改业务代码时它不会重编，两次构建显示同一时间是正常的。

可靠证据有三个：

1. `idf_monitor` 启动时会把 build 目录 ELF 的 sha256 与设备打印的 `ELF file SHA256` 对比，不一致就打 `--- Warning: Checksum mismatch between flashed and built applications. Checksum of built application is <hex>`。**看到这条就等于“芯片里跑的不是刚构建的那个固件”**，此时分析日志没有意义。反之，三者一致（设备打印 = monitor 报的 built checksum = 本地 `Get-FileHash build/lummiss_main.elf -Algorithm SHA256`）就是烧录成功的硬证据。
2. 日志里新增 / 改写过的文案是否出现（例：外设自检那次新增的“3000 ms 后打印一次静默期自检结论”，旧固件没有这半句）。
3. `App version`（git describe）。

VS Code 当前默认使用 `build`、`sdkconfig.bletest` 和 Ninja。若任务仍出现 `build_main_verified` 或 `build_bletest7`，需要确认工作区打开的是 `src/demo` 并执行 `Developer: Reload Window`。

## 10. 现在最需要解决的问题

### 2026-09-18（三通道迁移后）状态更新

1. **语音链路已切到三通道并实机打通**：MQTT 控制 + UDP 双向 Opus，全双工对话
   实测通过（`tx=258 rx=213` 零错、`SPK frames=213`、多轮自动对话），详见第 8A 节。
   旧的「Agent WebSocket」在 V3 下完全不建立。
2. **原第 2 条（视频应离开语音 WS 通道）已由三通道迁移从协议上解决**：
   V3 下 H.264 视频不再走任何 WebSocket，规划路径是 WebRTC（阶段四）；
   视频/音频分通道正是新协议的设计。该条目的遗留工作并入阶段四。
3. **内部 DMA 池紧张有所缓解但仍需警惕**：MQTT 收发缓冲调整为 4 KiB/1 KiB 后，
   `[VIDEO] MEM DMA` 恢复到 43～51/23～24 KB（旧 WS 时代 63/24，V3 初版曾跌到
   3/1 KB 并触发过 LVGL 分配失败 abort）。第 1 条的池子分析在 V3 下依然成立，
   WebRTC 阶段要重新核算（MQTT 收缓冲要调大、WebRTC 自身还要吃内存）。
4. **新增待确认项**：UDP 的 ssrc/connection_id 已按上游协议改为回显服务端
   nonce 的 4-7 字节（实测 `nonce[0]=0x01 ssrc=0x6b67f749/0x5ca2b182` 等按会话
   变化）。若后端网关对 ssrc 有校验规则（例如必须与会话绑定），需与后端确认；
   判据是对话时 `UDP 统计 rx` 是否持续增长。

### 2026-09-15 新增（优先级高于下方各条）

1. **全系统共用的那块内部 DMA 池被挤碎，ESP-Hosted 收包路径取不到缓冲就 assert 重启
   ——这是当前第一优先级问题。**

   启动日志里 `esp_psram: Reserving pool of 146K of internal memory for DMA/internal
   allocations` 那一行就是它，大小 = `CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL`（当前
   150000）。所有需要内部 DMA 的东西——Hosted SDIO、USB URB、I2S、LCD/SPI、JPEG/H264
   驱动、LVGL 绘制缓冲、FatFS 的 bounce、音频 codec——都从**这同一块池子**取。
   **实测它整场只剩 3～13 KB 空闲、最大连续块 1 KB**，也就是
   `[VIDEO] MEM DMA=x/y KB` 里的 y。

   ESP-Hosted 的 SDIO 收包路径（`CONFIG_ESP_HOSTED_SDIO_OPTIMIZATION_RX_STREAMING_MODE=y`
   选中的 streaming 分支）要为**每个包**取一块 **1664 字节、64 字节对齐**的内部 DMA 缓冲，
   **取不到直接 `assert`，没有错误分支**：

   ```text
   assert failed: sdio_push_data_to_queue sdio_drv.c:862 (pkt_rxbuff)
   --- 0x480210b0: hosted_malloc_align at port_esp_hosted_host_os.c:132
   --- 0x480205c2: sdio_push_data_to_queue at sdio_drv.c:866
   --- 0x480206fa: sdio_data_to_rx_buf_task at sdio_drv.c:904
   ```

   实测 `[VIDEO] MEM DMA=10/1 KB INT=13/1 KB PSRAM=23174/23040 KB` 之后立刻崩在这一行
   （运行 64.8 秒处）。**注意 PSRAM 那边空着 23 MB——这是碎片，不是总量不够。**

   同一块池子被挤空时还会连带出这一串（都不必单独去查）：
   `allocate_dma_buf: not enough mem`（表情动画读不出来）、
   `transport: STA TX transport buffer unavailable, drop=1`（这一条会优雅丢弃）、
   TLS `PK verify failed 0x4290` + `HOME_INFO: HTTPS 请求失败`。

   **两个旋钮**（详细代码路径见附录 A.9）：

   - **把占用还回去**：每个长期占着的内部 DMA 缓冲都值钱。已删掉 `anim_bin_player.c`
     里那块 16 KB 的 `MALLOC_CAP_INTERNAL|MALLOC_CAP_DMA` 读缓存——它只是 `fread` 的目标
     地址，真正被 SDMMC 当 DMA 目标的是 FatFS 自己的窗口，删掉是纯赚，还少一趟 memcpy。
   - **放大池子**：`CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL` 150000 → 200000。给太多会
     **明确报错**（`app_startup.c:176-180` 打 `Could not reserve internal/DMA pool` 然后
     abort），不会静默降级，所以可以放心试、崩了往回调。**这是下一次烧录要验的第一件事。**

   本次已落盘的改动（删 16 KB 读缓存、下行队列 6→24、上传任务排空上行音频，见 A.9）
   **都还没实机复测**；`CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL` 那一条**只是建议，还没落盘**
   （`sdkconfig.defaults:40` 仍是 150000）。

2. **视频仍然应该在 WS 语义上离开语音通道（结构性，与第 1 条无关）。**
   后端文档 §6.1 规定"一个 Binary payload = 一个完整 Opus packet，禁止附加 WAV 头/Ogg/JSON/
   长度/序号/时间戳，禁止合并或拆分"，而我们把每帧十几 KB 的 H.264 也以二进制帧发在同一条
   连接上，**服务端无法从帧本身区分两类流**；另外视频的 4 Mbps 会给语音加队头阻塞。后端
   目前**没有**给出独立的视频上传端点（文档 §3/§4 只定义了 OTA 和这一条 Agent WS），所以
   要么向后端要一个视频专用端点，要么视频暂时回到本地/明文通道。
   **注意：这一条不再有"A/B 证明语音因此失效"作为支撑**——那次 A/B 已被推翻。它是按协议
   约束和带宽理由提出的，优先级排在下面第 1 条之后。

3. **AES 的 DMA 描述符分配失败会拖垮 TLS，进而断掉整条语音+视频链路——它同时也是第 1 条
   那块池子的大户。**
   实机反复出现 `esp-aes: Failed to allocate memory for ...`，随后 TLS 写失败、WS 断连，
   再往后 JPEG_DEC 从 15 ms 涨到 662 ms、编码掉到 0 fps。**成因与已做的修改见附录 A.8。**
   `esp_aes_process_dma()` 对**每一次** AES 运算都要从 `MALLOC_CAP_DMA|MALLOC_CAP_INTERNAL`
   分配描述符数组和对齐缓冲，而且 GCM 路径分配失败是裸 `return -1`、**跳过 `cleanup:`
   泄漏**已分配的那几组——这正好解释了池子为什么单向恶化而不是偶发抖动。
   改动已落盘（`CONFIG_MBEDTLS_HARDWARE_AES=n`），**待实机复测**：判据是串口里 `esp-aes:`
   一行都不再出现。

2. **按需视频流需要在设备上走完四条路径。** 2026-09-18 已把「开机自动开视频」改成服务端命令驱动（第 7A 节），**构建通过但尚未实机验证**。烧录后按顺序确认：
   (a) 开机 → WiFi → WebSocket 全部就绪后**没有任何 H.264 上传**，判据是 `VIDEO_CTRL: WS connected, video state = IDLE`；
   (b) 服务端下发 `{"type":"video","state":"start"}`，日志走 `IDLE -> STARTING -> STREAMING`，画面首帧即可解（强制 IDR 生效）；
   (c) 下发 `stop` 回到 `IDLE`，再 `start` 时**不闪出关闭前的旧画面** —— 这是「清空两条队列 + 强制 IDR」的联合判据，单独看任何一个都证明不了；
   (d) 拔网/服务端断开时自动停，恢复连接后保持 `IDLE`，直到服务端重新下发 `start`。

   顺带说明：改造前是「WebSocket 一连上就上传」，所以下方第 2 条记的「断线恶化成 56 秒黑屏」要在新的按需模式下重新观察 —— 现在断线会立刻停视频，恢复后是否黑屏取决于服务端何时重新 `start`，而不是固件自己重连就恢复画面。

以下为此前记录，编号不变：

1. **640×480 完整链路需要长时间验收。** 2026-09-17 profile 5 短时统计显示 H.264/WebSocket 约 30 FPS、UVC drop 0%；1280×720 则约 14～16.7 FPS 且有帧损坏。先以 640×480 连续观察至少 10 分钟，再判断稳定性。profile 8 的 sample_even/ref A/B 不能直接推断完整链路的效果。
2. **WebSocket 断线会恶化成 56 秒黑屏。** 2026-09-12 日志里 `transport_poll_write(0)` 触发一次后，重连上去不到 1 秒又断，连续 5 次，累计 56 秒无画面。成因是 `VIDEO_WS_SEND_TIMEOUT_MS` 过短（原 100 ms），被 `esp_websocket_client` 判为致命错误直接 abort；同时重连间隔是 8000 ms，比它自己注释里写的 2 秒长 4 倍。已改为写超时 2000 ms（与 `network_timeout_ms` 拆成两个宏）、重连 2000 ms，**待实机复测**。详见附录 A.6。
3. **640×480 浏览器预览待长测验收。** 完整 profile 5 短时窗口里 `sent` 与 `encoded` 均约 30 FPS、`send_fail=0`；还需连续运行至少 10 分钟，观察断线、预览卡顿及 UVC/JPEG 错误。PC 预览覆盖宏当前关闭，重新测试 PC 预览时需显式开启 `VIDEO_STREAM_PC_PREVIEW_ENABLED` 并重新构建烧录。
4. **短期设备链路已通过，长期稳定性未测。** 热复位完整重建、JPEG 完整性门控、双任务编解码/上传和 CPU 分块转换均已实机工作；需连续运行至少 10 分钟，再逐步扩展到长时间测试。
5. **动态首页待真机结论。** 构建通过不能证明镜像修正、中文字体、IP 自动定位和两个 HTTPS API 在设备网络上均正常，需要烧录后的照片及 `HOME_INFO` 日志。
6. **阶段 0 尚未冻结。** 除屏幕、USB、P4-C6 SDIO 和新 PCB 的人机交互三组（按键 GPIO22 / 触摸 GPIO52、51 / 氛围灯 GPIO20、21）外，关键器件型号与 GPIO 未定，会阻塞阶段 4、5、9、11。
7. **当前只完成第一层模块化。** 屏幕和摄像头已提取为驱动文件，但仍位于主组件，尚未拆成独立 ESP-IDF 组件、服务层和业务层。
8. **没有页面和状态机。** 当前静态 HOME 不能代表 HOME→LISTEN→THINK→REPLY→恢复流程，也无法做优先级和异常覆盖。
9. **屏幕背光只有开/关，没有调光。** 现接线为 `BOARD_LCD_BL=GPIO47`，`display_driver.c` 初始化时拉高点亮，因此休眠时熄灭已可做；但要做亮度调节仍需改成 PWM（LEDC），并需确认实物 BL 是否真的接到 GPIO47（旧文档写的是 BL 直连 3V3）。
10. **按键 / TTP223 / WS2812 只到驱动层，缺实物验收。** 三组驱动已构建并实机运行（见第 6A 节），但灯是否真的亮、颜色与灯珠方向、按键与触摸的真实响应都未验证。这些引脚的物理接线只属于新 PCB，开发板上无法验证（GPIO22=RST 键、GPIO52 被板级拉高）。新 PCB 回来之前，可先用杜邦线短接 GPIO51→3V3 验证触摸的整条输入链路。

## 11. 按流程继续的顺序

### 2026-09-18（三通道迁移后）新增

1. **MCP 工具实测验收**：对话里说"把音量调到 30"/"声音太大了"，确认 LLM 走
   `self.get_device_status` → `self.audio_speaker.set_volume`，`CLOUD_MCP:
   tools/call … -> {"volume":30}` 且喇叭音量真的变化。表情素材拷入 SD 卡后
   一并验收 `light.pulse`（档位 0 会如实回 NOT_AVAILABLE，属预期——氛围灯
   只在档位 9 初始化）。
2. **表情素材拷入 SD 卡**（第 5 次提醒，两分钟的事）：`src/demo/tools/gif/
   exp_01..08.bin` → SD 卡 `/expressions/`。判据：唤醒后
   `ANIM_BIN` 不再报 BIN 不存在，屏幕出现表情动画。
3. **与后端确认两件事**（UDP 下行虽已打通，仍需对账）：
   ssrc/connection_id 的分配与校验规则（固件已按 nonce 模板回显，实测
   `nonce[0]=0x01 ssrc=0x…`）；TTS 音频是否始终从 `60.210.30.199:8884` 回发
   （换源端口会被端口受限型 NAT 拦掉）。
4. **阶段三收尾 + 阶段四（WebRTC）**：按文档 §5——凭据走 HTTPS + 一次性
   deviceTicket、MQTT RTC 信令（1 秒 ACK、message_id 幂等）、10 s HEARTBEAT、
   H264 → WebRTC Track。开工前把 MQTT 收缓冲调回 ≥16 KiB（SDP 最大 64 KiB）。
   视频默认 IDLE 原则不变：`media.webrtc.start` → 2 秒内 `accepted=true`。

### 2026-09-15 起

1. **先做第 10 节第 1 条的 `SPIRAM_MALLOC_RESERVE_INTERNAL` 实验**：150000 → 200000，
   只改这一行，别的都不动。判据三个：
   - 开机有没有打 `Could not reserve internal/DMA pool`——有就是给多了，往下调；
   - 第一个 5 秒统计行里 `[VIDEO] MEM DMA=x/**y** KB` 的最大连续块 y 有没有从 1 KB 涨上去；
   - 能不能活过 65 秒、不再见到 `sdio_drv.c:862` 那条 assert。

   **注意 `sdkconfig.defaults` 的改动只在 sdkconfig 重建时生效**，要么删掉 `sdkconfig`
   重新 `set-target esp32p4`，要么直接改 `sdkconfig`（`MBEDTLS_HARDWARE_AES` 那次就是
   两个文件都改了）。删 `sdkconfig` 时**不要**连 `sdkconfig.defaults` 一起删。
2. **同时复测 AES 改动**（附录 A.8）：烧录后看串口是否还有 `esp-aes:`，以及 WS 还会不会断。
   这一条与第 1 条同源（都是那块池子），一起看。
3. **看 TTS 卡顿有没有好转**：`SPK frames=` 那行新增的两个栏 `qovf=` / `derr=`。
   本次一并改了三处（下行队列 6→24、上传任务排空上行音频、`anim_bin_player` 让出 16 KB），
   判据是 `qovf` 归零或大幅下降；**若以 `derr` 为主，说明问题在 Opus 解码/codec 写入侧，
   加深队列没用**。用户报的"语音模糊不清，很卡顿"就应该落在这两个数上。
4. **视频通道搬迁（第 10 节第 2 条）排在上面之后**：向后端要一个独立的视频上传端点，或把
   视频暂时放回本地明文 `ws://`——两者都能同时解决协议混发和 4 Mbps 给语音加队头阻塞。
5. **补齐小智协议的缺口**：`abort` 命令的本地响应、鉴权失败后的清 Token 与重新 OTA
   （后端文档 §7 要求）、下行 24 kHz 与上行 16 kHz 的采样率映射复测。

以下为此前顺序，仍然有效：

1. **验证 YUV sample_even 在完整链路中的效果**：profile 8 的独立 A/B 显示其转换平均耗时由 10.31 ms 降至 8.72 ms（快约 15.4%），但这轮关闭了 H.264 和网络。恢复完整链路后，在相同摄像头模式下观察 UVC drop、H.264 FPS、WebSocket send FPS 及音频/LVGL情况；`callback_gap_max` 单独不能作为丢帧判据。P4 v1.3 的 DMA2D/PPA 仍不可用，当前维持 CPU 路径。
2. **复测 WebSocket 断线修复**（附录 A.6）：烧录后长时间跑，确认不再出现"连上不到 1 秒又断"的循环。判据是 `transport_ws: Error transport_poll_write(0)` 不再出现；若仍出现，说明 2000 ms 写超时还不够，下一步要给 socket 加 `SO_SNDTIMEO`（写超时只约束那次 select，不约束随后的 `send()`）。
3. **完整档位（`CAMERA_TEST_FULL=0`）现已跑通**：启动 `src/demo/_start_camera_server.bat`，打开 `http://127.0.0.1:8000/`，确认接收速率（`sent == encoded`，`send_fail=0`）和网页预览丢帧。当前瓶颈在设备侧 UVC 丢帧，不在上传。
4. 只有在 CPU 分块转换前后完成同条件对照后，才继续调整 tile 大小或短空隙；当前不能把 UVC 丢帧下降归因于该优化。传输层已于 2026-09-10 由 HTTP 长连接替换为 WebSocket，并建立了下行命令通道（`/cmd?cmd=ping|status`）。
5. 补齐阶段 0 的 BOM、完整 GPIO 表和音频、传感器、电机接口定义；P4-C6 SDIO 引脚已经按厂商示例确定。
6. 验收 HOME 页动态时间、日期、天气与镜像方向，再建立 LISTEN、THINK、REPLY、SLEEP、FAULT 页面骨架；电量在阶段 11 完成电池管理后接入。
7. 继续把 display、ui、expression 拆分为独立组件，提供 `expression_play()` 等统一接口。
8. 完成阶段 3：实现 App Event Bus、UI 状态机、优先级、超时和状态恢复；只有 UI Task 调用 LVGL。
9. ~~H.264 HTTP 实时链路通过后替换为 WebSocket~~ **已完成（2026-09-10）**：P4 侧走 `esp_websocket_client`（`ws://<PC>:8001/ws`），每帧前置 16 字节自描述头（分辨率/帧率随帧携带）；PC 侧 HTTP(8000) 保留预览页、`/h264` 回退入口与 `/cmd` 下行命令。编码队列和 Network Manager 接口未动。后续要做运行期改分辨率：需把 `VIDEO_STREAM_WIDTH/HEIGHT` 从编译期宏改为运行期变量，并同步 `camera_driver.c` 的格式耦合（兜底格式表、格式提升、帧门控、`preferred_mjpeg` 判定），帧头自描述已解除 PC 端对分辨率的感知需求。

以下为 2026-09-18 新增：

10. **新 PCB 回来后先验外设（阶段 4）**：用实物确认按键（GPIO22）、两路 TTP223（GPIO52/51）、两路 WS2812（GPIO20/21）的真实行为与极性（`BOARD_TOUCH_ACTIVE_LOW`、呼吸曲线、灯珠方向与数量）。**同时先把“GPIO52 在开发板上被板级硬件拉高”这件事查清**（查原理图，或断电量对 3V3/GND 电阻）——它会影响对新 PCB 接线的判断，也可能与 TTP223 自身的上下拉冲突。
11. **再把外设接进业务**：按键 / 触摸 → App Event Bus（`APP_EVT_*`）、WS2812 → UI / 业务状态（HOME 慢呼吸 / LISTEN 蓝呼吸 …）。这一步依赖阶段 3 的 Event Bus 与 UI 状态机，不要抢在前面做。
12. **按需视频流实机验收（对应第 10 节「2026-09-15 新增」第 2 条）**：不依赖新硬件，现在就能做。四条路径里重点验 (c) —— `stop` 之后再 `start` 不能闪出关闭前的旧画面。同时和后端把字段定死：当前 `{"type":"video","state":"..."}` 与旧的 `{"type":"video_on/off"}` 都兼容，确定一种后应删掉另一种，避免协议长期双轨。

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

### A.7 小智语音链路实测（2026-09-15）与视频共存 A/B（**A/B 结论已推翻，见本节末尾更正**）

链路：板载 ES8311 采集 → Opus 编码（16 kHz、单声道、60 ms/960 采样）→ `video_streamer`
的 Agent WSS 上行；下行二进制帧 → Opus 解码（24 kHz、单声道、60 ms）→ ES8311 播放。

**判读用的四个数**（这套判据是这一轮摸出来的，比看日志文本靠谱）：

| 数 | 健康值 | 说明 |
| --- | --- | --- |
| `MIC packets` | 每 5 秒 +84 | 60 ms 帧长 = 16.7 包/秒；**只在 `video_streamer_agent_send_audio()` 返回 `ESP_OK` 后自增**，所以它涨 = 确实进了视频上传任务的队列（**不等于已经发出去了**，真正发送在那边）；队列满走 `read_err=`（`s_capture_errors`） |
| 上行包长 | 约 106 字节 | ≈ 14 kbps |
| `mic peak` | 说话时 11676 | **环境底噪就是 284～544，不要以为这是增益不够去调 `XIAOZHI_CODEC_INPUT_GAIN_DB`（已经是 30 dB）**，必须真说话再看 |
| `SPK frames / drop` | `drop=0` 且 frames 在涨 | `frames=0 drop=0` = **服务端一个二进制帧都没发**，不是本地解码失败。`drop` 已于 2026-09-15 拆成 `qovf=`（猝发打满队列，丢最旧包，加深队列有效）和 `derr=`（Opus 解码/codec 写入失败，加深队列无效），见下方更正与 A.9 |

一次完整成功（摄像头断开，2026-09-15）：

```text
小智开始回答
小智回答文本：{"type":"tts","state":"sentence_start",...}
小智回答结束，恢复麦克风上行
SPK frames 0 → 62 → 85，drop=0
mic peak=11676
```

**A/B（同一次烧录、同一段代码，唯一差别是摄像头插着与否）——结论已推翻，见下**：

| 摄像头 | 服务端回答 | 下行 | 上行 |
| --- | --- | --- | --- |
| 开着 | "主人，lummiss现在有点忙" / "我们稍后再试吧"（兜底话术，问什么都一样） | `SPK frames=62`，随后 `send_fail=1`、TTS 结束后 WS 断 | 正常 |
| 断开 | "我一直都在呢，您请说。"（真实回答） | `SPK frames=46 drop=0` | 正常 |

**两次都没有 `stt` 消息。**（当时的推论是"这个配置下服务端不发 `stt`，判据只能用回答内容"
——**这条已被推翻**，见下方更正。）

**更正（2026-09-15 当日稍晚）：上面这张表的 A/B 结论不成立，"服务端不发 `stt`"也是错的。**

- **摄像头推流时语音是通的。** 另一次运行里摄像头开着、视频 13.9～15.6 fps 稳定上传
  （`send_fail=0`），同时上行 16.7 包/秒不断，服务端正常识别
  （`{"type":"stt","text":"好"}`）并回了完整回答，整场连续跑了 65 秒。所以"摄像头开着
  语音就失效、只回兜底话术"**不要再照它找根因**；那次 A/B 时上传路径还在抢占/中断，
  变量没控干净（那次也是 64.8 秒时那条 Hosted assert 崩掉的同一场，见 A.9）。
- **服务端这一版会发 `stt`**（实测 `{"type": "stt", "text": "好"}`），可以重新拿它当
  识别判据用。
- **它也会自己发起一轮**：用户全程没出声（`peak` 一直在 331～626 的底噪带里）时也会发
  `tts state=start`，内容形如"您请说，我正听着。"。所以"有回答"既不等于"听懂了"、
  也不等于"用户说过话"。设备侧只有 `listen mode:auto`，VAD 在服务端。
- 表里 `drop` 只有一栏，现已拆成 `qovf=` / `derr=`（见上方判读表和 A.9）。

**已知未解**：TTS 刚结束（`小智回答结束` 后 47 ms）WS 断过一次，报
`H.264 发送失败：259`（`ESP_ERR_INVALID_STATE`）+ `unexpected data readable on
socket=54` + `Connection terminated while waiting for clean TCP close`，2 秒后自动重连并
拿到新 `session_id`，`send_fail` 计 1。原因未定位。**但它是偶发的**：摄像头开着的那一场
连续跑 65 秒、`send_fail=0`，没有复现这条断连。

### A.8 AES 的 DMA 描述符分配失败：排查过程与结论（2026-09-15）

**症状链**：`esp-aes: Failed to allocate memory for ...` → `esp-tls-mbedtls:
mbedtls_ssl_handshake returned -0x0001` / TLS 写失败 → WS 断连 → 再往后 `JPEG_DEC` 从
15 ms 涨到 662 ms、编码掉到 0 fps。流量只有约 4 Mbps，硬件加速省下的那点 CPU 毫无意义。

**踩过的坑：先只关了 `CONFIG_MBEDTLS_HARDWARE_GCM`，无效。** 因为 AES 有**两个**描述符
消费者，GCM 那条只是其中之一：

| 消费者 | 位置 | 谁在用 |
| --- | --- | --- |
| `esp_aes_process_dma_gcm()` | `esp_aes_gcm.c:736` | 硬件 GCM |
| `esp_aes_process_dma()` | `port/aes/dma/esp_aes_dma_core.c:541` | **每个非 GCM 记录**：CBC / CTR / ECB，以及软件 GCM 走 CTR 时 |

关掉 GCM 只是把报错从 `len buffer`（GCM 那条，`:841`）换成 `start/end alignment buffer`
（通用那条，`:469` / `:491`）——错误换了个函数照样出，现象一点没变。

**为什么 AES 一定要去啃那块 146 KiB 的池子**：

- P4 上选哪份 `esp_aes.c` 由 `mbedtls/CMakeLists.txt:209-215` 的 `AES_PERIPHERAL_TYPE`
  决定，而它只看 SOC 能力 `CONFIG_SOC_AES_SUPPORT_DMA`——**没有 Kconfig 可以关掉 DMA 路径**；
- `esp_aes_process_dma()` 对**每一次** AES 运算都要调两遍 `generate_descriptor_list()`
  （`:616` / `:622`），**16 字节的 ECB 也一样，没有小缓冲区直通捷径**；
- 每次从 `MALLOC_CAP_DMA|MALLOC_CAP_INTERNAL` 里 `heap_caps_aligned_calloc` 描述符数组
  （`:357-359`）和最多两个对齐缓冲（`:467` / `:489`）——和 UVC URB、WiFi SDIO 抢同一块内存。

**这条路径失败即泄漏**：GCM 的 `len_desc`/`len_buf` 分配失败（`:833-845`）是裸
`return -1`，跳过了函数末尾的 `cleanup:`，而 `aad`/`input`/`output` 三组描述符和 6 个
alignment buffer 此时都已分配成功。失败一次池子就永久小一截，下一次更容易失败——正好解释
现象为什么是单向恶化而不是偶发抖动。

**最终修改**：`CONFIG_MBEDTLS_HARDWARE_AES=n`（`sdkconfig.defaults` 与 `sdkconfig` 都改）。
`port/include/mbedtls/esp_config.h:152-166` 显示 `MBEDTLS_AES_ALT` 与 `MBEDTLS_GCM_ALT`
**都由这一个开关控制**，关掉后 mbedtls 完全走自己的软件 `aes.c` / `gcm.c`，上面那条路径
一次都不会被调到。代价是纯软件 AES（GHASH 本来就是软件算的）：约 4 Mbps 的 TLS 流量，
400 MHz RISC-V 上软件 AES 约 10 MB/s，占不到一个核的 5%。

**已排除的旁支**（免得再查一遍）：

- **不是 SHA**：`esp_sha_gdma_impl.c` 里没有任何堆分配（静态描述符），日志里也从没出现过
  `esp-sha:` 的错误；
- **不是 flash 加密或 NVS 加密**：两者都 `is not set`；
- **工程里没有别处直接调 `esp_aes_*` / `mbedtls_aes_*`**。

**一条没解释清、但不要据此推翻结论的现象**：报错时刻的 `[VIDEO] MEM` 行显示
`DMA=65/22 KB INT=103/31 KB`，看着还有余量，而失败的是 ≤256 字节的小分配。MEM 行是 10 秒
周期的快照，很可能没落在失败那一瞬间（握手/重连时 UVC 与 WiFi 同时在抢池子）。`DMA_DESC_MEM_ALIGN_SIZE`
在 P4 上是 8（`GDMA_LL_AXI_DESC_ALIGNMENT`），所以也不是对齐粒度把池子切碎导致的。

**待复测判据**：串口里 `esp-aes:` 一行都不再出现。注意这一条**不是**视频/语音 A/B 的根因
——摄像头断开那一场也有 `esp-aes` 报错，语音却是好的。但它和 A.9 是同源问题：AES 每次运算
都从**同一块**内部 DMA 池取描述符和对齐缓冲，失败还会泄漏，是那块池子最主要的长期消耗者。

### A.9 内部 DMA 池被挤碎导致 ESP-Hosted assert 重启（2026-09-15）

**崩溃现场**（运行 64.8 秒处，摄像头开 + 视频推流 + 语音提问，MHARTID=1）：

```text
E (13764) allocate_dma_buf: not enough mem              ← 表情动画读不出
E (...)   transport: STA TX transport buffer unavailable, drop=1
-- 崩溃前最后一条 5 秒统计快照：
I (13864) [VIDEO] MEM DMA=10/1 KB INT=13/1 KB PSRAM=23174/23040 KB
E (13870) PK verify failed 0x4290 / HOME_INFO: HTTPS 请求失败
assert failed: sdio_push_data_to_queue sdio_drv.c:862 (pkt_rxbuff)
--- 0x480210b0: hosted_malloc_align at port_esp_hosted_host_os.c:132
--- 0x480205c2: sdio_push_data_to_queue at sdio_drv.c:866
--- 0x480206fa: sdio_data_to_rx_buf_task at sdio_drv.c:904
```

**这一串是同一个根因的连锁反应。** `MEM DMA=10/1` 读作"池子剩 10 KB 空闲、但**最大连续块
只有 1 KB**"，而 Hosted 每个包要的是 1664 字节**单块** → 取不到。PSRAM 那边还空着 23 MB，
所以不是总量问题，是这块 146 KB 的池子被切碎了。`PK verify failed 0x4290` 同理是**内存**
原因而不是时钟：它出现在 13870，紧跟 13764 那句 `allocate_dma_buf: not enough mem`，
时间上就在崩溃前约 100 ms。

**逐包分配的代码路径**（免得再翻一遍）：
`sdio_push_data_to_queue()`（`sdio_drv.c:844`，streaming 分支从 `:810` 的 `#else` 起）
→ `sdio_buffer_alloc` → `mempool_alloc(buf_mp_g, MAX_SDIO_BUFFER_SIZE, MEMSET_REQUIRED)`
→ `hosted_malloc_align` = `heap_caps_aligned_alloc(align, size, INTERNAL|DMA|8BIT)`
（`port_esp_hosted_host_os.c:131`）→ **`assert(pkt_rxbuff)`，没有错误分支**。
mempool 是空表起步、按需长出来的（`mempool.c:19` / `:104`），所以稳态下每个在途包都占一块。
寄存器转储里的 size=0x680(1664)、align=0x40(64) 就是这个请求。

那条 assert **只在 streaming 分支里**；非 streaming 分支（`sdio_drv.c:779` 起）是原地把整块
transfer buffer 排队、没有这条断言。当前由
`CONFIG_ESP_HOSTED_SDIO_OPTIMIZATION_RX_STREAMING_MODE=y` 选中——**如果调池子仍不稳，
这是另一个可以拿来换稳定性的开关**（代价是每包一次拷贝）。

**本次已做的改动（均未实机复测）：**

| 改动 | 位置 | 理由 |
| --- | --- | --- |
| 动画改用固定 4 KB 内部 DMA 读缓存 | `components/anim_bin_player/anim_bin_player.c` | FatFS 大块读取会把调用方 Buffer 直接交给 SDMMC；LUM1 帧偏移通常不满足 PSRAM 128 字节对齐，直接读整帧会申请大块 bounce buffer。4 KB 分块兼顾稳定性与内部内存 |
| SD 同时打开文件数 8 → 2 | `components/sd_card/sd_card.c` | 每个 FatFS 文件缓存占 4 KB，当前轮播只需一个动画文件，保留一个余量即可 |
| 下行播放队列 6 → 24 | `xiaozhi_audio.c:51` | 24 × 60 ms = 1.44 s，装得下一次猝发；队列存储在 PSRAM，只占约 34 KB |
| `drop` 拆成 `qovf=` / `derr=` | `xiaozhi_audio.c` | 前者是猝发打满（加深队列有效），后者是解码/codec 写入失败（加深无效）——不拆开就分不清该调哪个 |
| 移除 TTS 首播预缓存 | `xiaozhi_audio.c` | 实测 24 包队列无溢出，预缓存没有解决播放变慢，恢复 Git 基线的收到即播 |
| MIC/SPK 恢复 CPU0，优先级 6 | `xiaozhi_audio.c` | Git 基线在该配置下播放流畅；CPU1 已持续承担 JPEG/YUV/H.264，不再把 I2S 初始化和播放迁到 CPU1 |
| 开启 WebSocket 独立 TX 锁 | `sdkconfig.defaults` / `sdkconfig` | 新日志中 H.264 `max_SEND=1407/1934 ms`，同时 TTS 队列从空转为突发并出现 `qovf=4`；独立 TX 锁允许客户端在视频发送阻塞时继续接收下行 Opus |
| 上传任务每轮最多排空 8 个上行音频包 | `video_streamer.c` `VIDEO_UPLOAD_AUDIO_DRAIN_MAX` | 免得一帧视频发完才轮到音频 |

`CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL` 已由 150000 调到 200000。
机制是 `esp_psram_extram_reserve_dma_pool()`（`esp_psram.c:635`）在开机时从内部堆里挖出
连续块、重新注册成独立 heap region；给多了会**明确报错**（`app_startup.c:176-180`：
`Could not reserve internal/DMA pool (error 0x%x)` 然后 `abort()`），不会静默降级。

**TTS 卡顿的量化**（就是用户报的"语音模糊不清，很卡顿"）：同一次运行里
`SPK frames=28 drop=12`——一次回答 40 帧丢 12 帧，30%。成因是 `esp_websocket_client`
收发共用同一把 `client->lock`：上传任务每发一帧视频最长持锁
`VIDEO_WS_SEND_TIMEOUT_MS=2000 ms`，期间下行 Opus 只能堆在 TCP 接收缓冲里，锁一放就成串
回调进来；而播放队列原来只有 6 个槽（60 ms × 6 = 360 ms），一突发就丢最旧包。

**一个同类但独立的坑**：`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16384` 让 ≤16 KB 的
`malloc()` 优先走内部 RAM。当前通过限制 FatFS 文件数和将动画中转块缩到 4 KB 控制占用；
仍需观察日志中的 `MEM DMA=空闲/最大块`，最大块低于 2 KB 时 ESP-Hosted 仍可能分配失败。
