# H264 BACKWARD_CHAIN 审计

## 最新完整链实测结论（覆盖下文旧版待测结论）

输入 `logs/h264_fragment/backward_serial.txt`；已用当前ELF解析，SHA256 `B2E9B19A2B8613462F8CB251DD619FD0D8C0BC45AD964B5A1FD3986BA2EDDA67` 与 backward_manifest 一致。片段不含板端ELF摘要，仍不能独立验证板端身份。完整源码调用栈保存于 `backward_serial_owners.md`。本轮仅更新报告和离线结果，无固件改动。

最大FREE现在为 `[0x4ffa3e64,0x4ffb6ef8)`，raw77972B，API77824B。前置FREE在0x4ff9b020，17968B；中间52个allocated blocks，从0x4ff9f654到0x4ffa3e64，共18448B物理跨度。所有54行及52行累计表见文末。

| 后缀移除数 | 起点 | 合并物理大小 | 结论 |
|---:|---|---:|---|
| 45 | 0x4ffa0d84 | 90484 | 不足92224 |
| 46 | 0x4ffa0600 | 92408 | 首次跨过92224，只有184B物理余量，不保证分配成功 |
| 51 | 0x4ff9f664 | 96404 | 仍不足98304 |
| 52 | 0x4ff9b020（包含左FREE） | 114392 | 首次跨过98304 |

**最短物理suffix为index7..52，共46块；96KiB为index1..52全部52块，并合并index0 FREE。不是删除46个任意对象，也不是总计腾14KB即可。** TLSF档位和cache对齐条件下，92408物理大小仍不能直接宣布足够。

必要46块中包括：

- index7、9：I2S RX DMA数据buffer，各1920B。index3、5同属RX但不在最短92224物理suffix内。四块均由i2s_alloc_dma_desc:540 → i2s_tdm_set_slot → audio_hw_init确认。
- index11..23：LCD SPI DMA descriptor、LCD IO/面板对象、SPI队列、LVGL锁/TCB/port对象、1920B×2绘图DMA及中断管理对象。不能把主题样式迁移规则套用于这些对象。
- index24..26：动画锁/TCB和expression queue。
- index28、29、31、32、42：JPEG/DMA2D中断管理分配，必须沿引擎生命周期释放，不能直接free。
- index30、33..39、41、43、45：SD SPI descriptor、驱动控制、队列、FATFS挂载对象/锁和576B DMA bounce buffer。
- index40、44：newlib锁，具体业务归属超过trace深度，未知。
- index46/47：camera_photo队列/TCB；index48/49：home_info锁/TCB。
- index50/51：video_streamer两个worker TCB；index52：webrtc_whip控制任务TCB。TCB必须INTERNAL，不能迁PSRAM；此前largest未跨TLSF档位不能等同于完全不占用连续区，但本轮仍不优先调整它们。
- index4、8、10、19、27共5块owner未知；最短suffix包含其中8、10、19、27。不能假定为I2S对齐填充或任意释放。

## 本次建议

1. **不实施“只关闭I2S即可解决”**：当前最大FREE左邻是RTC控制TCB，其他LCD/SD/JPEG/任务对象也继续阻断。音频DMA回收只证明总量变化，不能保证最大块合并。
2. 不实施批量直接释放46块：该集合包括RTC工作必需对象及未知块。CPU-only对象可研究PSRAM，TCB、DMA和中断对象不能盲迁。
3. 继续方向应是审计H264真实编码器能否在启动阶段、这些长期驱动对象之前创建，再评估运行期安全余量和完整释放/恢复方案；这是创建顺序设计候选，不是guard占位，也尚未获准实施。提前存在的真实REF会占用约92KB，必须验证语音/显示/SD/USB随后初始化能否成功，不能默认可行。
4. 如果坚持按当前运行时布局释放后缀，必须先定义多个模块一致停机/重建方案，明显不是单一owner最小patch；本次证据不足以推荐这样做。

FRAG快照live173/bytes51168，稍后heap summary为176个allocated blocks；这是不同时间点，中间打印约2秒有并发分配，不能再写成两者完全一致。complete=1说明快照未截断，不代表打印期间heap冻结。H264仍NO_MEM，未进入STREAMING。

## 结论和本轮范围

H264失败仍为连续INTERNAL不足。已知最大原始FREE为 `[0x4ffa3e98, 0x4ffb6ef8)`，77920B，API largest77824B；申请实际92224B。右侧是region边界，仅分析左链。

现有日志没有记录链内部所有allocation。不能通过旧ELF还原运行时未输出的heap内容，也不能用GROUP example填补未知地址。本轮实现整链诊断、保存匹配ELF并解析已知owner；**完整链、精确最短块数及所有内部owner仍待新诊断的实机日志**。不改业务代码、不释放资源、不调整任何音视频参数。

## 已有证据及ELF

- 输入：`logs/h264_fragment/neighbors_serial.txt`。
- 已重新addr2line：`logs/h264_fragment/neighbors_serial_owners.md`。
- 保存对应ELF：`logs/h264_fragment/backward_baseline/lummiss_main.elf`。
- SHA256：`C4CAF03B148599F1A55A2A07F5065C29209FB146E5F7BF1218386A5745EFF4D6`，匹配neighbors_manifest；用户片段无启动ELF摘要，板端身份以烧录该构建为前提。
- 新诊断重新链接后PC可能变化，必须使用新构建ELF，不能沿用baseline。

## 已知链布局（不是完整链）

| state | address | payload | span | owner |
|---|---|---:|---:|---|
| FREE | 0x4ff9b04c（由前一块地址、payload和已验证4B间隔推导） | 17904 | 到下一allocation为17908 | 前置free，待直接walk输出核验 |
| ALLOC | 0x4ff9f640 | 1920 | 未知 | I2S RX DMA数据buffer |
| ALLOC ... | 链内部未完整输出 | 未知 | 未知 | 禁止从大小猜测 |
| ALLOC | 0x4ffa1c00 | 1920 | 未知 | LCD DMA绘图buf1 |
| ALLOC | 0x4ffa23c0 | 1920 | 未知 | LCD DMA绘图buf2 |
| ALLOC | 0x4ffa3300 | 576 | 未知 | SDSPI bounce buffer |
| ALLOC ... | 此处仍可能存在其他块 | 未知 | 未知 | 待实测 |
| ALLOC | 0x4ffa3ca4 | 92 | 96 | newlib mutex |
| ALLOC | 0x4ffa3d04 | 28 | 32 | JPEG/DMA2D共享中断描述 |
| ALLOC | 0x4ffa3d24 | 368 | 372 | webrtc_whip任务TCB |
| FREE | 0x4ffa3e98 | 77920 | 77920（到payload末端） | 最大free，右端0x4ffb6ef8 |

