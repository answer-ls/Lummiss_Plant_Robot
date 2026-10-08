# ESP32-P4 双麦音频链：分层参考改造审计

审计日期：2026-09-28。本轮只读固件源码，新增本报告及 `logs/box_integration_audit/` 参考快照，没有修改、编译或烧录固件。

**结论：保留当前 Codec/I2S 初始化主体、S0/S2/S1 重排和 MMR。首先完成已经加入的 UDP A/B 诊断；若证据支持，再把现有 AFE 输出缓冲的消费、Opus 和 UDP 从采集任务移走。不是重新实现 AFE，也不是照搬某块板。** 当前尚无本轮合格的 A/B 录音，不能把这个架构差异写成已证实的杂音根因。

本板物理映射以用户实测为准：SLOT0=MIC1、SLOT1=硬件 playback REF、SLOT2=MIC2、SLOT3不用。后文所有参考板描述均不覆盖此事实。

## 证据版本与源码入口

本次通过 GitHub API 重新查询 `78/xiaozhi-esp32/main`，得到固定提交 `8ce50d27cd7c72c777673f46cbfc3ef7454d1b5c`，提交时间 `2026-09-26T08:56:17Z`。以下“小智官方”指该仓库版本，不代表 Espressif BSP 或本板实机认证。

| 代号 | 实际核查的源码 |
|---|---|
| B | [BoxAudioCodec](https://github.com/78/xiaozhi-esp32/blob/8ce50d27cd7c72c777673f46cbfc3ef7454d1b5c/main/audio/codecs/box_audio_codec.cc)：构造、CreateDuplexChannels、EnableInput/Output、Read/Write |
| B3 | [BOX-3 config](https://github.com/78/xiaozhi-esp32/blob/8ce50d27cd7c72c777673f46cbfc3ef7454d1b5c/main/boards/espressif/esp32-s3-box-3/config.h)；[板级 GetAudioCodec](https://github.com/78/xiaozhi-esp32/blob/8ce50d27cd7c72c777673f46cbfc3ef7454d1b5c/main/boards/espressif/esp32-s3-box-3/esp_box3_board.cc#L148) |
| L | [BoxAudioCodecLite](https://github.com/78/xiaozhi-esp32/blob/8ce50d27cd7c72c777673f46cbfc3ef7454d1b5c/main/boards/espressif/esp32-s3-box-lite/box_audio_codec_lite.cc)；[头文件](https://github.com/78/xiaozhi-esp32/blob/8ce50d27cd7c72c777673f46cbfc3ef7454d1b5c/main/boards/espressif/esp32-s3-box-lite/box_audio_codec_lite.h)；[Lite config](https://github.com/78/xiaozhi-esp32/blob/8ce50d27cd7c72c777673f46cbfc3ef7454d1b5c/main/boards/espressif/esp32-s3-box-lite/config.h) |
| S | [AudioService](https://github.com/78/xiaozhi-esp32/blob/8ce50d27cd7c72c777673f46cbfc3ef7454d1b5c/main/audio/audio_service.cc)：Start 120附近、ReadAudioData 195、AudioInputTask 242、OpusCodecTask 381、PushTaskToEncodeQueue 569、CheckAndUpdateAudioPowerState 833 |
| E | [AfeAudioEngine](https://github.com/78/xiaozhi-esp32/blob/8ce50d27cd7c72c777673f46cbfc3ef7454d1b5c/main/audio/engines/afe_audio_engine.cc)：Initialize、Feed 252、ProcessingTask 406、HandleVoiceResult 473 |
| A | [Application](https://github.com/78/xiaozhi-esp32/blob/8ce50d27cd7c72c777673f46cbfc3ef7454d1b5c/main/application.cc#L235)：主循环取编码包并 SendAudio |
| W1 | Waveshare 固定提交 `5a7a9a2823d77983c15b44a72934a7699e5d2d8b`：[XiaozhiApp.cpp](https://github.com/waveshareteam/ESP32-P4-WIFI6-Touch-LCD-4B/blob/5a7a9a2823d77983c15b44a72934a7699e5d2d8b/firmware/brookesia/components/XiaozhiApp/XiaozhiApp.cpp)：输入/编码任务1795/1810、inputAudio 2589、encodeAudio 2750 |
| W2 | 同一提交：[XiaozhiAudioProcessor.cpp](https://github.com/waveshareteam/ESP32-P4-WIFI6-Touch-LCD-4B/blob/5a7a9a2823d77983c15b44a72934a7699e5d2d8b/firmware/brookesia/components/XiaozhiApp/XiaozhiAudioProcessor.cpp)：feed 208、AFE配置448、handlePcm 597 |
| P | [当前 xiaozhi_audio.c](E:/Lummiss_Plant_Robot/src/demo/components/xiaozhi_audio/xiaozhi_audio.c:1095)：audio_hw_init、resample_mic_24k_to_16k 1468附近、capture_task 1489、send_wake_preroll 554、wake_process_task 902附近 |
| Q | [当前 wake_word.c](E:/Lummiss_Plant_Robot/src/demo/components/xiaozhi_audio/wake_word.c:126)：handle_voice_result、detection_task、wake_word_init、wake_word_feed 429、wake_word_read_pcm 499附近 |
| D | [当前 codec I2S data_if](E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_codec_dev/platform/audio_codec_data_i2s.c:236)：set_drv_fs、set_fs 409、_i2s_data_enable 555、_i2s_data_read 693附近 |

本次参考下载位于 `logs/box_integration_audit/main/`；当前关键文件与参考文件的 SHA256 记录在 [source_manifest.json](E:/Lummiss_Plant_Robot/logs/box_integration_audit/source_manifest.json)。Waveshare沿用已保存的固定版本，未将不同版本的文件混为一个实现。

版本边界：本工程 IDF 5.5.5 / codec_dev 1.5.11；小智参考 manifest 为 IDF>=6.0.1、codec_dev~1.6.2，不能视为一份锁定的S3实机运行环境。具体驱动版本核查见 [已有底层审计](E:/Lummiss_Plant_Robot/OFFICIAL_ES7210_DIFF.md)。本次直接展开了本地 IDF 的 STD/TDM 默认宏，而非根据宏名推测参数。

## 1. BOX-3 中当前可以直接借鉴的部分

当前已经采用其基本结构：I2S0 一次创建 TX/RX、TX先初始化 STD Philips、RX再初始化四槽 TDM、先启用TX再启用RX；两个 codec 共用 data_if，P4产生时钟，ES8311与ES7210从模式工作。**这些不是待实施改造，而是已经一致的部分。**

### 逐项比较：底层配置

“一致”只表示表内所列层次的配置/逻辑一致；不等于两块板的寄存器、波形或噪声均已实测一致。风险列描述误改的影响。

| 项目 | 参考实现 | 当前实现 | 是否一致 | 是否需要修改 | 理由 | 风险 |
|---|---|---|---|---|---|---|
| i2s_new_channel | B：I2S0/master，一次取得TX/RX | P：同 | 一致 | 否 | 双工共享控制器 | 分拆控制器会改变时钟关系 |
| ES7210 init | B：独立ADC codec，共享data_if，选MIC1/2/3/4 | P：独立ADC，选MIC1/2/3；slave，PAD/256 | 框架一致，使能掩码不同 | 否 | MIC4未用；三个启用输入已进入TDM | 照抄mask会改变通道状态 |
| ES7210 地址 | B：板级传入地址 | P：probe 7-bit 0x40~43，再转codec所需8-bit地址 | 不同、合理 | 否 | 当前接线决定地址 | 复制参考地址可能找错设备 |
| ES7210 gain | B：配置指定MIC及可选REF物理通道增益 | P：两麦30dB；MIC3 REF单独0dB | 不完全一致 | 否 | 物理MIC增益编号与slot编号不同 | 禁止用乘2替代定位 |
| ES8311 init | B：DAC、use_mclk、PA GPIO、5.0/3.3增益补偿 | P：DAC、use_mclk、pa=-1、3.3/3.3零补偿 | codec模式一致，PA策略不同 | 否 | 当前PA CTRL未接GPIO，沿用既有供电补偿决策 | 错误PA配置影响播放/REF |
| I2S TX模式 | B：STD Philips stereo | P：同 | 一致 | 否 | DAC发送方向 | 改TDM发送可能破坏DAC格式 |
| I2S RX模式 | B：TDM Philips stereo配置，mask四槽 | P：同 | 一致 | 否 | ES7210接收方向 | 不能改为双槽MR读取 |
| sample rate | B3：输入/输出24000 | P：输入/输出24000；AFE16000 | codec速率一致 | 否 | AFE速率是后续层 | 不要把Lite的16k直接覆盖总线 |
| data width | B：TX/RX 16bit | P：TX/RX 16bit | 一致 | 否 | PCM解释为int16 | 改int32解释会破坏stride |
| RX slot width | B：AUTO（16bit data对应16bit slot） | P：AUTO，历史回读16 | 初始化一致 | 否 | 四槽64 clocks/frame | 不能只凭data width断言运行值 |
| TX slot width | B：初始化AUTO；随后codec open可协调帧宽 | P：初始化AUTO；共享帧宽协调后历史回读32 | 初始化与协调机制一致；参考实机值未测 | 否 | 2×32与RX 4×16同为64bit/frame | 硬锁TX为16可能打断共享RX |
| slot count | B：RX AUTO+mask0xF得到4；TX STD2 | P：RX显式4；TX STD2 | 有效数量一致 | 否 | 写法不同不是问题 | 把应用3通道当物理3槽会错 |
| RX slot mask | B：I2S层0xF | P：I2S层0xF | 一致 | 否 | 此mask与codec应用选择mask不同 | 两层mask不可混用 |
| codec input mask | B：channel=4，但reference=true时mask=0x3 | P：channel=4、mask=0xF，完整四槽交给应用 | 不同、必须保留 | 否 | 当前需要SLOT2 MIC2 | 改0x3会丢MIC2 |
| MCLK multiple | B：256 | P：256 | 一致 | 否 | 理论24k×256=6.144MHz | 理论值不是仪器实测 |
| MCLK/主从 | B：I2S master、codec slave、共享引脚 | P：同设计，P4平台 | 设计一致，SoC不同 | 否 | 无需复制S3时钟寄存器 | GPIO/芯片专用配置不可搬 |
| BCLK divider | B：RX bclk_div=8 | P：8 | 字段一致 | 否 | 不把此字段直接解释为实测MCLK/BCLK比 | P4/S3驱动实现需区分 |
| BCLK/WS理论 | B：按64bit帧宽为1.536MHz/24kHz | P：同一帧宽计算 | 理论一致；未新增实测 | 否 | 当前没有逻辑分析仪数据 | 不能宣称波形已验证 |
| WS width | B：TX初始16，RX AUTO | P：相同；历史稳定回读TX/RX 32 | 初始化一致 | 否 | open后的格式协调可改变TX WS宽度 | 仅比较初始化结构会漏掉运行重配 |
| bit_shift | B：TX/RX true | P：宏展开true | 一致 | 否 | Philips一位延迟 | 改动会错位采样 |
| 极性/字节序 | B：MCLK/BCLK/WS反相false，MSB优先、非big-endian | P：同 | 一致 | 否 | 原始4-slot解包前提 | 改极性不能作为无证据试错 |
| left_align | B：TX初始true、RX初始false | P：P4 STD宏true、TDM宏false；D重配TDM可设true | 初始化一致，需区分重配阶段 | 否 | 不能把构造值当永久寄存器值 | 误判为项目独有格式错误 |
| DMA desc | B：AUDIO_CODEC_DMA_DESC_NUM=6 | P：6 | 一致 | 否 | 已匹配 | 扩容会掩盖阻塞与增加内存 |
| DMA frame | B：AUDIO_CODEC_DMA_FRAME_NUM=240 | P：240 | 一致 | 否 | 24k下每块10ms | 6块不能当严格60ms无损保证 |
| auto_clear | B：after=true、before=false | P：after=true，默认before=false | 一致 | 否 | 保持现有TX清空行为 | 不通过改TX清空做额外变量 |
| TX/RX init顺序 | B：new→STD TX→TDM RX | P：同 | 一致 | 否 | 无需重写 | 反序可能影响双工配置 |
| TX/RX enable顺序 | B：TX→RX | P：同；中间新增RX溢出回调注册 | 一致 | 否 | 诊断回调不重启通道 | 关闭TX可使RX失去时钟 |
| EnableInput | B：状态变化才open/close，data_if_mutex串行化 | P：启动open RX，正常会话不反复close | 生命周期不同 | 不照抄 | 本板需要连续采集 | 引入省电close可能制造断流 |
| EnableOutput | B：按需open/close；S在RX工作时不因超时关闭TX | P：启动open TX，播放使用现有设备 | 共享时钟原则一致，策略不同 | 否 | TX静音/无数据不等于应disable | 停TX可能同时影响RX |
| codec open顺序 | B：由Read/Output按需触发 | P：固定先ES8311输出，再ES7210输入 | 不同 | 先观测、不改 | 双工data_if负责格式协调 | 同时改顺序与任务无法归因 |
| TTS开始/结束 | B/S：首用或重新启用输出可open；每次TTS结束不必close | P：普通TTS状态切换只改业务门控/队列，不直接reconfig I2S | 不是逐次等同；无证据表明本板每次TTS重配 | 否 | 已追到调用点 | 把省电路径当每轮TTS路径会误判 |

### shared clock 必须追到 codec data_if

调用顺序不仅是初始化结构体。`esp_codec_dev_open()` 会进入 D 的格式设置，`set_drv_fs()`可能重设slot/clock，`set_fs()`处理RX/TX帧宽兼容并在必要时短暂停启相关通道。当前历史回读的TX 2×32 / RX 4×16应在这个层次解释。

当前普通TTS路径没有调用 `audio_hardware_test_force_official_i2s()`；该函数的RX disable/reconfig/enable属于硬件自检入口。RTC暂停/恢复在 `wake_process_task()`中释放/重建AFE，正常不是重建I2S。不能因为全文搜索到了reconfig就认定FULL每次回答都在改时钟。

S 的 `CheckAndUpdateAudioPowerState()`明确保留正在支持双工RX的TX。这个原则值得保留；其完整省电启停策略并非当前必须新增的功能。源码配置一致也不证明运行期间没有异常；若重新做寄存器快照，必须标注初始配置、codec open后和状态切换后的时点。

## 2. BOX-3 中不能照抄的部分

B3启用reference，B的应用输入通道数因此为2，即 **MR**。虽然物理I2S接收配置有四槽，codec打开输入时只选0/1；不能由“四槽”推断应用保留了两麦。

当前为“4个总线slot → 3个应用通道 → mono AFE输出”。禁止复制B的2通道计算、0x3输入选择掩码、GPIO、PA GPIO或增益默认值。更不能仅把 `input_channels` 从2改3，却仍用只返回两路的mask读取。

BOX-3能作为ES7210/ES8311和双工时钟管理参考，不能作为本板完整MMR数据模型。此边界已经解释了部分代码差异，无需为形式统一重写当前驱动。

## 3. BOX-Lite 中当前可以直接借鉴的部分

L在reference启用时应用通道数为3，Read按两麦后一个参考的顺序组帧。E根据“总输入通道数减去参考数”构造输入格式，因此得到MMR。**当前这部分语义已经一致，不需要改造。** Lite配置中的reference受`CONFIG_USE_AUDIO_PROCESSOR`控制，不能脱离条件说所有Lite配置都为三通道。

| 项目 | 参考实现 | 当前实现 | 是否一致 | 是否需要修改 | 理由 | 风险 |
|---|---|---|---|---|---|---|
| input_channels | L：2+reference，启用时3 | P：AFE_CHANNELS=3；另有物理CAPTURE_CHANNELS=4 | 应用层一致 | 否 | 物理槽数与AFE通道数分开 | 混为同一个channel字段会错读 |
| mic_num | L/E：3−1=2 | Q：MMR解析为2麦 | 一致 | 否 | 不删MIC2 | 改成MR改变双麦目标 |
| ref_num | L/E：1 | Q：MMR解析为1 | 数量一致，来源不同 | 否 | 本板已有硬件REF | 双重REF会改变输入语义 |
| AFE input_format | L接E后MMR | Q：channels==3时MMR，并校验AFE通道数 | 一致 | 否 | 已满足双麦+参考 | 不复制参考的其他算法参数 |
| MMR内存排列 | L：每帧两麦然后REF | P：S0/S2/S1生成M1/M2/R | 语义一致 | 否 | 单独数组不是必要条件 | 为形式重写可能引入stride错误 |
| AFE算法模式 | 当前E：FD/LOW_COST路径；当前工程历史实机为SR高性能AEC+BSS等 | Q：SR/HIGH_PERF，2MIC MMR | 不同 | 本轮不改 | 借的是通道模型和调度，不是算法链 | 同时替换无法定位raw异常 |

这里不要求为了匹配名字再添加一套 `mic_num/ref_num` 全局变量。当前 `afe_config_init("MMR",...)`与输入通道数校验已表达约束。

## 4. BOX-Lite 中不能照抄的部分

L使用ES7243E输入、ES8156输出；板级输入输出均16k。其Read把真实双麦与Write缓存的播放PCM拼接为第三路，涉及 `ref_buffer_`、`read_pos_`、`write_pos_`。这是该板无硬件回采时的适配，不能搬到本板。

本板REF继续来自SLOT1，随同MIC1/MIC2经过相同速率变换后送MMR。不得叠加软件参考、替换为播放缓冲、沿用Lite的codec型号/增益/16k总线参数。借鉴“第三路是参考”的接口语义，到此为止。

## 5. 官方 AudioService 中值得借鉴的任务结构

### 实际源码链路，而非概念图

```text
AudioInputTask
  ReadAudioData(160 samples/ch @16k目标)
    Codec InputData → BoxAudioCodec::Read → esp_codec_dev_read → I2S read
    如需变速：rate_cvt（channel设置为应用输入通道数）
  AfeAudioEngine::Feed
    持久input_buffer；够feed_chunk就循环feed，保留余量
  下一次采集

audio_afe / ProcessingTask
  AFE fetch → HandleVoiceResult → 拼Opus所需mono帧
  OnOutput → PushTaskToEncodeQueue（有界；满时丢旧帧，不等待队列空间）

opus_codec / OpusCodecTask
  取PCM队列 → Opus编码 → audio_send_queue
  通知应用有待发送包

Application主循环
  PopPacketFromSendQueue → protocol_->SendAudio
```

**纠正附件中的简化表述：当前小智官方不是OpusCodecTask直接执行网络发送；Waveshare的encoder task才是在编码后直接sendAudio。** 两者共同点是I2S输入任务不承担同步发送。

S初始化rate converter时传入`codec_->input_channels()`，所以它可以合法处理多通道交错PCM；这不等于把MMR当单声道重采样。不能仅看到一次process调用就判为通道混合。

### 当前代码已具备的边界

```text
xiaozhi_mic (CPU0, priority6)
  esp_codec_dev_read(raw,1920B)
  等capture_guard
  S0/S2/S1逐通道3:2变换 → 160×3个int16
  wake_word_feed → 独立accumulator → AFE feed
  while能取得960个mono样本：Opus → cloud_udp_send_opus
  释放capture_guard → 下一次read

wake_detect (CPU1, priority11)
  fetch_with_delay → handle_voice_result
  写已有output_stream（PSRAM，7680B，满时丢本次输出并计数）

AFE内部处理：配置core1 / priority10
```

所以当前“AFE output”不是在采集线程里同步fetch，采集线程实际上是**读取另一个任务已经写好的输出缓冲，再做编码/发送**。真正要拆的是消费者，而非新增第二套AFE、第二个fetch任务或一整套新音频服务。

### 逐项比较：数据流与任务

| 项目 | 参考实现 | 当前实现 | 是否一致 | 是否需要修改 | 理由 | 风险 |
|---|---|---|---|---|---|---|
| I2S读取粒度 | S常规10ms；W1 240×4×2=1920B | P常规1920B/10ms | 时长一致 | 否 | 不必改变DMA参数 | 完整返回不证明连续 |
| 24k→16k | S多通道rate_cvt；W1三个独立mono rate_cvt | P同槽3:2抽取/平均 | 算法不同，通道不混用 | 推迟独立A/B | raw观测点在变速前 | 同时改会掩盖调度因果 |
| AFE feed | E/W2持久累积，循环消费完整chunk | Q同样持久累积、循环feed、保留余量 | 机制一致 | 否 | 已不与Opus60ms帧1:1绑定 | 不能硬编码512代替API值 |
| feed大小/频率 | 运行时get_feed_chunksize | Q同；get_feed_channel_num校验 | 原则一致 | 否 | 每通道样本数不是字节数 | feed1024/fetch512时78/156每5秒可正常 |
| AFE fetch | E独立audio_afe；W2独立processing task | Q独立wake_detect | 已分离 | 否 | 不需要再增加fetch层 | 双消费者/双fetch会破坏状态 |
| PCM queue | S编码队列2帧，满丢旧；W1有界PCM队列满丢旧再入新 | Q字节流7680B，满丢新输出块 | 有缓冲，策略不同 | 首轮保留大小和策略 | 足以先迁消费者 | 同时改丢弃策略会增加实验变量 |
| mono Opus帧 | S/E按frame_duration拼帧；W1 960 samples | P每次read_pcm取960 samples（60ms@16k） | 当前帧语义一致 | 否 | AFE chunk不等于Opus帧 | 错拼会改变编码输入 |
| Opus执行位置 | S独立opus_codec；W1独立encoder | P在xiaozhi_mic的循环内 | 不一致 | 证据支持后优先拆 | 耗时直接延迟下一次read | 拆分需处理编码器独占 |
| UDP/网络执行 | S发送队列→Application；W1 encoder→sendAudio | P直接cloud_udp_send_opus | 不一致 | 先现有UDP旁路A/B | 可能等待crypto mutex/发送 | 不能从“同步”直接推断已超时 |
| capture priority/core | S启用processor时core0/prio8；W1 core0/prio8 | P core0/prio6 | 不一致 | 本轮不改 | 调度背景不同 | 直接抬优先级会挤压Hosted/USB |
| Opus priority/core | S prio2，xTaskCreate未固定core；W1 core0/prio2 | P实时编码沿用capture core0/prio6，无独立编码任务 | 不一致 | 后续只新增必要worker | 不照抄S3整套优先级 | 新worker还需验证CPU与内存预算 |
| fetch priority/core | E prio3，未固定core | Q prio11/core1；AFE内部10/core1 | 不一致 | 本轮不改 | 当前是另一算法模式和系统负载 | 调优与架构同时改变无法归因 |
| capture锁范围 | S输入路径不围住网络发送；W1输入与发送分离 | P从重采样前一直持锁到编码/UDP完成 | 不一致 | 先统计锁等待；拆任务时缩短范围 | 网络延迟也扩大状态切换锁等待 | 不能只把send移到新线程却继续持同一把锁 |

以上优先级只描述源码配置，不等于已测CPU利用率。官方未固定core的任务不能写成“必在CPU0”。

### 最小修改方案（仅设计，本轮未实施）

若连续性证据支持，保留 `wake_detect → output_stream` 生产侧、容量、丢弃策略、MMR、AFE和重采样不变：

1. `capture_task`只保留读取、当前拆槽/重采样和feed；移出其`while(wake_word_read_pcm...)`编码/发送段。
2. 新增一个上行worker作为现有mono缓冲的唯一消费者，使用独立mono输入/Opus输出暂存，避免与capture的MMR缓冲共用。暂不再增加“PCM队列+Opus队列+网络任务”三套设施。
3. 生产者完成写入后通知worker；worker无完整帧时阻塞等待。读取/清空仍由现有短锁保护，不得持`output_lock`阻塞等待数据，也不跨Opus/UDP持`capture_guard`。
4. **preroll必须纳入编码器所有权设计。** `send_wake_preroll()`也使用同一个encoder和capture_opus，并调用reset。应由同一上行worker串行执行preroll/实时编码，保持preroll完成后detect/start的现有先后关系；不能只搬实时循环就假定共享encoder安全。
5. 给取出的帧附带会话/输出generation并在发送前复核，切入SPEAKING、结束会话或RTC暂停时丢弃旧代数据；停止时先让worker退出/确认不再访问，再释放AFE/缓冲/encoder。不能持锁等待worker，而worker又等同一把锁。
6. 现有任务core/priority保持原值。新worker的core/priority需在具体补丁中显式列为新增调度条件，不默认复制参考的8/2；本报告不把它伪装成“没有任何调度变化”。先复用现有队列而不扩容，以便观察output_drop与端到端延迟。

预期是将背压留在有界上行缓冲，而非反传到I2S读取。若新worker长期处理能力仍不足，会表现为输出丢帧，而不是获得无限吞吐；不能用加大队列掩盖它。

## 6. Waveshare P4 中值得借鉴的 P4/MMR 路径

W1的`inputAudio()`实际读1920B；按四槽stride取0、2、1，拆成三个planar通道；分别使用三个持续存在的mono `esp_ae_rate_cvt`，检查输出长度相同，再交错成MMR。W2累积feed并独立fetch；W1的PCM回调把mono帧放入有界队列，`encodeAudio()`取帧编码并发送。两个关键线程为输入core0/prio8和编码core0/prio2。

与当前逐索引对照如下，`p`表示3个输入frame组成的一组，`ch`表示MMR中的通道：

| 处理点 | Waveshare | 当前 | 结论 |
|---|---|---|---|
| I2S frame | 4个int16，交错原始缓冲 | 同 | 一致 |
| 选slot | MIC1=0、MIC2=2、echo=1 | 同 | 与本板实测吻合，但本板实测才是依据 |
| 通道隔离 | 先拆3个mono数组 | 每个ch只索引同一个slot | 不同写法，同样不混通道 |
| 变速 | 三个有状态库handle | 输出2p取输入3p；输出2p+1取输入3p+1/3p+2平均 | 算法确有差异 |
| 交错输出 | 每frame按通道索引组MMR | `out[3*n+ch]` | 内存语义一致 |
| feed | 以样本/通道传入processor，再累积整chunk | count为交错int16数量，Q按3通道累积 | API计量单位不同，不能直接复制调用参数 |
| 编码/发送 | 与采集分离 | 当前仍在capture线程 | 本轮最有价值的架构参考 |

当前明确的内存布局为：

```text
raw frame n（24kHz，int16）:
  raw[4*n+0] = MIC1
  raw[4*n+1] = 硬件REF
  raw[4*n+2] = MIC2
  raw[4*n+3] = unused

目标frame n（16kHz，int16）:
  pcm[3*n+0] = MIC1
  pcm[3*n+1] = MIC2
  pcm[3*n+2] = 硬件REF
```

这不是把三通道交错数据当单声道变速。当前没有显式planar中间数组，并不构成错误；已有逐样本拆分校验为零差异，也不能反过来证明当前变速滤波质量等同于rate_cvt。

Waveshare的 `bsp_extra_i2s_read()`成功后把调用者的bytes_read设为请求长度；底层仍经codec封装，不能用这个字段排除DMA历史丢失。当前IDF的RX完成队列满时会丢旧队列项，触发`on_recv_q_ovf`，随后继续接收；完整读取1920B与发生过overflow可以同时成立。

Waveshare本版本AFE采用FD/LOW_COST配置，不能因为MMR相同就同时替换当前SR/HIGH_PERF链。BSP GPIO也不复制。硬件初始化的更细参数与库版本见 [Waveshare底层审计](E:/Lummiss_Plant_Robot/WAVESHARE_P4_ES7210_DIFF.md)，完整读取/重采样追踪见 [P4数据路径审计](E:/Lummiss_Plant_Robot/WAVESHARE_P4_AUDIO_REFERENCE_DIFF.md)。

## 最值得验证/修改的三个点

### 1. 先完成已有“只旁路UDP”的A/B

**证据：** P在下一次read前同步执行Opus和`cloud_udp_send_opus`；UDP内部有最长100ms的crypto mutex获取等待及同步sendto。实际耗时尚未测得；之前串口打开失败不是音频实验结果。

**为什么可能解释FULL raw异常：** 网络负载只在完整业务中出现；延迟下一次read可能令已完成DMA队列丢历史数据。此影响发生在重采样前，与raw观测点相容，但目前只是可检验假设。

**最小A/B：** 使用现有诊断固件，A正常，B短窗口只本地计数；AFE/Opus保持不变。同声源、同状态、同长度目标raw；同时看read interval、post-processing、Opus/UDP/锁等待和overflow。必须拒绝跨状态、B混入实际发送、数据不完整的窗口。当前诊断还统一暂停了两套旧探针，避免仅A侧录音/导出的额外负载；不得直接把旧探针固件与新固件的差异算作UDP效果。

**预期结果：** B的gap/overflow与raw同时改善才支持同步发送是重要因素。只降低UDP耗时、raw不改善，不足以定位；本轮无同步改善则不据此推进“UDP已是根因”的结论。

### 2. 证据支持后，做“仅迁移消费者”的任务解耦A/B

**证据：** S和W1均隔离采集与编码；当前已有独立fetch及7680B缓冲，缺少独立上行消费者。

**为什么可能解释FULL raw异常：** 可以移除read之间累计的编码和同步发送延迟；与修改算法不同，它直接作用于采样连续性。

**最小A/B：** 实施第5节方案，保持采集参数、原任务优先级/核、resampler、AFE、队列容量和丢弃策略不变，只增加必要worker/唤醒通知/编码器所有权。记录新增调度条件，不同时改采集优先级或增加队列。

**预期结果：** 在真实UDP发送下read gap和overflow下降，raw改善；若只是上行output_drop增加、raw不改善，不能宣布修复。验收还包括preroll顺序、第二轮对话、SPEAKING切换及RTC退出后的旧帧隔离。

### 3. 若UDP旁路无效，定位非网络的capture_guard等待

**证据：** P在read后无限等待`s_capture_guard`；RTC恢复在同一锁内调用`wake_word_init()`。当前新诊断已有`capture_guard_wait_us`。锁内AFE初始化可能较长，但不能仅凭源码断言本次录音发生过这个等待。

**为什么可能解释FULL raw异常：** 本地专用raw任务不经过这些业务锁，而FULL路径即使不发UDP也可能被控制/恢复路径拖延下一次read。普通TTS本身未发现直接I2S重配调用，不能先替换底层驱动。

**最小A/B：** 在UDP策略固定不变时，对比不触发与触发一次RTC暂停/恢复的受控窗口，关联锁等待、read gap、overflow和raw。仅增加锁持有者/时间标记及被动I2S/ES7210前后快照也可，不改clock和算法；两窗口差异必须注明恢复过程，不能混作同状态UDP对照。

**预期结果：** 若gap与长锁等待重合且格式快照不变，优先缩小生命周期锁的范围并设计停止/恢复握手；若没有相关性，该方向也不成立。若快照出现实际clock/slot变化，再追具体调用者，不直接复制BOX初始化覆盖现场。

resampler替换不列入本轮这三个优先点：它确实有音质实现差异，但不能直接解释其上游已保存raw的异常。待raw连续性问题收敛后再单独比较，绝不与任务解耦同批修改。

## 本轮交付边界

- 已核查Codec初始化、默认宏、codec open后的共享格式协调，以及读取→四槽→变速→MMR→feed→fetch→编码→网络的实际函数调用。
- 已明确“参数一致”“通道语义一致”“生命周期不同”“尚无实测证据”的边界。
- 未修改SLOT映射、MMR、MIC2、硬件REF、算法阈值、增益、DMA、已有任务或resampler；也没有新增软件REF。
- 没有新固件：本轮仅新增文档和参考源码/哈希快照，不需要重新烧录。上轮A/B诊断固件仍保留，下一步应先取得合格实机记录。
