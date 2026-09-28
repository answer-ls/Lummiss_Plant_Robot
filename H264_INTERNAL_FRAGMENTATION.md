# H264 INTERNAL 碎片定位

## LVGL软件内存迁移实测

新日志 `logs/h264_fragment/lvgl_psram_serial.txt`，用lvgl_psram_manifest对应ELF解析。
与上一单类别实验对比，H264前INT free由204651增至210403（+5752）；
largest由69632增至77824（+8192），但仍小于92224，缺14400B连续空间。
目标reserve已分配载荷55960→50692（-5268），存活块294→173（-121）。
这些是不同启动的观测差值，不是每个字节均能单独归因LVGL的精确计量。
结合已验证的分配路径改动，结果支持LVGL软件小对象是碎片贡献者，迁移有效但不是唯一原因。

现存大组仍是I2S DMA、MQTT任务栈/软件收发buffer/TLS、Hosted RX和LCD DMA；不据此认定其中任一组可直接删除。
当前EDGE总数21而输出上限16，末尾5条未输出；因此不能用本日志确认最终大free前全部bridge owner。
没有新增统计或改变上限，后续若按现有已确认CPU-only类别做实验，可优先审核MQTT软件收发buffer，
不包括MQTT任务栈/TLS，也不保证迁移其5120B足以修复连续空间。
本次cleanup完整回收，video task/queue前后largest仍77824；RTC失败后AFE恢复ESP_OK。
本轮只分析与归档，未修改下一类别固件。

## 下一单类别：LVGL软件内存迁PSRAM

仅修改 `managed_components/lvgl__lvgl/src/misc/lv_mem.c` 的LV_MEM_CUSTOM分支，
覆盖当前翻译单元的ALLOC/REALLOC/FREE宏为heap_caps对应接口。
alloc/realloc都指定SPIRAM|8BIT，不回退INTERNAL；free使用heap_caps_free。
保留LVGL既有零长度sentinel、realloc失败保留旧块及初始化流程。
主题、样式、控件、CPU渲染临时对象从创建时进入PSRAM，不复制活动对象。

当前GPU后端均关闭。LCD port的disp_ctx、disp_drv、draw_buf控制结构使用独立malloc，
绘图buf1/2使用独立heap_caps_malloc(MALLOC_CAP_DMA)，均不经过lv_mem接口。
SPI完成回调仅访问port的drv/draw_buf标志与信号量，不遍历本次迁出的主题或控件。
这些控制结构、1920B×2 DMA绘图缓冲及driver descriptor本轮都未改。
普通任务持LVGL锁执行UI更新，迁移不扩展ISR或cache-disabled路径权限。

这是单独一个owner类别实验：保持capture_diag已迁PSRAM，不修改cleanup、
video顺序、任何音频链或720p@20参数；不增加heap统计，也不修改sdkconfig/Kconfig。
使用原有MEM_CONTIG/FRAG判定实际largest，不能仅凭软件分配改向PSRAM就宣布H264修复。
实机还需确认LCD刷新正常、H264_REF_ALLOC成功、STREAMING及后续START/STOP。

此修改位于managed component；升级/重新下载LVGL组件时需保留或重放该补丁。
新固件构建前已备份上版ELF至 `logs/h264_fragment/lvgl_baseline/`，禁止用新ELF解旧日志。

## capture_diag栈迁移实测结果

日志 `logs/h264_fragment/bridge_stack_serial.txt`，对应ELF SHA256
`DB3460E1F9ACCBDDA0547C89E292A97BC99D43382C00AEB7037E4AD670398CDA`，已核对bridge_stack_manifest。
未新增统计、未修改第二类owner。

| H264前指标 | 迁移前focused_serial | 迁移后bridge_stack_serial |
|---|---:|---:|
| INT free | 202823 | 204651 |
| INT largest | 69632 | 69632 |
| reserve allocated payload | 57876 | 55960 |
| reserve live blocks | 272 | 294 |
| H264 REF | NO_MEM | NO_MEM |

