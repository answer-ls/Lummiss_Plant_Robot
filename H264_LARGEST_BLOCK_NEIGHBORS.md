# H264 最大连续块相邻对象定位

## 最新实测更新（neighbors诊断固件）

本节覆盖下文旧日志的待定结果。新日志已保存为 `logs/h264_fragment/neighbors_serial.txt`，全调用栈解码为 `neighbors_serial_owners.md`。当前ELF SHA256为 `C4CAF03B148599F1A55A2A07F5065C29209FB146E5F7BF1218386A5745EFF4D6`，与neighbors_manifest一致；所给日志未包含启动ELF摘要，板端身份仍以用户烧录对应构建为前提。

最大原始free：`0x4ffa3e98 .. 0x4ffb6ef8`（右端不含），77920B；API可分配largest=77824B。右侧已是region边界，没有右侧allocated splitter。本次edges=22，全部打印；live=176、bytes=51264与heap summary一致，unknown_bytes=1348。

| 顺序 | 地址 | payload大小 | 状态与owner | caps / 生命周期 / 处理判断 |
|---|---|---:|---|---|
| -3 | 0x4ffa3ca4 | 92 | newlib惰性锁mutex：lock_acquire_generic → lock_init_generic → xQueueCreateMutex | pvPortMalloc INTERNAL；底层调用者超出8层trace，具体哪把锁未确定；不能手工释放 |
| -2 | 0x4ffa3d04 | 28 | JPEG解码引擎 → dma2d_acquire_pool → esp_intr_alloc_intrstatus_bind共享中断描述 | 显式INTERNAL\|8BIT，中断生命周期；A/B保留，不迁PSRAM |
| -1 | 0x4ffa3d24 | 368 | webrtc_whip控制任务TCB | pvPortMalloc INTERNAL，RTC控制任务生命周期；B必须INTERNAL；栈已在PSRAM，不是12KB栈 |
| 0 | 0x4ffa3e98 | 77920 | FREE | API largest=77824 |
| +1..+3 | region末端 | — | REGION_BOUNDARY | 无可释放的右侧对象 |

关键源码证据：

- `components/webrtc_whip/webrtc_whip.c:1552` 创建 `controller_task / webrtc_whip`，12288B栈使用SPIRAM；IDF `components/freertos/esp_additions/idf_additions.c:46` 独立用pvPortMalloc申请TCB，并明确要求TCB在INTERNAL。
- IDF `components/esp_hw_support/intr_alloc.c:611` 为共享中断申请28B INTERNAL对象；上游 `dma/dma2d.c:441`、`esp_driver_jpeg/jpeg_decode.c:119`、项目 `video_streamer.c:2030`。
- IDF `components/newlib/src/locks.c:76/134` 创建惰性mutex；栈在这里截断，不能把它进一步归因到特定业务对象。

### 真实收益与结论

唯一直接相邻候选368B TCB：left_free=0、right_free=77920，payload合并仅78288B（不含可回收块头），不足92224。即使假设最左三个对象全部消失，payload之和也仅78408B，仍远远不足；这不是可以通过迁移一个小邻居解决的布局。

前一块17904B free结束后，`0x4ff9f640` 开始的是1920B I2S RX DMA数据buffer，调用栈定位到 `i2s_common.c:540 → i2s_tdm_set_slot → i2s_channel_init_tdm_mode → audio_hw_init`。从该地址到最大free起点的跨度为18520B，中间是一段连续allocated block链。全部EDGE没有该链内部free边缘。不能把整条链当成一个owner，也不能仅搬末尾TCB就合并前面的17904B free。

最小修改候选结论：**目前没有已证明安全、且单独能达到92224B的迁移候选。** 本次只解析日志、更新文档，不改固件。若继续定位，应针对18520B链复用现有trace快照逐块关联owner，而不是继续调整cleanup/video任务或盲迁I2S DMA。现有EDGE和±3邻居输出不能列全链内部对象。

H264本次仍NO_MEM，RTC未进入STREAMING；总INTERNAL=209819不能弥补连续块不足。

## 本轮结论与证据范围

