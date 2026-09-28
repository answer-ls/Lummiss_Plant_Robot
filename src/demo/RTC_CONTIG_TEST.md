# H264 连续 INTERNAL 区域定位与 cleanup A/B

当前 blocker：reference 需要实际92224B连续 INTERNAL，已有日志最大块65536B。
模型清理的 cache assert 本轮不再作为待修问题。
本轮不改720p/20fps、QP/码率、Codec/AFE/MMR、重采样、Opus/UDP或DMA描述符配置。

## 两组固件

| 组 | 配置 | 构建目录 | cleanup生命周期 |
|---|---|---|---|
| A | `CONFIG_WAKE_CLEANUP_TRANSIENT`关闭 | `src/demo/build_rtc_contig_a` | 原来的静态DRAM栈8192B、静态TCB，永久等待任务通知 |
| B | 配置开启（默认） | `src/demo/build_rtc_mem_b`、`src/demo/build` | 初始化及清理时按需创建INTERNAL栈，初始化成功或完整deinit结束后同步删除；待机不保留任务 |

A使用独立 `logs/rtc_contig/sdkconfig_a`，不覆盖IDF扩展的配置。
两组有相同诊断日志，唯一功能变量是cleanup任务的存活/分配方式。
去掉静态对象必然也会改变启动时DRAM边界，因此既要对比两次冷启动，
也必须观察B组同一轮删除前后的largest差值，不能把两固件所有差异都归因于删除。

原实现：第一次 `wake_word_init()` 时创建任务，在模型加载之前；栈8192B和TCB
用DRAM_ATTR静态存储。静态对象不是heap分配，删除静态任务不会让其数组变成空闲heap。
B使用 `xTaskCreatePinnedToCoreWithCaps(INTERNAL|8BIT)`；TCB由IDF内部申请。
保留的两个静态信号量用于串行请求及完成确认，不跟随任务删除。
初始化过程保留同次worker供失败回收使用，成功后才删除。RTC清理前申请失败则
返回ESP_ERR_NO_MEM，保持原AFE资源并恢复门控，RTC不得继续准备编码器。

## 回收不是靠延时猜测

worker完成fetch退出、AFE销毁和模型munmap后给完成信号。调用方随后调用
`vTaskDeleteWithCaps(worker)`，不让worker自删。
已检查本地IDF5.5.5的 `freertos/esp_additions/idf_additions.c:prvTaskDeleteWithCaps`：
它先暂停目标，等待目标不再是任一CPU的当前任务，再同步执行TCB销毁及栈/TCB free。
因此 `WAKE_CLEANUP_DELETE_AFTER` 位于真实回收返回之后，不依赖IDLE未来回收，
也不使用一个固定vTaskDelay假装回收完成。若更换IDF版本，应重新核查这个实现。
capture_guard仍由上层持有，worker不获取它；RTC仍在完成ACK后创建编码器。

## 阶段日志

统一格式：`MEM_CONTIG[tag] us=... INT=free/largest DMA=free/largest PSRAM=free/largest`。
`ref_ok`表示largest至少92224；`target_96KiB`表示至少98304。它们只是预算指示，
并不替代带alignment/caps的实际申请结果。每项为相邻时刻采样，非多核原子快照。
92224B门槛用于尚未分配reference时；成功分配后的 `H264_REF_ALLOC_AFTER`
低于该门槛可能只是正常占用，不能把它判成新的启动故障。