原4096B capture_diag INTERNAL栈组不再出现，与本次仅迁该栈的代码改动一致；
LCD两个1920B buffer地址分别由4ffa2c40/4ffa3400前移至4ffa1c40/4ffa2400，差值均4096B。
但新运行中其它存活分配数量增加22，净free仅增加1828B，不能把不同启动的净差当栈大小。
尾部原始free从70928变为72732；有效largest仍69632，没有达到92224，迁移未打通bridge。
不能从这次结果宣称H264修复或capture_diag为唯一根因。
后续只考虑独立CPU-only类别，例如LVGL主题/样式/软件控件；LCD DMA绘图buffer保持不动。

## Bridge单类别实验：仅capture_diag栈迁PSRAM

### ELF校验与证据边界

解析前再次核对 `build_rtc_mem_b/lummiss_main.elf` SHA256：
`176ED5DACFCEB3B980AE5E4414BABCFE12D9F165A755092D4A80A496DDA66B4A`，
与focused_manifest完全一致；未使用build目录旧ELF。日志片段无启动ELF SHA行，
这一匹配是对已交付该版构建的校验，不能代替板端固件身份回读。
重编译前已保存该ELF到 `logs/h264_fragment/bridge_baseline/lummiss_main.elf`。
指定GROUP4/8/9/12所有PC均已实际执行addr2line解析，见focused_serial_owners.md。

当前focused日志只有12组聚合和边界，不包含bridge每个allocation的地址/调用栈。
不能将上一版不同运行的154块表当作本次完整表，也不能对数据地址addr2line得到owner。
本轮不新增统计；缺失的其余owner明确未知，不能宣称“全部解析完成”。

### 本次bridge内可证实的对象

| allocation | size | caller与源码位置 | 模块/生命周期 | 请求caps与DMA | 必须INTERNAL | RTC需要 | 可迁PSRAM | 可RTC_START释放 |
|---|---:|---|---|---|---|---|---|---|
| 4ffa1544 | 4096 | capture_diag_init，capture_diag.c:205；xTaskCreatePinnedToCore | 音频A/B诊断导出栈，初始化至两窗口导出完成 | pvPortMalloc的INTERNAL\|8BIT；非DMA | 否，任务只做内存统计/排序/base64/串口输出 | RTC本身不需要；诊断可能仍在等待 | 本轮仅迁此栈 | 未加同步删除；不在导出中强删 |
| 4ffa2c40 | 1920 | lvgl_port_add_disp_priv，esp_lvgl_port_disp.c:348 | LCD buf1，display生命周期 | buff_dma=true，MALLOC_CAP_DMA | 当前传输路径保留 | 事件刷新仍需 | 本轮禁止迁 | 不能单独free仍注册的绘图buffer |
| 4ffa3400 | 1920 | lvgl_port_add_disp_priv，esp_lvgl_port_disp.c:351 | LCD buf2，同上 | 同上 | 同上 | 同上 | 本轮禁止迁 | 同上 |
| 4ffa3d54 | 608 | lv_mem_alloc → lv_theme_default_init，lv_theme_default.c:654 | LVGL主题，UI生命周期 | 普通malloc，DEFAULT路径；非直接DMA | 软件对象可评估 | RTC保持UI仍引用 | 可作为后续独立类别审计 | 不能直接释放活动主题 |
| 4ffa5874 | 368 | xTaskCreatePinnedToCoreWithCaps → webrtc_whip_request_start，webrtc_whip.c:1552 | RTC控制任务TCB，任务生命周期 | pvPortMalloc的INTERNAL\|8BIT；非DMA载荷 | 是，IDF要求TCB内部内存 | 是 | 否 | 正在处理RTC，不能删除 |
| 4ff9f63c | 64 | trace未匹配 | 未知 | 未知 | 未知 | 未知 | 不动 | 不动 |

### 唯一固件改动及验证方法

只修改 `components/xiaozhi_audio/capture_diag.c`：
`xTaskCreate` 改 `xTaskCreateWithCaps(..., MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT)`；
栈仍4096B、优先级仍1、仍不绑定核。正常结束及错误结束均改配套vTaskDeleteWithCaps，
避免WithCaps栈/TCB泄漏；IDF自删除使用临时回收任务，不是wake_cleanup。
TCB仍INTERNAL。没有关A/B总开关，因为该开关还控制lifecycle/audio_probe互斥，
直接关闭会改变其他实验路径；UDP、AFE及诊断采集本身均未改。
新增注释中文，无新heap统计，不修改cleanup/video创建顺序，不碰LCD DMA buffer。