本轮只修改 FRAG 诊断及其 PC 离线解码脚本，不修改资源布局、业务生命周期、720p/20fps、音频或 DMA 参数。

现有日志 `logs/h264_fragment/lvgl_psram_serial.txt`：INTERNAL free=210403，largest=77824，H264 实际申请92224。距离最低要求14400B，距离96KiB目标20480B。目标 heap 起点4ff861c0，长度199999；live=173、bytes=50692、overflow=0。布局覆盖与 heap summary 对应，但其中1032B/19块仍无 caller，不能声称所有 owner 已知。

**现有日志只有16/21个EDGE，未完整展示最大块邻居，不能从它还原最大块地址、左右完整布局或确认关键 splitter。以下具体邻居和收益必须等待新增诊断实机日志，不编造地址。**

| 必需证据 | 当前结果 |
|---|---|
| API 最大可分配值 | 77824B |
| 最大原始 free 起止地址 | 待新日志 LARGEST |
| 左右各3块完整局部布局 | 待新日志 NEIGHBOR |
| 紧邻候选地址、大小及PCS | 待 NEIGHBOR offset=-1/+1 |
| 单候选合并收益 | 待 CANDIDATE |
| 优先迁移/释放/延后对象 | 未定，不能按GROUP大小排序代替邻接证据 |

## 诊断输出

`src/demo/components/board/mem_fragment.c` 的 `largest_neighbors()` 在目标heap中寻找最大原始FREE块，输出：

- LARGEST：start、raw_size、end_exclusive、api_internal_largest。
- NEIGHBOR：offset=-3..3、FREE/ALLOC、ptr、size、pcs；遇到heap边界明确输出REGION_BOUNDARY。
- CANDIDATE：最大块紧邻左/右allocation，left_free、size、right_free、merged_payload及92224/98304阈值判断。
- EDGE：取消16条上限，实际多少条就打印多少条，仍只在首次H264创建前执行一次。

`merged_payload=left_free+candidate_size+right_free`，只合并真实相邻块，不跨越另一块allocation或heap边界。该值是原始payload之和，不包括释放时可合并的块头，也不保证TLSF规格取整后就能分配同样大小；最终以实际目标caps/alignment申请结果为准。

注意：heap walker原始块大小不一定等于API报告的largest。ESP-IDF `components/heap/multi_heap.c` 的统计路径会对原始最大块调用 `tlsf_fit_size()`。因此不能强制要求找到一个raw_size恰好77824的块。API在walk前采样，并发分配也可能导致瞬态差异。

## ELF身份与addr2line

在重新编译前保存旧ELF：`logs/h264_fragment/neighbors_baseline/lummiss_main.elf`。

SHA256：`08608D679B02844AB2B8276B3A9257FEA7AFC0FA6DD3CA25FFC1B13D5115639C`，与上次构建清单 `lvgl_psram_manifest.json` 一致。旧日志没有启动ELF摘要，因此这是已交付构建匹配，不能独立证明板上身份。

已使用此ELF重新执行addr2line，完整调用栈及源码行位于 `logs/h264_fragment/lvgl_psram_serial_owners.md`。新诊断固件的PC必须使用新ELF，不能使用该baseline。

## 已有GROUP源码归属

下表地址为GROUP example，多块组不代表所有块地址；也不代表最大块邻居。caps来自对应调用点源码，非trace直接记录的请求字段。