| 标签 | 实际插入点 |
|---|---|
| HOSTED_INIT_DONE | esp_hosted_init完成RPC/transport设置后，app_main之前的弱钩子 |
| APP_MAIN_ENTER | app_main第一条语句 |
| HOSTED_WIFI_READY | esp_wifi_init成功返回后，补充链路/WiFi就绪阶段 |
| USB_INIT_BEFORE / USB_UVC_INIT_AFTER | USB Host/UVC安装前后；按需启动，若H264先失败则不会出现 |
| DISPLAY_INIT_BEFORE / DISPLAY_INIT_AFTER | LCD及LVGL端口创建前后 |
| AUDIO_TASKS_BEFORE / AFTER | 音频工作任务批量创建前后 |
| AFE_CREATE_BEFORE / AFTER / AFE_INIT_DONE | AFE库创建前后、feed/preroll/输出流/fetch任务完成后 |
| WAKE_PROCESS_CREATE_BEFORE / AFTER | 控制任务创建前后 |
| WAKE_CLEANUP_CREATE_BEFORE / AFTER | 模型生命周期任务创建前后；A的静态栈已在启动前占用 |
| MODEL_LOAD_BEFORE / AFTER | 实际esp_srmodel_init前后 |
| WAKE_IDLE_STABLE | 引擎每次初始化后首次待机5秒统计点 |
| RTC_START | RTC控制器进入STARTING、释放音频之前 |
| AFE_WAKE_RELEASED | 清理worker完成AFE/模型释放，但worker仍存在 |
| WAKE_CLEANUP_DELETE_BEFORE / AFTER | B删除任务和实际回收前后 |
| WAKE_CLEANUP_INIT_DELETE_BEFORE / AFTER | B初始化成功后回收任务，待机不保留栈/TCB |
| WAKE_CLEANUP_RETAINED_A | A保留任务，明确不声称已经回收 |
| VIDEO_CODEC_TASK_CREATE_BEFORE / AFTER | 创建codec任务前后 |
| VIDEO_TASK_QUEUE_BEFORE / AFTER | codec任务进入后到JPEG引擎/输出队列/上传任务就绪 |
| H264_OPEN_BEFORE / H264_REF_ALLOC_AFTER | 实际编码器分配前后 |

多任务运行会使创建前后夹杂其他任务分配，阶段标签定位区间，不能代替分配调用栈。
第一次编码器创建失败还打印 `heap_caps_print_heap_info(INTERNAL)`，分别查看各heap的
范围、空闲和最大块；多个heap的free不能拼接。若阶段仍不足定位，下一步才扩展
启动窗口的heap trace（当前已有peer窗口trace，不能冒充启动期trace）。

## RTC资源顺序审计

现有顺序：音频同步释放 -> video准备 -> codec任务初始化PSRAM输入/输出帧、
JPEG engine、输出队列、video_upload/video_report -> H264 new/open -> Camera/Peer按RTC流程。
实际H264仍早于Peer；本轮没有同时调整创建顺序。

准备阶段内部小对象包括input/output队列控制/载荷、JPEG engine/descriptor、
任务TCB等。视频大帧与任务栈已经是PSRAM。上一份实测总内部空闲约减少2916B、
largest保持65536，说明此前已经低于门槛；仅前移这段内的H264创建不能保证解决。
如果新阶段记录发现这段首次跨阈值，再单独把H264实际创建前移，不能先扩大队列。

## RTC前长期对象分类

A=硬件DMA必需；B=必须INTERNAL但不以DMA数据用途分配；C=CPU-only，可考虑PSRAM；
D=未确认。大小为源码配置/公式，不是本次实时heap地址清单。完整检索位置在
`logs/rtc_contig/allocation_sites.txt`，既有详细表见 `RTC_MEMORY_ARCHITECTURE.md`。