仅能确定4096B栈的请求转到PSRAM，实际largest变化及H264成功仍需实机。
下一次先看现有H264_REF_ALLOC_BEFORE是否>=92224（建议98304），
未达标即记录失败，不将total free增加当成功，也不在同一版迁第二类对象。

## 最新实测owner（focused_serial）

原日志 `logs/h264_fragment/focused_serial.txt`，完整离线调用栈见
`logs/h264_fragment/focused_serial_owners.md`；使用focused_manifest记录的build_rtc_mem_b ELF。
272个存活块、57876B载荷、8个free；trace无溢出，布局无截断。
unknown为692B/12块；仅输出前12组，不能把下表当作全部分配。

| 来源 | 大小/数量 | 实际创建点 | 处理判断 |
|---|---:|---|---|
| I2S TDM RX DMA数据缓冲 | 1920×6=11520B | i2s_alloc_dma_desc:540 → i2s_tdm_set_slot → audio_hw_init:1197 | 硬件DMA，不迁 |
| MQTT任务栈 | 6144B | esp_mqtt_client_start:1876 → cloud_mqtt_start:532 | 迁栈前必须审计TLS/flash/cache路径，不直接迁 |
| MQTT接收buffer | 4096B | mqtt_client.c esp_mqtt_set_config:435 | 普通malloc软件载荷，可作为后续PSRAM候选 |
| capture_diag导出任务栈 | 4096B | capture_diag.c:205 → service_task:2164 | 旧音频A/B诊断仍启用；任务等待两次窗口，RTC不会自行释放 |
| Hosted SDIO RX buffer | 2块共3584B | hosted_malloc_align → sdio_rx_get_buffer:849 | SDIO DMA，不迁 |
| I2S STD TX DMA数据缓冲 | 512×6=3072B | i2s_alloc_dma_desc:540 → i2s_channel_reconfig_std_slot → set_drv_fs | 硬件DMA，不迁 |
| MQTT TLS上下文 | 2432B | esp_tls_init:195 → ssl_connect → esp_mqtt_task | 需核对库内访问约束，暂不迁 |
| LCD双绘图buffer | 1920×2=3840B | esp_lvgl_port_disp.c:348/351 → lvgl_initialize:351 | 当前DMA用途需保留 |
| MQTT输出buffer | 1024B | mqtt_msg_buffer_init:623 → esp_mqtt_set_config:430 | 普通软件载荷，可作为后续PSRAM候选 |
| LVGL默认主题 | 608B | lv_theme_default_init:654 | CPU-only普通malloc候选；并非全部LVGL占用 |

边界块额外确认：39080B空闲区左侧是20B LVGL style属性，右侧是28B newlib _Balloc；
10364B空闲区边界是GDMA中断分配对象；17904B左侧32B为codec I2C控制对象；
尾部70928B原始free前的368B是webrtc_whip_request_start:1552创建的任务TCB。
该TCB即使释放，单块合并估计71296B，仍不足92224B；不能删除仍在执行的RTC控制任务。
heap API可申请largest=69632与walk原始free=70928并非同一口径，验收用真实H264分配结果。

本轮cleanup占用8568B，删除前后INT从197163到205731、largest从61440到69632，
恢复为RTC_START前的69632；没有遗留cleanup栈导致本次REF失败。
VIDEO_TASK_QUEUE阶段largest一直69632；本次不能靠将H264提前到该阶段之前声称解决。
音频恢复已实测完成：RTC_AUDIO restore=ESP_OK。

最小下一步建议：先单独停用与RTC无关的capture_diag A/B诊断，观察largest，而非只看free；
若仍不达标，再单变量处理确认CPU-only的MQTT软件buffer或LVGL软件对象。
这只是后续候选顺序，不保证节省4096B即可合并出92224B；本轮只定位，未修改固件。

## 当前输出策略

