> 历史记录：当前方案以 [RTC_MEMORY_ARCHITECTURE.md](RTC_MEMORY_ARCHITECTURE.md) 为准；下文各轮Guard/640p/缓存上限描述不是当前配置。

# RTC 内存与 CPU 调度审计（2026-09-24）

本表覆盖主工程的长期/会话期分配，以及已定位的 ESP-Hosted、H.264、LCD SPI 底层分配。`malloc`/`calloc` 默认 caps 的具体落点取决于 IDF 堆策略，不能只凭 API 名认定为 DMA；第三方预编译 `esp_peer` 无源代码，其内部净占用由阶段日志测量。数值为申请量，不含堆元数据及对齐开销。

| 模块 | 位置/用途 | 大小 | 当前 caps、生命周期 | DMA 直接访问 / 必须 INTERNAL | 处置 |
|---|---|---:|---|---|---|
| ESP-Hosted SDIO mempool | `mempool_alloc/free`、SDIO 包 | 约 1664 B/块，按包增减 | INTERNAL+DMA，联网期 | 是 / 是 | 保留在内部；720p 实测余量不足后改为全局最多缓存 1 块，超出真实释放 |
| ESP-Hosted SDIO 收发/寄存器 | `sdio_drv.c` | 由事务长度决定 | INTERNAL+DMA，事务期 | 是 / 是 | 保留并监控峰值 |
| ESP-Hosted SDIO RX 软件包副本 | `sdio_push_data_to_queue` 和 `sdio_process_rx_task` | 每包约 84–1536 B，网络栈持有期 | **PSRAM**，逐包 | 否 / 否；原始 SDIO DMA 已完成，后续仅 CPU/协议栈访问 | 第三轮日志定位 DMA 紧张后迁移；TX 与原始双缓冲不变 |
| USB/UVC URB | `uvc_host_stream_open` | 18 KiB × 8 | PSRAM+DMA，流期 | 是 / 视 P4 USB DMA 能力 | 保留驱动的分配方式 |
| UVC 帧与 handoff | `camera_driver.c` | 512 KiB × 3 + 512 KiB × 3 | PSRAM，摄像头期 | 驱动帧是，handoff 否 / 否 | 保持当前 PSRAM |
| JPEG 输出 | `video_codec_task` | 720p YUV422 约 1.8 MiB | 驱动申请的对齐 PSRAM，codec 期 | JPEG 硬件写入 / 否 | 保留驱动分配器 |
| YUV420 输入和 H264 输出槽 | `video_codec_task` | 720p 约 1.35 MiB；131200 B × 4 | PSRAM，codec 期 | H264 控制器需要对齐/可访问；无需 INTERNAL | 保留 PSRAM |
| H264 reference | `video_encoder_create` | 720p 约 92224 B；640p 约 46144 B | INTERNAL 对齐，RTC 推流期 | 是 / 是 | 不强迁 PSRAM；低内存时优先使用 640p 配置 |
| H264 Guard | `webrtc_whip.c` | 96 KiB | INTERNAL，建链时临时 | 否；释放后让 reference 占原连续块 | 保留当前短生命周期，核对释放阶段日志 |
| LCD LVGL draw | `lvgl_port_add_disp` | 1920 B × 2 | INTERNAL+DMA，UI 期 | 是 / 是 | 保留；RTC 暂停刷新但保留最后一帧 |
| LCD SPI 私有 TX | IDF `spi_master.c:setup_dma_priv_buffer` | 本项目一次 draw 约 1920 B，按对齐增大 | INTERNAL+DMA，事务期 | 是 / 是 | 保留安全余量，不能容忍最大块为 0 |
| 小智采集用户缓冲 | `audio_work_buffers_init` | `encoder_input_size × codec_rate/uplink_rate`，约 2880 B | **PSRAM**，音频期 | 否 / 否；I2S 驱动从自身 DMA ring `memcpy` | 本轮从 INTERNAL+DMA 迁出 |
| 小智采集 PCM/Opus | `audio_work_buffers_init` | 分别为编码器输入/输出上限 | INTERNAL+8BIT，音频期 | 否 / 否；CPU/Opus 访问频繁 | 暂保留，性能数据后再判断 |
| 小智播放 PCM、业务队列 | `xiaozhi_audio.c` | PCM 池和队列视配置 | PSRAM，音频期 | 否 / 否 | 已在 PSRAM |
| 动画文件读缓冲 | `allocate_play_resources` | 4096 B | **PSRAM**，仅动画播放时 | 否 / 否；`fread` 填用户缓冲，SD 驱动自有 bounce | 本轮从 INTERNAL+DMA 迁出 |
| 动画帧索引 | `load_animation` | 最多 1024 × 12 = 12288 B | **PSRAM**，动画期 | 否 / 否 | 本轮从 INTERNAL 迁出 |
| 动画 RGB565 三缓冲 | `allocate_play_resources` | 150 KiB × 3 | PSRAM，动画期 | 否 / 否 | 已在 PSRAM；RTC 开始前停止动画 |
| video_codec/taskLVGL/anim_player 栈 | 各任务创建处 | 8192/7168/8192 B | PSRAM，任务期 | 否 / 否 | 已在 PSRAM |
| video_upload/video_report 栈 | `video_streamer_init` | 8192/4096 B | **PSRAM**，任务期 | 否 / 否 | 本轮从默认内部堆迁出 |
| ui_task/camera_task 栈 | `main.c` | 8192/8192 B | **PSRAM**，任务期 | 否 / 否；USB 控制任务栈不作为 URB 描述符 | 本轮从默认内部堆迁出 |
| WebRTC SDP、HTTP 凭据、MCP/UDP 业务队列 | `webrtc_whip.c`、`cloud_mcp.c`、`cloud_udp.c` | 按各消息/队列定义 | PSRAM，连接期或短暂 | 否 / 否 | 已在 PSRAM，不重复搬迁 |
| `esp_peer` | 预编译库，peer open 到 stop | 旧日志约 16–19 KiB 净变化 | 由库选择，RTC 期 | 未证实直接 DMA | 保留阶段 heap trace；不改库 |