链跨度为18520B。当前所有EDGE表明两块FREE之间没有其他FREE，但EDGE不能列出连续allocation内部所有对象。

## cumulative reclaim：已有地址能支持的计算

按地址计算，range_end固定为0x4ffb6ef8（不包含）；new_contiguous=end-start，包含相邻块合并时回收的metadata空间。reclaimed_span为相比现有77920B增加的连续跨度。

| blocks_removed | range_start | range_end | reclaimed_span | new_contiguous_size | >=92224 | >=98304 |
|---:|---|---|---:|---:|---|---|
| 1 | 0x4ffa3d24 | 0x4ffb6ef8 | 372 | 78292 | 否 | 否 |
| 2 | 0x4ffa3d04 | 0x4ffb6ef8 | 404 | 78324 | 否 | 否 |
| 3 | 0x4ffa3ca4 | 0x4ffb6ef8 | 500 | 78420 | 否 | 否 |
| 完整链，块数待测 | 0x4ff9f640 | 0x4ffb6ef8 | 18520 | 96440（尚未计左侧FREE） | 是 | 否 |
| 完整链并合并左侧FREE | 0x4ff9b04c（推导） | 0x4ffb6ef8 | 36428 | 114348 | 是 | 是 |

92224B物理阈值对应起点<=`0x4ffa06b8`，具体最短suffix需找到第一个向左跨过该地址的实际allocation边界，当前未知。

98304B物理阈值对应起点<=`0x4ff9eef8`，已小于整条allocation链起点。因此在当前布局下，达到96KiB必须处理整个链并合并左侧FREE，不能只处理链末端小对象。该集合已知含I2S RX、LCD DMA、SDSPI、JPEG中断、TCB等多个owner，不是单独I2S类别。

这些是理想释放后的物理连续跨度，不是承诺分配必定成功。TLSF fit档位、CACHE_ALIGNED/16B对齐、同时存活的新分配都可能影响实际申请。不能用物理>=92224直接宣称H264成功。

## 已知allocation owner及处理分类

地址与源码行来自上述匹配ELF的addr2line；caps来自源码调用点，未取到的字段明确保留未知。

| address / payload | caller / file:line | 用途、生命周期 | caps | DMA_required | INTERNAL_required | RTC_needed | PSRAM_possible | can_delay | can_release_on_RTC |
|---|---|---|---|---|---|---|---|---|---|
| 4ff9f640 /1920 | i2s_alloc_dma_desc，IDF esp_driver_i2s/i2s_common.c:540；上游i2s_tdm_set_slot/init_tdm_mode/audio_hw_init | RX DMA数据，channel生命周期 | I2S DMA分配caps | 是 | 是，当前驱动 | audio=false功能不需音频，但当前采集仍运行 | 否，不能盲迁 | 常态语音需要，不能简单推迟到RTC | 必须先完整停止读写并销毁双通道；条件方案 |
| 4ffa1c00 /1920 | lvgl_port_add_disp_priv，managed_components/espressif__esp_lvgl_port/src/lvgl8/esp_lvgl_port_disp.c:348 | LCD buf1，显示生命周期 | MALLOC_CAP_DMA | 是 | 当前路径保留 | 是，RTC仍允许事件刷新 | 当前不迁 | 启动UI依赖 | 否，未设计显示停用协议 |
| 4ffa23c0 /1920 | 同上:351 | LCD buf2 | 同上 | 是 | 当前路径保留 | 是 | 当前不迁 | 同上 | 同上 |
| 4ffa3300 /576 | get_block_buf，components/esp_driver_sdspi/src/sdspi_host.c:288 | SPI bounce buffer，SD slot生命周期 | MALLOC_CAP_DMA | 是 | 当前路径保留 | 需核对所有SD使用者 | 否 | 首次SD读写需要 | 只有先停止全部SD用户并正常卸载，不能直接free |
| 4ffa3ca4 /92 | lock_init_generic/lock_acquire_generic，IDF newlib/src/locks.c:76/134；xQueueCreateMutex | newlib惰性锁；具体业务owner和寿命未知 | pvPortMalloc INTERNAL/8BIT | 无DMA证据 | 按FreeRTOS控制对象规则保留 | 未知 | 不直接迁 | 未知 | 不手动free |
| 4ffa3d04 /28 | esp_intr_alloc_intrstatus_bind，IDF esp_hw_support/intr_alloc.c:611；dma/dma2d.c:441 → jpeg_decode.c:119 → video_streamer.c:2030 | 共享中断描述，JPEG引擎生命周期 | INTERNAL/8BIT | 不是帧buffer | 是 | JPEG解码需要 | 否 | 可设计JPEG引擎晚于H264，收益仅32B，非当前修复建议 | 不能在引擎使用中直接free |
| 4ffa3d24 /368 | xTaskCreatePinnedToCoreWithCaps，IDF freertos/esp_additions/idf_additions.c:46 → webrtc_whip.c:1552 | RTC控制任务TCB；栈12288B已PSRAM | pvPortMalloc INTERNAL/8BIT | 否 | 是，IDF明确要求 | 是 | 否 | 当前由该任务启动H264，不能简单后移 | RTC启动过程不能删除自己来腾块 |
| 其余链内allocation | 未输出 | 未知 | 未知 | 未知 | 未知 | 未知 | 未知 | 未知 | 未知 |