本轮定向owner模式 `CONFIG_H264_FRAGMENT_FOCUSED=y`：开启首次H264前的trace，
但全量verbose仍关闭。最多16条EDGE、12条GROUP，附少量统计，不逐层展开PC。
调用栈使用无0x前缀的hex，使用 `tools/decode_fragment_owners.py --elf ...` 离线解析。
因此下文“不开启trace”仅指两个开关都关闭的普通运行模式。
诊断结束释放PSRAM记录；不迁移业务对象，不调整H264/音频参数。

### 已确认的reserve机制与布局

本地IDF `esp_psram_extram_reserve_dma_pool()` 将内部区域申请出来，再用
`heap_caps_add_region_with_caps()` 注册为嵌套heap；它不是只供H264使用的池。
caps包含INTERNAL、DMA及低优先级DEFAULT/8BIT，普通显式INTERNAL分配也可进入。
`heap_caps_malloc_default()` 对小对象优先请求DEFAULT|INTERNAL，因此普通业务小块
也可能进入该区域；必须查caller，不能把region中每个块都当作硬件DMA。
父heap本身还覆盖该地址，分析必须按heap.start精确匹配，不能只判断地址范围包含。

已保存的完整block快照：8个free载荷为260、60、38496、872、10364、17904、1344、68572B。
没有单个allocated block释放后能达到92224B；单块最好合并估计70284B。
按需搬移载荷最少的free到free区间：`4ff9b060`至最后free `4ffa631c`，
跨154个allocated块，共25884B载荷，理论载荷合计113704B。
这只是布局算术，不表示这些块均可释放，也不是迁移建议。
旧附件的OWNER部分在目标region输出前截断，当前无法命名这些块的业务owner。
下一次定向日志按完整调用栈聚合，补齐目标region证据，不重复输出其他heap。

全量诊断开关 `CONFIG_H264_FRAGMENT_VERBOSE` 默认关闭：不启动boot trace、
不申请其PSRAM缓冲、不输出BLOCK/OWNER/CALLER，保留MEM_CONTIG和H264结果。
只有明确需要重采完整布局时才在menuconfig启用；下文全量流程仅适用于开关开启时。
已收到的583块快照和部分caller已保存于 `logs/h264_fragment/serial_verbose.txt`。
附件没有LAYOUT_END，owner记录不完整；不能把缺失caller视为无owner。

## 已有证据与当前边界

H264 REF实际申请92224B，已有日志INTERNAL最大连续块69632B，失败为连续空间不足。
旧固件region `0x4ff861c0` 长199999B、空闲140771B、8个空闲块、269个存活块。
summary没有给出block地址，**无法从这些数字还原完整布局或判定owner**。
本轮新增采集和分析工具，未迁移任何业务对象，未调整创建顺序；不能宣称碎片已修复。

## 1. 完整block布局

首次H264创建之前输出 `FRAG BLOCK heap=... end=... ptr=... size=... used=...`。
使用IDF `heap_caps_walk(MALLOC_CAP_INTERNAL)` 取得实际payload指针、大小、状态。
回调只复制到预分配PSRAM，不打印、不分配；遍历结束后才输出。
各heap快照非跨核全局原子快照，其他任务可继续分配；日志输出期间也会变化。
不硬编码旧region过滤：链接后起点可能改变，采集全部INTERNAL块，PC选择对应region。
本次真实地址排序表：**等待新固件实机日志**，不能用示例地址充当证据。

## 2–5. Splitter、owner、左右空闲和理论合并大小

`mem_fragment_begin()` 在app_main入口开始IDF HEAP_TRACE_LEAKS，容量4096条，缓冲PSRAM。
首次H264 open前停止，匹配trace用户指针与布局中allocated block范围，输出OWNER及各层CALLER PC。
trace结束后释放诊断缓冲，已有Peer trace仍在后续独立窗口运行。
不记录ISR分配；app_main之前已有分配没有caller；overflow或并发释放会留下未知owner。
只可用同次固件ELF反解CALLER；不能凭size或任务名字猜owner。

PC脚本按地址排序；对每个allocated block只取直接相邻free block，计算：
`left_free + candidate_payload + right_free`，按结果降序列出候选并标记是否>=92224。
这是payload之和，未加回收后allocator元数据，实际对齐可用大小仍以真实申请为准。
不能跨越另一个存活allocation或跨heap合并。
若没有单块达标，需分析连续多块集合，不能虚报单块修复。