| 对象/位置 | 大小/生命周期 | 分类与本轮处理 |
|---|---|---|
| wake_cleanup栈/TCB，wake_word.c | 8192B/sizeof(StaticTask_t)；A永久，B到deinit | B，必须支持cache关闭；只做生命周期A/B |
| wake_cleanup请求锁/完成信号 | 两个StaticSemaphore_t，永久 | B，保留内部静态控制对象 |
| wake_process / mic / decode / speaker栈 | 40960/40960/20480/6144，业务期 | C，已经PSRAM，不重复迁移 |
| wake_detect / AFE内部任务 | 4096/库内部大小，RTC前释放 | D，库API及cache约束不能凭名称推定 |
| FreeRTOS任务TCB | 每任务sizeof，随任务存活 | B，IDF默认内部内核对象 |
| I2S RX/TX DMA ring及descriptor | 6×240及对应槽宽，音频硬件期 | A，不动 |
| 采集PCM、Opus、播放用户PCM | 编码器实际输入/输出大小、池深度 | C，已PSRAM；不等于I2S DMA ring |
| AFE feed/preroll/output_stream载荷 | chunk×3×2、两秒PCM、7680B | C，已PSRAM；AFE库隐藏对象为D |
| AFE输出锁/事件组/stream控制 | sizeof控制对象，引擎期 | B/D，不把全部控制对象当可随意迁移载荷 |
| Hosted固定TX pool | 2×1664B，联网期 | A，不动 |
| Hosted缓存块、RX双缓冲、SDIO寄存器缓冲 | 依运行长度/配置，联网期 | A，不动；必须独立于固定pool计数 |
| Hosted线程栈/RPC队列/锁 | 驱动配置/深度 | D，具体cache/ISR使用需逐调用核查 |
| Hosted已完成RX后的协议副本 | 逐包长度 | C，当前已有PSRAM路径 |
| USB Host frame list/必要descriptor | 驱动配置，Host期 | A；本次RTC_START时camera尚未初始化 |
| USB/UVC内部线程栈和控制对象 | 4096及驱动配置，Host期 | D，保留；不是看到USB就全部归为DMA |
| UVC URB/frame | 当前驱动允许的PSRAM+DMA，stream期 | A，不改DMA布局 |
| LCD SPI真实传输缓冲/descriptor | 行缓存3840B及驱动动态块 | A，不动 |
| LVGL任务栈/普通UI控件 | 7168B栈已PSRAM；控件依实际对象 | C，栈已迁；控件需allocator/caller确认 |
| animation SD读/索引/帧 | 4096/最多12288/3×153600，RTC释放 | C，已有PSRAM，不重复计算收益 |
| MQTT/MCP普通业务消息、JSON | 实际消息长度/队列深度 | C；底层TCP/TLS/锁/任务中的隐藏对象为D |
| video_codec/upload/report栈 | 已PSRAM；创建于资源准备阶段 | C，保留；TCB仍B |
| video input/free/ready队列 | 槽索引/指针×深度及控制块 | 载荷C，控制块B/D；本轮只测量 |
| JPEG engine/硬件descriptor | 驱动实际分配 | A/D，不能强迁PSRAM |
| H264 reference/db_tmp/descriptor | 92224/约10368/对齐descriptor，编码器期 | A，保留真实组件约束 |
| WHIP SDP/凭据、Peer控制 | SDP显式PSRAM；预编译内部未知 | C/D；本次失败时Peer尚未创建 |
| 普通应用ringbuffer/event group/semaphore | 随模块大小/生命周期 | 载荷可为C；ISR/cache使用不明确者D，不批量迁移 |

本轮没有迁移任何C对象，不把静态审计当作已证明的碎片占用者。

## 实机流程和判据

先A后B，每次冷启动、相同外设/网络/待机时长。每组使用匹配的ELF解码日志，
原始串口日志全部存到 `logs/rtc_contig/`。A组若首次H264失败，保存现场即可。
默认IDF扩展仍指向B的build_rtc_mem_b；A需显式使用build_rtc_contig_a及其flash_args。

B成功必须同时出现 reference非空、`STARTING -> STREAMING`、浏览器持续运动。
预览至少60秒后STOP，确认AFE恢复；连续10轮，逐轮比较相同阶段free/largest。
未做板端测试前不填写PASS，也不将192KB总空闲称为内存足够。

可分析每次启动的串口文件：

```powershell
& $py E:\Lummiss_Plant_Robot\src\demo\tools\analyze_rtc_contig.py E:\Lummiss_Plant_Robot\logs\rtc_contig\B_serial.txt
```

脚本列出首次及后续阈值跨越、每轮cleanup free/largest差值，并保存完整JSON。
复位后单独记录文件；未出现某阶段是缺失证据，不补造记录。
如果B的free上升但largest不变，不能认定cleanup是碎片根因，继续定位更早区间。