11520B/6块GROUP已由调用栈确认I2S TDM RX数据buffer，3072B/6块组来自I2S STD TX数据buffer。函数名i2s_alloc_dma_desc同时管理descriptor和数据buffer，540行对应数据buffer，不把所有分配混称descriptor；当前链内各个实例还需BACKWARD_BLOCK列全。

## BACKWARD_CHAIN 诊断实现

只改 `components/board/mem_fragment.c` 与 `tools/decode_fragment_owners.py`：

1. 最大free向左扫描到前一FREE或region边界，包含两端FREE，按升序逐块输出BACKWARD_BLOCK。
2. 输出index、state、address、payload_size、block_span、pcs。span采用下一块payload地址减当前地址，最后一块截止free payload末端。
3. 每个suffix输出BACKWARD_RECLAIM；完整删除链时自动包括左侧FREE的合并。
4. 输出达到两个物理阈值的最短块数；zero表示未达到。快照dropped非零时complete=0，不能据此下完整链结论。
5. 使用现有PSRAM快照，无新INTERNAL诊断buffer，不在heap锁内打印。仍仅首次H264前输出，不循环刷屏。
6. 解码脚本接受BACKWARD行，使用调用者指定ELF解析所有链内PC。

没有新实机采样，故本报告不能填充未知块或声称已经取得精确最短allocation集合。

## 条件设计：RTC audio=false释放音频硬件

源码 `xiaozhi_audio.c:919` 暂停分支仅释放wake/AFE。采集循环1531行仍先调用esp_codec_dev_read，然后1544行才判断s_rtc_suspended；s_capture_guard不是I2S in-flight屏障。播放路径1778行也需独立排空确认。直接调用audio_hw_cleanup不安全，且该函数1054行还清理Opus、用户buffer、codec接口和I2C总线，范围超出仅回收DMA。

如果完整链证明I2S类别确有必要收益，后续单独方案应为：

1. RTC audio=false才进入停机握手，拒绝新增播放/采集/诊断请求。
2. capture/playback及所有硬件测试、lifecycle访问者确认当前read/write已返回，并阻塞等待恢复；不能在驱动API内强删任务。
3. 停止wake/AFE，等待所有使用者确认停机；失败或超时则取消H264启动，不能继续销毁句柄。
4. 关闭ES7210/ES8311 codec数据路径，按接口依赖解绑数据接口；保留其他共享I2C使用者，不粗暴删除共享总线。
5. RX/TX共享时钟，统一disable两侧并按驱动状态检查结果，再delete两侧，回收DMA数据及descriptor。
6. 打印free/largest；仍不足则此次A/B判无效收益，不连带修改其他owner。
7. 创建H264，正常RTC生命周期运行；停止时先完全关闭编码器释放REF。
8. 重建同样I2S/codec配置，恢复AFE/Wake，确认成功后再放行读写任务。部分失败必须回滚已创建对象，不能发布“恢复成功”。

该方案本轮未实现，且**不能保证只释放I2S就连接最大free**：只要链中还有其他owner存活，仍会阻断合并。

## 最小后续决策

先运行一次本轮诊断，完成最短suffix的逐块owner表。只有CPU-only且可安全管理生命周期的对象才可考虑PSRAM/延后/RTC释放；不能直接操作heap地址。若必要suffix横跨多个必须INTERNAL对象，应明确单owner迁移无法解决，而不是承诺腾出总量就能成功。

不再优化cleanup；不把video task/queue创建顺序列为首要原因；不修改720p、20fps、QP、bitrate、WDT、MMR、AFE、ES7210、增益或resampler。后续资源patch需用户确认，一类资源一次A/B，不提前提交业务修改。

## 构建交付

两个目录 `src/demo/build_rtc_mem_b`、`src/demo/build` 均增量编译通过，未烧录、未提交commit。日志 `logs/h264_fragment/backward_build_rtc_mem_b.log`、`backward_build.log`，新固件哈希 `backward_manifest.json`。使用IDF扩展原build_rtc_mem_b配置烧录后，仅需触发一次RTC，保存BACKWARD_CHAIN_BEGIN到BACKWARD_CHAIN_END整段；下一次以该新ELF解析，补齐本报告未知项。


## 本次完整链与逐块源码（自动整理）

完整PC及内联栈见 logs/h264_fragment/backward_serial_owners.md。unknown保持未知，不按相邻对象归属。