## 6–8. 迁移、延后与RTC释放资格

| 候选类型 | PSRAM | 延后至H264后 | RTC_START释放 |
|---|---|---|---|
| wake_cleanup栈/TCB | 禁止迁栈；清理含munmap | 清理必须先完成 | B已完成后同步删除；需地址证据确认不再存活 |
| CPU-only task stack/buffer/诊断载荷 | 核对cache/ISR调用后可考虑 | 核对启动依赖 | 必须先退出任务/停止使用 |
| 普通queue载荷 | 核对API和ISR访问后判断 | 核对生产消费顺序 | 必须停双方且无未完成操作 |
| TCB、锁、驱动控制对象 | 未确认，默认不动 | 依调用链确认 | 依对象生命周期确认 |
| Hosted/USB/I2S/JPEG/H264硬件DMA对象 | 不盲迁 | 仅在硬件启动顺序允许时 | 驱动完成停止后才可回收 |

实际每个splitter的上述资格：等待地址→caller→源码核对，当前未确认。

## 9. 最小修改建议和验收

先运行本诊断固件一次，保存从启动至 `LAYOUT_END` 和H264结果的完整日志。
对排名最高且能合并至目标的候选，只选一个对象进行实验。
优先合法延后创建；其次迁CPU-only载荷；不碰真实DMA或cache-disabled执行栈。
H264成功后还必须确认延后的对象都创建成功、运行期DMA余量充足。
保持encoder before Peer及1280x720@20、QP、码率、音频算法和DMA参数。

每次核对 `MEM_CONTIG[H264_REF_ALLOC_BEFORE]` 最大块>=92224，建议>=98304；
真正通过仍需REF非空、STREAMING、60秒预览及10轮START/STOP恢复和内存稳定性。

## 操作与文件

IDF扩展烧录目录仍为 `src/demo/build_rtc_mem_b`，同时构建 `src/demo/build`；不自动烧录。
该固件是一次性启动诊断：trace与大量布局输出会增加时延，不用于性能基准。
每次复位单独保存日志到项目 `logs/h264_fragment/`。没有RTC请求时trace会继续到首次H264请求。

```powershell
& $py E:\Lummiss_Plant_Robot\src\demo\tools\analyze_h264_fragment.py E:\Lummiss_Plant_Robot\logs\h264_fragment\serial.txt --elf E:\Lummiss_Plant_Robot\src\demo\build_rtc_mem_b\lummiss_main.elf --addr2line C:\Espressif\tools\riscv32-esp-elf\esp-14.2.0_20260121\riscv32-esp-elf\bin\riscv32-esp-elf-addr2line.exe
```

输出实测报告到 `logs/h264_fragment/serial_H264_INTERNAL_FRAGMENTATION.md`。
若实际region未覆盖旧地址，使用 `--region 0x实际region起点`，不强套旧布局。

新增 `components/board/mem_fragment.c`（begin/dump/walker）、分析脚本和3项测试；
修改board CMake及mem_contig头文件、main入口、video_streamer H264 open日志调用点。
本轮未修改wake_cleanup执行上下文、音频链、DMA缓冲或其他业务对象。

验证：build_rtc_mem_b 和 build 均编译成功；分析脚本3项单元测试通过。未烧录，实测block表及owner仍待日志。构建日志与产物哈希见 logs/h264_fragment/。

定向诊断请使用IDF扩展目录 build_rtc_mem_b（现有trace深度8）；build 保持原深度3兼容，可能只能看到分配器包装层，不能用浅栈强行认定业务owner。两者本轮都未修改trace深度。

定向owner版已在 build_rtc_mem_b 与 build 编译通过，未烧录；日志和固件哈希见 logs/h264_fragment/focused_*。实际owner仍需本版实机FRAG GROUP/EDGE输出。

Bridge栈单变量版：两个目录增量构建均通过（仅capture_diag.c重新编译）；未烧录。产物哈希见 logs/h264_fragment/bridge_stack_manifest.json。

LVGL单类别版：两目录增量编译成功；ELF反汇编确认lv_mem_alloc/realloc均传入caps=0x404（SPIRAM|8BIT）。未烧录、未实测largest变化，哈希见 logs/h264_fragment/lvgl_psram_manifest.json。