本轮直接迁出的显式 DMA 申请：音频采集约 2880 B + 动画读取 4096 B（动画时才存在），合计约 6976 B；从默认 INTERNAL 迁出的普通任务栈 28672 B，动画索引最多 12288 B。节省的是**申请需求**，堆的实际 free/largest 增量还会受布局和分配顺序影响，必须看烧录后的阶段日志。

## CPU1/CPU0 调度表

| 任务 | Core / 优先级 / 栈 | 运行方式 | 已知单次耗时 / CPU 占用 |
|---|---|---|---|
| USB/UVC driver、`usb_events` | CPU0 / 20、19 / 驱动配置、4096 B INTERNAL | USB 事件等待、URB 回调 | 运行占用待 `TASK_LOAD_5S`；USB 优先级保留 |
| `camera_handoff` | CPU0 / 18 / 4096 B 默认堆 | 队列阻塞等待，转交帧 | 待测 |
| ESP-Hosted SDIO | Hosted 内部任务 / 高优先级 / 内部分配 | SDIO 队列/中断 | 待测，不迁核 |
| `xiaozhi_mic`、`xiaozhi_spk` | CPU0 / 6、10 / 40960、6144 B PSRAM | I2S 阻塞读取/播放队列 | 待测 |
| `video_upload` | CPU0 / V3 9 / 8192 B **PSRAM** | 输出队列阻塞等待、网络发送 | `TASK_LOAD_5S` 待测，发送耗时已有视频日志 |
| `webrtc_whip` | CPU0 / 4 / 12288 B PSRAM | 20ms 队列等待 + peer loop | 待测 |
| `video_codec` | CPU1 / 8 / 8192 B PSRAM | 输入队列最长等待 50ms；队列非空时每帧额外 `vTaskDelay(1)` | 旧日志 JPEG 约 11ms、YUV 约 30ms、H264 约 16ms；新占用待测 |
| `ui_task` | CPU1 / 6 / 8192 B **PSRAM** | 初始化 UI 后按业务事件运行 | 待测 |
| `taskLVGL` | CPU1 / 4 / 7168 B PSRAM | `lv_timer_handler` 后最少延迟 5ms；RTC 把 display refresh timer 降到 5 Hz | 旧 WDT 栈停在 flush；新占用待测 |
| `anim_player` | CPU1 / 7 / 8192 B PSRAM | RTC 前停止并释放缓冲，此后拒绝新动画请求、任务阻塞等通知 | 新占用待测 |
| `xiaozhi_dec`、`wake_process` | CPU1 / 9、8 / 20480、40960 B PSRAM | 音频队列/AFE 工作 | 新占用待测 |
| `video_report` | CPU1 / 4 / 4096 B **PSRAM** | 5 秒周期统计 | 占用预期很低，待测 |
| `camera_task` | CPU0 / 7 / 8192 B **PSRAM** | 摄像头安装/重连控制，事件和延时等待 | 待测 |

当前 WDT 仅显示 CPU1 抽样时分别在 `draw_buf_flush` 和 YUV CPU 转换；不能仅凭栈证明某个函数单次卡住 5 秒。编码队列持续有帧时，原来 `xQueueReceive` 会立即返回，CPU1 的 IDLE 可能持续饿死。新调度点保证每帧至少阻塞 1 tick；LVGL 在 RTC 期间把屏幕刷新设为 5 Hz，动画在 RTC 前释放并阻止重启。没有关闭 WDT，也没有把 LVGL 转到繁忙的 CPU0。

## DMA 安全预算与 A/B