| index | state | address | payload | span | caller / file:line |
|---:|---|---|---:|---:|---|
| 0 | FREE | 0x4ff9b020 | 17968 | 17972 | FREE |
| 1 | ALLOC | 0x4ff9f654 | 12 | 16 | add_to_keeper at E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_codec_dev/platform/audio_codec_data_i2s.c:114<br>_i2s_data_open at E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_codec_dev/platform/audio_codec_data_i2s.c:543<br>audio_codec_new_i2s_data at E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_codec_dev/platform/audio_codec_data_i2s.c:775<br>audio_hw_init at E:/Lummiss_Plant_Robot/src/demo/components/xiaozhi_audio/xiaozhi_audio.c:1222 (discriminator 1) |
| 2 | ALLOC | 0x4ff9f664 | 24 | 28 | audio_codec_new_gpio at E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_codec_dev/platform/audio_codec_gpio.c:49<br>audio_hw_init at E:/Lummiss_Plant_Robot/src/demo/components/xiaozhi_audio/xiaozhi_audio.c:1232 (discriminator 1)<br>service_task at E:/Lummiss_Plant_Robot/src/demo/components/xiaozhi_audio/xiaozhi_audio.c:2161 (discriminator 1) |
| 3 | ALLOC | 0x4ff9f680 | 1920 | 1924 | i2s_alloc_dma_desc at D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_i2s/i2s_common.c:540 (discriminator 1)<br>i2s_tdm_set_slot at D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_i2s/i2s_tdm.c:150<br>i2s_channel_init_tdm_mode at D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_i2s/i2s_tdm.c:309<br>audio_hw_init at E:/Lummiss_Plant_Robot/src/demo/components/xiaozhi_audio/xiaozhi_audio.c:1197 |
| 4 | ALLOC | 0x4ff9fe04 | 56 | 60 | 未知 |
| 5 | ALLOC | 0x4ff9fe40 | 1920 | 1924 | i2s_alloc_dma_desc at D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_i2s/i2s_common.c:540 (discriminator 1)<br>i2s_tdm_set_slot at D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_i2s/i2s_tdm.c:150<br>i2s_channel_init_tdm_mode at D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_i2s/i2s_tdm.c:309<br>audio_hw_init at E:/Lummiss_Plant_Robot/src/demo/components/xiaozhi_audio/xiaozhi_audio.c:1197 |
| 6 | ALLOC | 0x4ffa05c4 | 56 | 60 | esp_codec_dev_new at E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_codec_dev/esp_codec_dev.c:133<br>audio_hw_init at E:/Lummiss_Plant_Robot/src/demo/components/xiaozhi_audio/xiaozhi_audio.c:1314 (discriminator 1)<br>service_task at E:/Lummiss_Plant_Robot/src/demo/components/xiaozhi_audio/xiaozhi_audio.c:2161 (discriminator 1) |
| 7 | ALLOC | 0x4ffa0600 | 1920 | 1924 | i2s_alloc_dma_desc at D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_i2s/i2s_common.c:540 (discriminator 1)<br>i2s_tdm_set_slot at D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_i2s/i2s_tdm.c:150<br>i2s_channel_init_tdm_mode at D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_i2s/i2s_tdm.c:309<br>audio_hw_init at E:/Lummiss_Plant_Robot/src/demo/components/xiaozhi_audio/xiaozhi_audio.c:1197 |
| 8 | ALLOC | 0x4ffa0d84 | 56 | 60 | 未知 |
| 9 | ALLOC | 0x4ffa0dc0 | 1920 | 1924 | i2s_alloc_dma_desc at D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_i2s/i2s_common.c:540 (discriminator 1)<br>i2s_tdm_set_slot at D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_i2s/i2s_tdm.c:150<br>i2s_channel_init_tdm_mode at D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_i2s/i2s_tdm.c:309<br>audio_hw_init at E:/Lummiss_Plant_Robot/src/demo/components/xiaozhi_audio/xiaozhi_audio.c:1197 |
| 10 | ALLOC | 0x4ffa1544 | 56 | 60 | 未知 |
| 11 | ALLOC | 0x4ffa1580 | 64 | 68 | spicommon_dma_desc_alloc at D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_spi/src/gpspi/spi_common.c:311<br>spi_bus_initialize at D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_spi/src/gpspi/spi_common.c:862<br>lcd_initialize at E:/Lummiss_Plant_Robot/src/demo/main/display_driver.c:243 (discriminator 1)<br>display_driver_start at E:/Lummiss_Plant_Robot/src/demo/main/display_driver.c:575 |
| 12 | ALLOC | 0x4ffa15c4 | 544 | 548 | esp_lcd_new_panel_io_spi at D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_lcd/spi/esp_lcd_panel_io_spi.c:78<br>lcd_initialize at E:/Lummiss_Plant_Robot/src/demo/main/display_driver.c:254 (discriminator 1)<br>display_driver_start at E:/Lummiss_Plant_Robot/src/demo/main/display_driver.c:575<br>ui_task at E:/Lummiss_Plant_Robot/src/demo/main/main.c:247 |
| 13 | ALLOC | 0x4ffa17e8 | 216 | 220 | xQueueGenericCreate at D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/freertos/FreeRTOS-Kernel/queue.c:545<br>spi_bus_add_device at D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_spi/src/gpspi/spi_master.c:537 (discriminator 1)<br>esp_lcd_new_panel_io_spi at D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_lcd/spi/esp_lcd_panel_io_spi.c:95<br>lcd_initialize at E:/Lummiss_Plant_Robot/src/demo/main/display_driver.c:254 (discriminator 1) |
| 14 | ALLOC | 0x4ffa18c4 | 216 | 220 | xQueueGenericCreate at D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/freertos/FreeRTOS-Kernel/queue.c:545<br>spi_bus_add_device at D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_spi/src/gpspi/spi_master.c:543 (discriminator 1)<br>esp_lcd_new_panel_io_spi at D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_lcd/spi/esp_lcd_panel_io_spi.c:95<br>lcd_initialize at E:/Lummiss_Plant_Robot/src/demo/main/display_driver.c:254 (discriminator 1) |
| 15 | ALLOC | 0x4ffa19a0 | 76 | 80 | esp_lcd_new_panel_st7789 at D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_lcd/src/esp_lcd_panel_st7789.c:73<br>lcd_initialize at E:/Lummiss_Plant_Robot/src/demo/main/display_driver.c:263 (discriminator 1)<br>display_driver_start at E:/Lummiss_Plant_Robot/src/demo/main/display_driver.c:575<br>ui_task at E:/Lummiss_Plant_Robot/src/demo/main/main.c:247 |
| 16 | ALLOC | 0x4ffa19f0 | 92 | 96 | xQueueGenericCreate at D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/freertos/FreeRTOS-Kernel/queue.c:545<br>xQueueCreateMutex at D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/freertos/FreeRTOS-Kernel/queue.c:669<br>lvgl_port_init at E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_lvgl_port/src/lvgl8/esp_lvgl_port.c:72 (discriminator 1)<br>lvgl_initialize at E:/Lummiss_Plant_Robot/src/demo/main/display_driver.c:330 (discriminator 1) |
| 17 | ALLOC | 0x4ffa1a50 | 368 | 372 | xTaskCreatePinnedToCoreWithCaps at D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/freertos/esp_additions/idf_additions.c:46<br>lvgl_port_init at E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_lvgl_port/src/lvgl8/esp_lvgl_port.c:85<br>lvgl_initialize at E:/Lummiss_Plant_Robot/src/demo/main/display_driver.c:330 (discriminator 1)<br>display_driver_start at E:/Lummiss_Plant_Robot/src/demo/main/display_driver.c:576 |
| 18 | ALLOC | 0x4ffa1bc4 | 116 | 120 | lvgl_port_add_disp_priv at E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_lvgl_port/src/lvgl8/esp_lvgl_port_disp.c:279<br>lvgl_port_add_disp at E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_lvgl_port/src/lvgl8/esp_lvgl_port_disp.c:92<br>lvgl_initialize at E:/Lummiss_Plant_Robot/src/demo/main/display_driver.c:351<br>display_driver_start at E:/Lummiss_Plant_Robot/src/demo/main/display_driver.c:576 |
| 19 | ALLOC | 0x4ffa1c3c | 64 | 68 | 未知 |
| 20 | ALLOC | 0x4ffa1c80 | 1920 | 1924 | lvgl_port_add_disp_priv at E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_lvgl_port/src/lvgl8/esp_lvgl_port_disp.c:348 (discriminator 1)<br>lvgl_port_add_disp at E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_lvgl_port/src/lvgl8/esp_lvgl_port_disp.c:92<br>lvgl_initialize at E:/Lummiss_Plant_Robot/src/demo/main/display_driver.c:351<br>display_driver_start at E:/Lummiss_Plant_Robot/src/demo/main/display_driver.c:576<br>ui_task at E:/Lummiss_Plant_Robot/src/demo/main/main.c:247 |
| 21 | ALLOC | 0x4ffa2404 | 28 | 32 | lvgl_port_add_disp_priv at E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_lvgl_port/src/lvgl8/esp_lvgl_port_disp.c:356<br>lvgl_port_add_disp at E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_lvgl_port/src/lvgl8/esp_lvgl_port_disp.c:92<br>lvgl_initialize at E:/Lummiss_Plant_Robot/src/demo/main/display_driver.c:351<br>display_driver_start at E:/Lummiss_Plant_Robot/src/demo/main/display_driver.c:576 |
| 22 | ALLOC | 0x4ffa2424 | 24 | 28 | get_desc_for_int at D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_hw_support/intr_alloc.c:172<br>esp_intr_alloc_intrstatus_bind at D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_hw_support/intr_alloc.c:601<br>esp_intr_alloc_intrstatus at D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_hw_support/intr_alloc.c:718<br>esp_intr_alloc at D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_hw_support/intr_alloc.c:729<br>spi_master_init_driver at D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_spi/src/gpspi/spi_master.c:331 (discriminator 2) |
| 23 | ALLOC | 0x4ffa2440 | 1920 | 1924 | lvgl_port_add_disp_priv at E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_lvgl_port/src/lvgl8/esp_lvgl_port_disp.c:351 (discriminator 1)<br>lvgl_port_add_disp at E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_lvgl_port/src/lvgl8/esp_lvgl_port_disp.c:92<br>lvgl_initialize at E:/Lummiss_Plant_Robot/src/demo/main/display_driver.c:351<br>display_driver_start at E:/Lummiss_Plant_Robot/src/demo/main/display_driver.c:576<br>ui_task at E:/Lummiss_Plant_Robot/src/demo/main/main.c:247 |
| 24 | ALLOC | 0x4ffa2bc4 | 92 | 96 | xQueueGenericCreate at D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/freertos/FreeRTOS-Kernel/queue.c:545<br>xQueueCreateMutex at D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/freertos/FreeRTOS-Kernel/queue.c:669<br>anim_bin_player_init at E:/Lummiss_Plant_Robot/src/demo/components/anim_bin_player/anim_bin_player.c:683 (discriminator 1)<br>expression_manager_init at E:/Lummiss_Plant_Robot/src/demo/main/expression_manager.c:182 |
| 25 | ALLOC | 0x4ffa2c24 | 368 | 372 | xTaskCreatePinnedToCoreWithCaps at D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/freertos/esp_additions/idf_additions.c:46<br>anim_bin_player_init at E:/Lummiss_Plant_Robot/src/demo/components/anim_bin_player/anim_bin_player.c:702<br>expression_manager_init at E:/Lummiss_Plant_Robot/src/demo/main/expression_manager.c:182<br>ui_task at E:/Lummiss_Plant_Robot/src/demo/main/main.c:247 (discriminator 1) |
| 26 | ALLOC | 0x4ffa2d98 | 92 | 96 | xQueueGenericCreate at D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/freertos/FreeRTOS-Kernel/queue.c:545<br>expression_manager_init at E:/Lummiss_Plant_Robot/src/demo/main/expression_manager.c:190 (discriminator 1)<br>ui_task at E:/Lummiss_Plant_Robot/src/demo/main/main.c:247 (discriminator 1) |
| 27 | ALLOC | 0x4ffa2df8 | 100 | 104 | 未知 |
| 28 | ALLOC | 0x4ffa2e60 | 12 | 16 | esp_intr_alloc_intrstatus_bind at D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_hw_support/intr_alloc.c:574<br>esp_intr_alloc_intrstatus at D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_hw_support/intr_alloc.c:718<br>dma2d_acquire_pool at D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_hw_support/dma/dma2d.c:441<br>jpeg_new_decoder_engine at D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_jpeg/jpeg_decode.c:119<br>video_codec_task at E:/Lummiss_Plant_Robot/src/demo/components/video_streamer/video_streamer.c:2030 (discriminator 1) |
| 29 | ALLOC | 0x4ffa2e70 | 12 | 16 | esp_intr_alloc_intrstatus_bind at D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_hw_support/intr_alloc.c:574<br>esp_intr_alloc_intrstatus at D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_hw_support/intr_alloc.c:718<br>dma2d_acquire_pool at D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_hw_support/dma/dma2d.c:441<br>jpeg_new_decoder_engine at D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_jpeg/jpeg_decode.c:119<br>video_codec_task at E:/Lummiss_Plant_Robot/src/demo/components/video_streamer/video_streamer.c:2030 (discriminator 1) |
| 30 | ALLOC | 0x4ffa2e80 | 64 | 68 | spicommon_dma_desc_alloc at D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_spi/src/gpspi/spi_common.c:310 (discriminator 1)<br>spi_bus_initialize at D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_spi/src/gpspi/spi_common.c:862<br>sd_card_mount at E:/Lummiss_Plant_Robot/src/demo/components/sd_card/sd_card_spi.c:375<br>app_main at E:/Lummiss_Plant_Robot/src/demo/main/main.c:791 |
| 31 | ALLOC | 0x4ffa2ec4 | 24 | 28 | esp_intr_alloc_intrstatus_bind at D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_hw_support/intr_alloc.c:611<br>esp_intr_alloc_intrstatus at D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_hw_support/intr_alloc.c:718<br>dma2d_acquire_pool at D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_hw_support/dma/dma2d.c:441<br>jpeg_new_decoder_engine at D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_jpeg/jpeg_decode.c:119<br>video_codec_task at E:/Lummiss_Plant_Robot/src/demo/components/video_streamer/video_streamer.c:2030 (discriminator 1) |
| 32 | ALLOC | 0x4ffa2ee0 | 28 | 32 | esp_intr_alloc_intrstatus_bind at D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_hw_support/intr_alloc.c:611<br>esp_intr_alloc_intrstatus at D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_hw_support/intr_alloc.c:718<br>dma2d_acquire_pool at D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_hw_support/dma/dma2d.c:441<br>jpeg_new_decoder_engine at D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_jpeg/jpeg_decode.c:119<br>video_codec_task at E:/Lummiss_Plant_Robot/src/demo/components/video_streamer/video_streamer.c:2030 (discriminator 1) |
| 33 | ALLOC | 0x4ffa2f00 | 64 | 68 | spicommon_dma_desc_alloc at D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_spi/src/gpspi/spi_common.c:311<br>spi_bus_initialize at D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_spi/src/gpspi/spi_common.c:862<br>sd_card_mount at E:/Lummiss_Plant_Robot/src/demo/components/sd_card/sd_card_spi.c:375<br>app_main at E:/Lummiss_Plant_Robot/src/demo/main/main.c:791 |
| 34 | ALLOC | 0x4ffa2f44 | 200 | 204 | mount_prepare_mem at D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/fatfs/vfs/vfs_fat_sdmmc.c:96<br>esp_vfs_fat_sdspi_mount at D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/fatfs/vfs/vfs_fat_sdmmc.c:349<br>sd_card_mount at E:/Lummiss_Plant_Robot/src/demo/components/sd_card/sd_card_spi.c:397<br>app_main at E:/Lummiss_Plant_Robot/src/demo/main/main.c:791 |
| 35 | ALLOC | 0x4ffa3010 | 136 | 140 | spi_master_init_driver at D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_spi/src/gpspi/spi_master.c:302<br>spi_bus_add_device at D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_spi/src/gpspi/spi_master.c:448<br>configure_spi_dev at E:/Lummiss_Plant_Robot/src/demo/components/esp_driver_sdspi/src/sdspi_host.c:348<br>sdspi_host_init_device at E:/Lummiss_Plant_Robot/src/demo/components/esp_driver_sdspi/src/sdspi_host.c:495<br>init_sdspi_host at D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/fatfs/vfs/vfs_fat_sdmmc.c:322 |
| 36 | ALLOC | 0x4ffa309c | 92 | 96 | xSemaphoreCreateGenericWithCaps at D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/freertos/esp_additions/idf_additions.c:300<br>spi_bus_lock_register_dev at D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_hw_support/spi_bus_lock.c:685 (discriminator 1)<br>spi_bus_add_device at D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_spi/src/gpspi/spi_master.c:492<br>configure_spi_dev at E:/Lummiss_Plant_Robot/src/demo/components/esp_driver_sdspi/src/sdspi_host.c:348<br>sdspi_host_set_card_clk at E:/Lummiss_Plant_Robot/src/demo/components/esp_driver_sdspi/src/sdspi_host.c:432 |
| 37 | ALLOC | 0x4ffa30fc | 124 | 128 | spi_bus_add_device at D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_spi/src/gpspi/spi_master.c:527<br>configure_spi_dev at E:/Lummiss_Plant_Robot/src/demo/components/esp_driver_sdspi/src/sdspi_host.c:348<br>sdspi_host_set_card_clk at E:/Lummiss_Plant_Robot/src/demo/components/esp_driver_sdspi/src/sdspi_host.c:432<br>sdmmc_init_host_frequency at D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/sdmmc/sdmmc_common.c:248<br>sdmmc_card_init at D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/sdmmc/sdmmc_init.c:166 (discriminator 1) |
| 38 | ALLOC | 0x4ffa317c | 144 | 148 | xQueueGenericCreate at D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/freertos/FreeRTOS-Kernel/queue.c:545<br>spi_bus_add_device at D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_spi/src/gpspi/spi_master.c:537 (discriminator 1)<br>configure_spi_dev at E:/Lummiss_Plant_Robot/src/demo/components/esp_driver_sdspi/src/sdspi_host.c:348<br>sdspi_host_set_card_clk at E:/Lummiss_Plant_Robot/src/demo/components/esp_driver_sdspi/src/sdspi_host.c:432 |
| 39 | ALLOC | 0x4ffa3210 | 144 | 148 | xQueueGenericCreate at D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/freertos/FreeRTOS-Kernel/queue.c:545<br>spi_bus_add_device at D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_spi/src/gpspi/spi_master.c:543 (discriminator 1)<br>configure_spi_dev at E:/Lummiss_Plant_Robot/src/demo/components/esp_driver_sdspi/src/sdspi_host.c:348<br>sdspi_host_set_card_clk at E:/Lummiss_Plant_Robot/src/demo/components/esp_driver_sdspi/src/sdspi_host.c:432 |
| 40 | ALLOC | 0x4ffa32a4 | 92 | 96 | xQueueGenericCreate at D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/freertos/FreeRTOS-Kernel/queue.c:545<br>xQueueCreateMutex at D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/freertos/FreeRTOS-Kernel/queue.c:669<br>lock_init_generic at D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/newlib/src/locks.c:76<br>lock_acquire_generic at D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/newlib/src/locks.c:134 |
| 41 | ALLOC | 0x4ffa3304 | 36 | 40 | esp_vfs_fat_sdspi_mount at D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/fatfs/vfs/vfs_fat_sdmmc.c:393<br>sd_card_mount at E:/Lummiss_Plant_Robot/src/demo/components/sd_card/sd_card_spi.c:397<br>app_main at E:/Lummiss_Plant_Robot/src/demo/main/main.c:791<br>main_task at D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/freertos/app_startup.c:216 (discriminator 10) |
| 42 | ALLOC | 0x4ffa332c | 16 | 20 | get_desc_for_int at D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_hw_support/intr_alloc.c:172<br>esp_intr_alloc_intrstatus_bind at D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_hw_support/intr_alloc.c:601<br>esp_intr_alloc_intrstatus at D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_hw_support/intr_alloc.c:718<br>jpeg_isr_register at D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_jpeg/jpeg_common.c:138<br>jpeg_new_decoder_engine at D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_jpeg/jpeg_decode.c:106 |
| 43 | ALLOC | 0x4ffa3340 | 576 | 580 | get_block_buf at E:/Lummiss_Plant_Robot/src/demo/components/esp_driver_sdspi/src/sdspi_host.c:288 (discriminator 1)<br>start_command_read_blocks at E:/Lummiss_Plant_Robot/src/demo/components/esp_driver_sdspi/src/sdspi_host.c:1123<br>sdspi_host_start_command at E:/Lummiss_Plant_Robot/src/demo/components/esp_driver_sdspi/src/sdspi_host.c:683<br>sdspi_host_do_transaction at D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_sdspi/src/sdspi_transaction.c:153 (discriminator 1)<br>trace_transaction at E:/Lummiss_Plant_Robot/src/demo/components/sd_card/sd_card_spi.c:158 |
| 44 | ALLOC | 0x4ffa3584 | 92 | 96 | xQueueGenericCreate at D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/freertos/FreeRTOS-Kernel/queue.c:545<br>xQueueCreateMutex at D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/freertos/FreeRTOS-Kernel/queue.c:669<br>lock_init_generic at D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/newlib/src/locks.c:76<br>_lock_init at D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/newlib/src/locks.c:88 |
| 45 | ALLOC | 0x4ffa35e4 | 92 | 96 | xQueueGenericCreate at D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/freertos/FreeRTOS-Kernel/queue.c:545<br>xQueueCreateMutex at D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/freertos/FreeRTOS-Kernel/queue.c:669<br>ff_mutex_create at D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/fatfs/port/freertos/ffsystem.c:105 (discriminator 1)<br>f_mount at D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/fatfs/src/ff.c:3706 (discriminator 1) |
| 46 | ALLOC | 0x4ffa3644 | 108 | 112 | xQueueGenericCreate at D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/freertos/FreeRTOS-Kernel/queue.c:545<br>camera_photo_init at E:/Lummiss_Plant_Robot/src/demo/components/camera_photo/camera_photo.c:225 (discriminator 1)<br>app_main at E:/Lummiss_Plant_Robot/src/demo/main/main.c:818<br>main_task at D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/freertos/app_startup.c:216 (discriminator 10) |
| 47 | ALLOC | 0x4ffa36b4 | 368 | 372 | xTaskCreatePinnedToCoreWithCaps at D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/freertos/esp_additions/idf_additions.c:46<br>camera_photo_init at E:/Lummiss_Plant_Robot/src/demo/components/camera_photo/camera_photo.c:226 (discriminator 1)<br>app_main at E:/Lummiss_Plant_Robot/src/demo/main/main.c:818<br>main_task at D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/freertos/app_startup.c:216 (discriminator 10) |
| 48 | ALLOC | 0x4ffa3828 | 92 | 96 | xQueueGenericCreate at D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/freertos/FreeRTOS-Kernel/queue.c:545<br>xQueueCreateMutex at D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/freertos/FreeRTOS-Kernel/queue.c:669<br>home_info_start at E:/Lummiss_Plant_Robot/src/demo/components/home_info/home_info.c:352 (discriminator 1)<br>app_main at E:/Lummiss_Plant_Robot/src/demo/main/main.c:827 |
| 49 | ALLOC | 0x4ffa3888 | 368 | 372 | xTaskCreatePinnedToCoreWithCaps at D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/freertos/esp_additions/idf_additions.c:46<br>home_info_start at E:/Lummiss_Plant_Robot/src/demo/components/home_info/home_info.c:372<br>app_main at E:/Lummiss_Plant_Robot/src/demo/main/main.c:827<br>main_task at D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/freertos/app_startup.c:216 (discriminator 10) |
| 50 | ALLOC | 0x4ffa39fc | 368 | 372 | xTaskCreatePinnedToCoreWithCaps at D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/freertos/esp_additions/idf_additions.c:46<br>video_streamer_init at E:/Lummiss_Plant_Robot/src/demo/components/video_streamer/video_streamer.c:2696 (discriminator 1)<br>rtc_prepare_resources at E:/Lummiss_Plant_Robot/src/demo/main/main.c:280<br>controller_task at E:/Lummiss_Plant_Robot/src/demo/components/webrtc_whip/webrtc_whip.c:1346 (discriminator 2) |
| 51 | ALLOC | 0x4ffa3b70 | 380 | 384 | xTaskCreatePinnedToCoreWithCaps at D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/freertos/esp_additions/idf_additions.c:46<br>video_streamer_init at E:/Lummiss_Plant_Robot/src/demo/components/video_streamer/video_streamer.c:2777<br>rtc_prepare_resources at E:/Lummiss_Plant_Robot/src/demo/main/main.c:280<br>controller_task at E:/Lummiss_Plant_Robot/src/demo/components/webrtc_whip/webrtc_whip.c:1346 (discriminator 2) |
| 52 | ALLOC | 0x4ffa3cf0 | 368 | 372 | xTaskCreatePinnedToCoreWithCaps at D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/freertos/esp_additions/idf_additions.c:46<br>webrtc_whip_request_start at E:/Lummiss_Plant_Robot/src/demo/components/webrtc_whip/webrtc_whip.c:1552 (discriminator 1)<br>tool_webrtc_start at E:/Lummiss_Plant_Robot/src/demo/components/cloud_mqtt/cloud_mcp.c:594<br>handle_tools_call at E:/Lummiss_Plant_Robot/src/demo/components/cloud_mqtt/cloud_mcp.c:818 (discriminator 10) |
| 53 | FREE | 0x4ffa3e64 | 77972 | 77972 | FREE |