| 地址 / 大小 | caller及源码位置 | 模块、生命周期 | caps/分类 | 迁PSRAM、RTC释放或延后 |
|---|---|---|---|---|
| 4ff8c0c0 / 11520B共6块 | i2s_alloc_dma_desc，IDF esp_driver_i2s/i2s_common.c:540；上游i2s_tdm_set_slot/init_tdm_mode | I2S RX数据缓冲，通道生命周期 | I2S DMA分配caps；A | 不能盲迁；停音频不等于DMA通道可销毁，本轮保留 |
| 4ff88b84 / 6144B | esp_mqtt_client_start，IDF mqtt/esp-mqtt/mqtt_client.c:1876 | MQTT任务栈，客户端运行期间 | FreeRTOS INTERNAL/8BIT；B待调用链进一步验证 | RTC仍需MQTT；不能未经cache-disabled调用审计迁栈 |
| 4ff86ae8 / 4096B | esp_mqtt_set_config，mqtt_client.c:435 | MQTT输入buffer，客户端生命周期 | malloc默认caps，CPU软件buffer；C候选 | 可研究PSRAM；RTC不能直接释放；邻接收益未证实 |
| 4ff87f40 / 3072B共2块 | hosted_malloc_align:132 → sdio_rx_get_buffer，sdio_drv.c:849 | Hosted SDIO RX，通信期间 | 对齐DMA分配；A | 保留，不盲迁 |
| 4ff8b440 / 3072B共6块 | i2s_alloc_dma_desc:540 → i2s_std_set_slot → set_drv_fs:267 | I2S TX数据buffer，通道生命周期 | I2S DMA分配caps；A | 保留，不盲迁 |
| 4ff8a388 / 2432B | esp_tls_init，IDF esp-tls/esp_tls.c:195 | MQTT TLS上下文，连接期间 | calloc默认caps；C候选须核查所有访问 | 不关闭RTC信令连接；不凭大小迁移 |
| 4ffa1a40 / 1920B | lvgl_port_add_disp_priv，esp_lvgl_port/src/lvgl8/esp_lvgl_port_disp.c:348 | LCD第一个绘图buffer，显示生命周期 | buff_dma=true，MALLOC_CAP_DMA；A | 不能随LVGL主题/样式一起迁移 |
| 4ffa2200 / 1920B | 同函数，esp_lvgl_port_disp.c:351 | LCD第二个绘图buffer，显示生命周期 | 同上；A | 保留 |
| 4ff866e4 / 1024B | mqtt_msg_buffer_init，mqtt/lib/mqtt_msg.c:623 | MQTT输出buffer，客户端生命周期 | calloc默认caps；C候选 | 可研究PSRAM；不能RTC期间直接释放 |
| 4ffa3100 / 576B | get_block_buf，components/esp_driver_sdspi/src/sdspi_host.c:288 | SDSPI bounce buffer，slot生命周期 | 显式MALLOC_CAP_DMA；A | 不能盲迁；释放须先停全部SD访问，不作为当前建议 |
| 4ffa1384 / 544B | esp_lcd_new_panel_io_spi，IDF esp_lcd/spi/esp_lcd_panel_io_spi.c:78 | LCD SPI IO控制对象，显示生命周期 | calloc默认caps，涉及SPI回调；B保守保留 | 未完成ISR/cache访问审计，不能归C |
| 多地址 / 1032B共19块 | unknown | 未确认 | 未确认 | 禁止据此释放 |

11520/6块来自I2S RX的结论由调用栈和源码得出，不是因为1920B大小吻合。LCD两块1920B与I2S块同大小但用途不同，进一步说明不能凭大小判断。

## 候选决策和下一次验收

目前没有证据可排序真正的左右splitter。最小下一步是运行本轮诊断固件一次RTC启动，取得全EDGE和局部布局，用该固件ELF解码两侧PC，再按真实邻接收益排序。

若两侧是DMA/内部必需对象，保留；若是CPU-only软件buffer，才考虑单一owner类别PSRAM实验；若RTC仍需要，不直接释放；延后创建只有在不破坏依赖、且后续内存预算足够时才可评估。

不再调整wake_cleanup或video worker顺序：当前日志已证明cleanup删除后largest恢复77824，video task/queue创建前后largest不变。

本轮尚未实机验证新布局，未修复H264，也未宣称达到STREAMING。实际迁移后的验收仍为INT largest至少92224（建议98304）、H264 REF ptr非NULL、随后验证RTC持续运行和重复启停。

## 构建交付

`src/demo/build_rtc_mem_b` 和 `src/demo/build` 均增量构建成功，未烧录。构建日志为 `logs/h264_fragment/neighbors_build_rtc_mem_b.log`、`neighbors_build.log`；新ELF/BIN哈希见 `neighbors_manifest.json`。IDF扩展可继续使用原来的build_rtc_mem_b目录烧录。