LCD 一次刷新需约 1920 B 对齐私有 TX，Hosted 包约 1664 B，还要给驱动描述符、分配器碎片及其它同步事务留空间。因此调试预警设为**最大连续 DMA 块 < 8 KiB**，运行期紧急线设为 **< 2 KiB**：稳定推流日志警告，5 秒采样发现紧急线则正常停止该 RTC 会话，避免继续让所有模块在 0–1 KiB 下运行。8 KiB 是起始预算，不是已测得的最终最小值；应结合 10 分钟峰值再调整。

改动前日志：720p 参考块约 92224 B；`AFTER_H264` 曾见 DMA 12667 B free / 6144 B largest，推流时降到 3723 B / 1440 B，另一次 DTLS 后达 15 B / 0 B 并出现 LCD SPI 分配失败。640p 参考块约 46144 B，先前 640p 日志约 15 fps、0.75–0.9 Mbps，但第二次 RTC 被旧 Hosted 缓存增长挡住。两组不是相同固件和同一负载的并排试验，不能把这些旧值写成修改后结果。

新固件每 5 秒打印 `TASK_LOAD_5S`（CPU0/CPU1 非 IDLE 百分比与关键任务）以及 DMA free/largest。用户已要求本轮不生成 640p 固件，因此只编译/烧录 720p，并连续运行至少 10 分钟、做多次 RTC START/STOP；比较 `H264 reference requested_bytes`、`AFTER_H264_REF_ALLOC`、`STREAMING_5S`、`POOL[...]`、`TASK_LOAD_5S`、发送 fps/码率、WDT 与分配失败。

首轮 720p 实机日志（ELF SHA `71a2954a2`）：UVC 29.6 fps、0% 丢帧；RTC 已到 `DTLS_CONNECTED` 并开始 H.264 推流。参考帧分配后 DMA 仅约 12.7 KiB free / 6 KiB largest；Hosted 空闲池 3 块占约 4992 B。推流约 2 秒后，Hosted 申请一个实际约 1664 B 的 SDIO RX 块失败，`sdio_push_data_to_queue` 的断言使 P4 重启。缓存上限 3 已生效（`real_free_count=16`），但不足以留出突发接收余量。现将缓存上限改为 1，并把该路径的断言改为记录错误后丢包；需再次实机测试，尚不能认定 720p 已稳定。首轮在出现首次分配失败前没有 CPU1 WDT；运行不到 5 秒，缺少 `TASK_LOAD_5S` 样本，无法据此判断 CPU1 稳定性。

第二轮实机日志（ELF SHA `a7e9fa928`）：缓存上限 1 已生效，没有再因 SDIO RX `assert` 重启，但预览仍失败。`esp_peer_open` 后 DMA free/largest 为 4527/2048 B；紧接着 Hosted 申请 1664 B 对齐块失败，当时 largest 仅 1216 B，STUN 发包被丢弃，ICE 5 秒后只有 host candidate。此时还有 6144 B 的 WHIP HTTP 临时预留占用 L2 DMA 堆，下一版将它移到非 DMA RTCRAM。失败停止后第二次 RTC Guard 申请 96 KiB 失败，虽然 heap 最大块显示 96 KiB，说明分配器元数据/对齐开销不能忽略；下一版 Guard 改为 94 KiB（仍大于实测 92224 B reference 请求）。两处都需重新实机验证，不得把编译通过视为预览恢复。

第三轮实机日志（ELF SHA `a0895561c`）：WHIP HTTP 6 KiB 已确实占用非 DMA RTCRAM；`esp_peer_open` 后 DMA free/largest 提升到 12219/10240 B，ICE 成功获得 srflx，WHIP HTTP=201、DTLS_CONNECTED，720p H264 已推流约 22 秒且无 WDT。CPU1 约 60–65%，codec 46–50%，LVGL 3–4%，animation 0%；CPU0 约 90–97%，video_upload 53–58%。实际稳定编码/发送约 15–16 fps、5.8–6.2 Mbps，远高于 1.5 Mbps 目标；H264 码控长期 `target_bits=1`、`last_qp=40`，QP 已触顶。Hosted 池空闲缓存始终 1 块，但 DMA largest 推流时约 3.5–6.9 KiB，alloc_fail_count 增至 9，并有一次 RX 包副本分配失败。下一版只改 720p 路径：QP 上限试到硬件允许的 51；SDIO DMA 原始双缓冲和 TX 仍留 INTERNAL|DMA，仅把 DMA 完成后的 RX 软件包副本以及交给 Wi-Fi 网络栈的 CPU payload 副本移到 PSRAM。需实机确认码率、画质、CPU0 与 alloc_fail 计数，不能把编译通过视为稳定运行。

720p 当前设置的输入/编码上限为 20 fps，但旧日志 JPEG 解码约 11ms + CPU YUV 转换约 30ms + H264 编码约 16ms，总计约 57ms，超过 20 fps 的 50ms 帧周期。即使 CPU1 只运行 codec，也不能据此保证稳定 20 fps。每帧 1 tick 的调度点是为 IDLE1/LVGL 留出运行机会；若实测发送 fps 降低，需要优化转换路径或采用 640p@15 配置，而不能取消 WDT 或移除让出点。