## 本次完整累计回收表

仅为理想物理合并跨度，不等于TLSF/对齐后可成功申请值。

| removed | start | end | reclaimed_span | new_contiguous | >=92224 | >=98304 |
|---:|---|---|---:|---:|---|---|
| 1 | 4ffa3cf0 | 4ffb6ef8 | 372 | 78344 | 0 | 0 |
| 2 | 4ffa3b70 | 4ffb6ef8 | 756 | 78728 | 0 | 0 |
| 3 | 4ffa39fc | 4ffb6ef8 | 1128 | 79100 | 0 | 0 |
| 4 | 4ffa3888 | 4ffb6ef8 | 1500 | 79472 | 0 | 0 |
| 5 | 4ffa3828 | 4ffb6ef8 | 1596 | 79568 | 0 | 0 |
| 6 | 4ffa36b4 | 4ffb6ef8 | 1968 | 79940 | 0 | 0 |
| 7 | 4ffa3644 | 4ffb6ef8 | 2080 | 80052 | 0 | 0 |
| 8 | 4ffa35e4 | 4ffb6ef8 | 2176 | 80148 | 0 | 0 |
| 9 | 4ffa3584 | 4ffb6ef8 | 2272 | 80244 | 0 | 0 |
| 10 | 4ffa3340 | 4ffb6ef8 | 2852 | 80824 | 0 | 0 |
| 11 | 4ffa332c | 4ffb6ef8 | 2872 | 80844 | 0 | 0 |
| 12 | 4ffa3304 | 4ffb6ef8 | 2912 | 80884 | 0 | 0 |
| 13 | 4ffa32a4 | 4ffb6ef8 | 3008 | 80980 | 0 | 0 |
| 14 | 4ffa3210 | 4ffb6ef8 | 3156 | 81128 | 0 | 0 |
| 15 | 4ffa317c | 4ffb6ef8 | 3304 | 81276 | 0 | 0 |
| 16 | 4ffa30fc | 4ffb6ef8 | 3432 | 81404 | 0 | 0 |
| 17 | 4ffa309c | 4ffb6ef8 | 3528 | 81500 | 0 | 0 |
| 18 | 4ffa3010 | 4ffb6ef8 | 3668 | 81640 | 0 | 0 |
| 19 | 4ffa2f44 | 4ffb6ef8 | 3872 | 81844 | 0 | 0 |
| 20 | 4ffa2f00 | 4ffb6ef8 | 3940 | 81912 | 0 | 0 |
| 21 | 4ffa2ee0 | 4ffb6ef8 | 3972 | 81944 | 0 | 0 |
| 22 | 4ffa2ec4 | 4ffb6ef8 | 4000 | 81972 | 0 | 0 |
| 23 | 4ffa2e80 | 4ffb6ef8 | 4068 | 82040 | 0 | 0 |
| 24 | 4ffa2e70 | 4ffb6ef8 | 4084 | 82056 | 0 | 0 |
| 25 | 4ffa2e60 | 4ffb6ef8 | 4100 | 82072 | 0 | 0 |
| 26 | 4ffa2df8 | 4ffb6ef8 | 4204 | 82176 | 0 | 0 |
| 27 | 4ffa2d98 | 4ffb6ef8 | 4300 | 82272 | 0 | 0 |
| 28 | 4ffa2c24 | 4ffb6ef8 | 4672 | 82644 | 0 | 0 |
| 29 | 4ffa2bc4 | 4ffb6ef8 | 4768 | 82740 | 0 | 0 |
| 30 | 4ffa2440 | 4ffb6ef8 | 6692 | 84664 | 0 | 0 |
| 31 | 4ffa2424 | 4ffb6ef8 | 6720 | 84692 | 0 | 0 |
| 32 | 4ffa2404 | 4ffb6ef8 | 6752 | 84724 | 0 | 0 |
| 33 | 4ffa1c80 | 4ffb6ef8 | 8676 | 86648 | 0 | 0 |
| 34 | 4ffa1c3c | 4ffb6ef8 | 8744 | 86716 | 0 | 0 |
| 35 | 4ffa1bc4 | 4ffb6ef8 | 8864 | 86836 | 0 | 0 |
| 36 | 4ffa1a50 | 4ffb6ef8 | 9236 | 87208 | 0 | 0 |
| 37 | 4ffa19f0 | 4ffb6ef8 | 9332 | 87304 | 0 | 0 |
| 38 | 4ffa19a0 | 4ffb6ef8 | 9412 | 87384 | 0 | 0 |
| 39 | 4ffa18c4 | 4ffb6ef8 | 9632 | 87604 | 0 | 0 |
| 40 | 4ffa17e8 | 4ffb6ef8 | 9852 | 87824 | 0 | 0 |
| 41 | 4ffa15c4 | 4ffb6ef8 | 10400 | 88372 | 0 | 0 |
| 42 | 4ffa1580 | 4ffb6ef8 | 10468 | 88440 | 0 | 0 |
| 43 | 4ffa1544 | 4ffb6ef8 | 10528 | 88500 | 0 | 0 |
| 44 | 4ffa0dc0 | 4ffb6ef8 | 12452 | 90424 | 0 | 0 |
| 45 | 4ffa0d84 | 4ffb6ef8 | 12512 | 90484 | 0 | 0 |
| 46 | 4ffa0600 | 4ffb6ef8 | 14436 | 92408 | 1 | 0 |
| 47 | 4ffa05c4 | 4ffb6ef8 | 14496 | 92468 | 1 | 0 |
| 48 | 4ff9fe40 | 4ffb6ef8 | 16420 | 94392 | 1 | 0 |
| 49 | 4ff9fe04 | 4ffb6ef8 | 16480 | 94452 | 1 | 0 |
| 50 | 4ff9f680 | 4ffb6ef8 | 18404 | 96376 | 1 | 0 |
| 51 | 4ff9f664 | 4ffb6ef8 | 18432 | 96404 | 1 | 0 |
| 52 | 4ff9b020 | 4ffb6ef8 | 36420 | 114392 | 1 | 1 |
