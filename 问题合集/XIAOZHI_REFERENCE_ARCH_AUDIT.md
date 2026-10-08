# 小智语音系统参考架构审核

日期：2026-09-29。项目基线：`c220ace`。本轮只审核并生成本报告，不修改固件、Kconfig、通道、算法或任务配置，不编译、不烧录。

结论：当前只有 AFE fetch 与下行播放完成了任务拆分，上行仍是 `xiaozhi_mic: read → resample → feed → 取增强PCM → Opus → UDP`。另外，UDP A/B 诊断仍默认启用，会在第二个连续监听窗口主动屏蔽真实发送；必须先消除这个测试变量，再判断正常连续对话。不能以这些代码问题直接证明历史低频噪声、唤醒失败的唯一根因。

分类：A=硬件差异，应保留；B=产品策略，可保留；C=架构/并发风险，有触发条件但尚无本轮复现；D=源码可证明的行为缺陷；E=证据不足。P0表示正常语音验收前必须清除的阻断项，不表示已经发生内存破坏。

## 证据与版本

- 当前源码：`src/demo/components/xiaozhi_audio/{xiaozhi_audio.c,wake_word.c,capture_diag.c,capture_diag.h}`、`cloud_mqtt/cloud_mqtt.c`、`cloud_udp/cloud_udp.c`、`webrtc_whip/webrtc_whip.c`、`video_streamer/video_streamer.c`。下文 XA、WW、CD、MQ、UDP、RTC、VS 分别指这些文件；行号以本次审核为准。
- 官方固定参考：[78/xiaozhi-esp32@8ce50d27](https://github.com/78/xiaozhi-esp32/tree/8ce50d27cd7c72c777673f46cbfc3ef7454d1b5c)。本轮直接读取已保存源码 `logs/box_integration_audit/main/`，版本记录在同目录 `reference.json`，没有用最新main替换基线。AS=`audio/audio_service.cc`，AE=`audio/engines/afe_audio_engine.cc`，APP=`application.cc`。
- Waveshare固定参考：[ESP32-P4-WIFI6-Touch-LCD-4B@5a7a9a28](https://github.com/waveshareteam/ESP32-P4-WIFI6-Touch-LCD-4B/tree/5a7a9a2823d77983c15b44a72934a7699e5d2d8b)。直接读取 `logs/waveshare_p4_audit/reference/firmware/brookesia/components/XiaozhiApp/`：WA=`XiaozhiApp.cpp`，WP=`XiaozhiAudioProcessor.cpp`。
- 参考文件代表上述固定版本，不宣称已核对2026-09-29最新上游，也不宣称参考板具有本板相同物理接线。
- 当前两份构建配置均确认 `CONFIG_SPIRAM_USE_MALLOC=1`、`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=0`、`CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=200000`、`CONFIG_WAKE_CLEANUP_TRANSIENT=1`、V3启用。实际选项名是 `SPIRAM_USE_MALLOC`，不是 `SPIRAM_MALLOC_USE_MALLOC`。全部保持。

## 1. 当前音频架构

```text
ES7210 → I2S0 RX DMA → codec data_if → xiaozhi_mic
  24k/16bit/4-slot，每次240帧=1920B
  → 按S0/S2/S1独立取值并3:2变换 → 16k MMR，每批160帧=960B
  → WW持久accumulator → AFE feed

AFE内部处理 → wake_detect fetch
  → VAD统计、WakeNet事件、2秒preroll
  → 受generation保护的mono StreamBuffer

xiaozhi_mic在同一个read循环继续：
  → 从StreamBuffer取960 mono样本 → Opus 60ms → cloud_udp_send_opus
  → 最后释放capture_guard → 下一次I2S read

下行：cloud_udp_rx → Opus包复制队列 → xiaozhi_dec
  → 三块PCM池/ready队列 → xiaozhi_spk → ES8311/I2S TX
控制：MQTT/检测回调 → voice_events → wake_process
```

### 逐级责任、格式与阻塞

| 步骤/源码 | 执行上下文 | 输入→输出 | owner/复制 | 锁与阻塞 | 生命周期 |
|---|---|---|---|---|---|
| `audio_hw_init` XA1125 | service初始化任务 | codec/data_if和I2S句柄 | service创建，硬件层拥有DMA | codec open/I2C会等待 | 音频服务期间；RTC不拆I2S |
| `capture_task` XA1519，`esp_codec_dev_read` | mic CPU0/P6 | DMA四槽→1920B raw | 驱动复制到`capture_raw`，mic独占 | read在capture_guard外，会阻塞；成功不证明历史DMA无丢失 | 循环 |
| `resample_mic_24k_to_16k` XA1498 | mic CPU0/P6 | 240四槽帧→160 MMR帧 | 读raw，写capture_pcm | 持capture_guard，CPU处理 | 每10ms批次 |
| `wake_word_feed` WW543 | mic CPU0/P6 | MMR→整块AFE输入 | memcpy到feed_buffer，余量持久保存 | 外层capture_guard；AFE feed内部可能等待 | AFE实例期间 |
| AFE内部运算 | 配置CPU1/P10 | MMR→mono/检测结果 | ESP-SR内部拥有 | 内部实现不完全可见，不推断无阻塞 | AFE实例期间 |
| `detection_task` WW253 | wake_detect CPU1/P11 | AFE fetch结果 | 结果是库借用内存；立即统计/复制 | fetch最大100ms；output_lock只覆盖缓冲操作 | 初始化至EXITED确认 |
| `handle_voice_result` WW225 | wake_detect | mono→StreamBuffer | 复制到PSRAM 7680B存储 | output_lock；send等待0，满丢本块 | AFE实例期间 |
| `wake_word_read_pcm` WW610 | mic | mono流→960样本/1920B | 复制回capture_pcm | capture_guard→output_lock，receive等待0 | 仅uplink开启 |
| `esp_opus_enc_process` XA1615附近 | mic | 16k mono 60ms→Opus | 共享encoder/capture_opus，此时mic使用 | 持capture_guard；同步编码 | 会话实时上传 |
| `cloud_udp_send_opus` UDP301 | 调用者mic或wake_process | Opus→加密UDP | 栈上独立packet；同步sendto | crypto_mutex取锁最多100ms；sendto在锁内；仅配置RCVTIMEO，无应用SNDTIMEO | socket会话期间 |
| `send_wake_preroll` XA557 | wake_process CPU1/P8 | 冻结mono→Opus→UDP | 独立PCM副本，但复用实时encoder和Opus输出 | 不持capture_guard；通过实时uplink关闭取得逻辑独占 | 每次唤醒 |
| `incoming_audio_callback` XA1818 | cloud_udp_rx CPU0/P5 | Opus→包队列 | 复制包体，队列满丢最旧 | queue等待0 | 通道active期间 |
| `decode_task` XA1667 | dec CPU1/P9 | Opus→24k mono PCM | free池取一块，ready转移所有权 | 等free/ready队列可能无限等待 | 服务期间 |
| `output_task` XA1744 | spk CPU0/P10 | PCM→codec TX | write完成后归还PCM池 | I2S写阻塞；不持capture_guard | 服务期间 |

注意：应用没有独立encoder task或上行send queue。不能把下行独立decoder误称为“Opus上行已经解耦”。

## 2. 官方xiaozhi架构

已逐函数核对，而非按类名推断：

1. AS127/242 `AudioInputTask` → AS195 `ReadAudioData` → codec InputData；必要时使用按输入通道数配置的 `esp_ae_rate_cvt`。
2. AS313 → AE252 `Feed`：累积完整AFE chunk并保留余量；不调用网络。
3. AE406 `ProcessingTask`：应用控制generation、fetch、过滤过期fetch结果；AE473把输出凑成编码帧。
4. AS87 `OnOutput` → `PushTaskToEncodeQueue`；AS381 `OpusCodecTask`在取出任务后解锁，再编码。
5. AS494：Opus packet进入`audio_send_queue_`，通知application；队列满丢最旧，避免网络背压卡住codec。
6. **APP240 主循环 PopPacketFromSendQueue → protocol_->SendAudio。不是Opus任务直接网络发送。**

这是隔离采样时限、算法时延和网络时延的设计。当前硬件MMR并不阻碍采用同样的职责分离；但不需要照搬C++类体系。

## 3. BOX-3对比

核查 `main/boards/espressif/esp32-s3-box-3/` 和 `audio/codecs/box_audio_codec.cc`。**该参考板是ESP32-S3，不是ESP32-P4。** 可参考的是codec接口组织，不能把它的SoC时钟/资源实测写成本板证据。

- BOX-3配置输入/输出24k，ES7210+ES8311，共用I2S data_if。
- `CreateDuplexChannels`一次 `i2s_new_channel`得到TX/RX，TX STD、RX TDM。
- `input_channels_ = input_reference_ ? 2 : 1`，Read/EnableInput有自己的通道选择逻辑；不是本板完整四槽→MMR链。
- A类保留：本板四槽0xF、S0/S2/S1、双麦硬件REF。不能复制其2通道mask覆盖SLOT2。
- task设计主要来自公共AudioService，不能说BOX codec本身提供了P4任务分配方案。

## 4. BOX-Lite对比

核查 `esp32-s3-box-lite/box_audio_codec_lite.cc`：

- 使用 **ES7243E输入、ES8156输出**，不是本板ES7210+ES8311。
- 构造 `input_channels_=2+input_reference_`。
- `Write`253起保存播放PCM到`ref_buffer_`；`Read`223起把缓存参考插入输入。这是软件REF。
- 可参考两麦加参考的语义、输入组织以及公共AFE框架。**不移植其REF缓存、时延假设、通道mask。** 本板R只来自硬件SLOT1。

## 5. Waveshare P4对比

WA2590 `inputAudio`：read四槽 → 三个planar通道 → 三个独立rate_cvt实例 → 检查输出长度一致 → 交错MMR → WP208 feed。WP使用持久input_buffer，while整块feed，保留余量。

WP processing任务fetch、聚合mono、回调PCM队列；WA2750 `encodeAudio`取队列、Opus，**随后在该encoder任务内持audio_tx_mutex调用protocol->sendAudio**。所以它隔离了采集与编码/网络，但不是官方AudioService那样编码和应用发送再拆一层。不能把两个参考混写成同一实现。

WA1795起：input CPU0/P8，encoder CPU0/P2，playback CPU0/P6；WP119用独立AFE处理任务。仅比较职责，不照搬核和优先级。

当前重采样是每通道3:2抽取/相邻均值，不是高质量rate_cvt；它按同一相位分别取各槽，并未把交错MMR误当单声道。此为C/P2音质实现差异，不能解释重采样之前raw的噪声；本轮不调整。

## 6. 硬件差异与必须保留项

| 项目 | 当前源码/事实 | 分类与结论 |
|---|---|---|
| ADC/DAC | ES7210/ES8311；XA1125初始化 | A，保留 |
| I2S | I2S0成对TX/RX；24k；TX STD；RX TDM total_slot=4 | A，保留；不把data width直接当所有阶段slot width |
| 四槽 | `raw[4*n+0]=MIC1, +1=硬件REF, +2=MIC2, +3=unused` | 用户实测事实，优先于参考板定义 |
| AFE输入 | `pcm[3*n]=M1, +1=M2, +2=R`，16k | A，保留MMR与MIC2 |
| 算法 | `afe_config_init("MMR",...,SR,HIGH_PERF)`，AEC=true，WakeNet=true | 不改AEC/BSS/VAD/WakeNet参数；具体内部pipeline以print_pipeline为证，不推断源码不可见算法 |
| gain、硬件REF | MIC1/2既有配置，R不是软件回放副本 | A，保留，不做增益补偿猜测 |
| 内存策略 | ALWAYSINTERNAL=0，reserve=200000 | 保留；不恢复Guard/224KiB/长期预占H264 |

## 7. Audio task对比

| 当前任务 | core / priority | 栈字节/位置 | 工作与等待 |
|---|---|---|---|
| xiaozhi_mic | 0 / 6 | 40960 PSRAM | read+feed+Opus+UDP；同步网络可延后下次read |
| wake_detect | 1 / 11 | 4096 INTERNAL（普通xTaskCreate） | fetch、VAD、preroll、输出copy；100ms fetch等待 |
| AFE内部 | 配置1 / 10 | 库管理，未臆测大小 | AFE内部运算 |
| wake_process | 1 / 8 | 40960 PSRAM | 状态机、hello等待、preroll编码发送、RTC请求；50ms事件等待 |
| wake_cleanup | 1 / 8 | 8192 INTERNAL | 临时load/deinit；完成后调用方删除，保留cache安全 |
| xiaozhi_dec | 1 / 9 | 20480 PSRAM | 下行解码、等PCM池 |
| xiaozhi_spk | 0 / 10 | 6144 PSRAM | PCM队列、I2S write |
| cloud_udp_rx | 0 / 5 | 8192 PSRAM | recvfrom、解密、音频回调 |
| MQTT | 核由组件配置 / 5 | 请求6144；实际位置需结合组件与运行分配 | 网络事件、hello解析、控制消息回调 |
| xiaozhi_service | 0 / 5 | 7168 PSRAM | 初始化完成退出 |
| capture_diag | 无绑定 / 1 | 4096 PSRAM | A/B封存后串口导出，buffer保留到重启 |

官方启用processor时：audio_input CPU0/P8/6144B；audio_output无绑定/P4/4096B；opus_codec无绑定/P2/24576B；AE处理任务无绑定/P3、PSRAM静态栈。这些不是本板推荐直接替换值。当前P6采集与CPU0网络、CPU1 AFE并存是否丢样本，要看read interval和RX overflow；本轮未测CPU占用和栈余量。

## 8. AFE数据流对比

当前WW543 accumulator已经正确循环消耗整块并保存不足块的数据，不存在每60ms只feed一次并直接扔尾巴的写法。WW610只读取完整960 mono样本供Opus，AFE feed与Opus帧大小没有硬绑1:1。

**纠正旧诊断：**最近日志明示 `feed_samples_per_channel=1024, channels=3, feed_i16=3072, feed_bytes=6144, fetch_samples=512`。此时理论feed=78.125次/5s、fetch=156.25次/5s。78/156正常；必须比较 `feed_samples`、有效fetch样本和overflow，不能再套用512样本feed得到“恰好欠喂一半”。

当前应用VoiceProcessing=false仅关闭增强PCM输出缓存，并不停止AEC/BSS/feed/fetch（RTC除外）。SPEAKING期间持续参考处理是B类产品设计，不是“语音开关失效”。WW output_generation在fetch前后验证，可丢弃跨门控转换结果，是有效防护。

## 9. Wake状态机对比

| 状态 | WakeNet门控 | 增强PCM输出/上传 | 实际行为 |
|---|---|---|---|
| WAKE_IDLE | 开 | 关 | 持续采集/feed/fetch以检测唤醒 |
| CONNECTING | 关 | 关 | 等旧播放结束、重新hello、建UDP、preroll |
| LISTENING | 关 | 开 | 静音也上传；VAD不是会话状态 |
| SPEAKING | 关 | 关 | 播放与硬件参考继续；等stop及排空 |
| CONTINUOUS_LISTENING | 关 | 开 | 自动listen/start，无需重说唤醒词 |
| RTC suspended | 关 | 关，AFE销毁 | I2S采集任务仍read并丢弃，非关闭硬件 |

XA337统一门控，XA792事件消费者统一状态转换，是值得保留的结构。官方可按配置在SPEAKING/Listening允许唤醒打断；本产品禁止重复唤醒是B，不是bug。

## 10. Continuous listening对比

XA501等待tts_stop、软件播放排空、至少120ms余量，再发布listen/start并开启上传。XA970附近以进入监听/最近STT为依据30s超时回WAKE_IDLE。不会因单帧VAD SILENCE退出。

当前最直接的干扰是 **CD.h末尾宏=1**，service无条件调用capture_diag_init；第二个连续监听入口等待500ms后，CD122 `capture_diag_sink`将最多4秒真实UDP替换为本地packet/bytes计数。XA1643仍在send_error=ESP_OK时增加capture计数，故“计数增长”不是“服务器收到”。窗口会因状态改变提前无效；不能说每个第二轮一定完整屏蔽4秒。

即使关掉该测试，30秒持续UDP无STT也只能说明尚未收到识别结果。需分开验证音频有效、UDP真实send、会话key/connection、服务端listen状态。客户端QoS0 publish成功不是ASR启动确认。

## 11. TTS状态切换对比

官方APP619：收到tts/start后调度进入Speaking；stop进入Listening，若播放未排空则延迟StartListeningAudio。当前方向相同，额外120ms为产品余量。

67/93ms LISTENING不是单凭时长就能判bug：官方也支持发送wake音频和detect后服务端立即应答，不要求先收到STT才能tts/start。当前XA728也先发preroll/detect/start，随后消费已排队TTS。因此既可能是正常唤醒问候，也可能是旧/无session事件污染；必须对应VOICE_TRACE中TTS_START_RX、FIRST_STT_RX、PREROLL、FIRST_LIVE_OPUS_TX和session，不能直接归因说话不清楚。

排空判定存在C类竞态：decode/output先xQueueReceive，再标in_flight；XA466先读in_flight再读队列深度，非同一事务。控制任务可能恰好看到队列空且in_flight尚未置位。120ms降低概率但不构成同步证明。最小方案是用受统一保护的待播放计数/代际完成事件，在入队到write完成全过程覆盖所有权，不忙等也不扩大队列。

## 12. Preroll对比

当前WW184单写者保存最近2秒mono=64000B PSRAM，检测时先stop WakeNet再投事件，后续不再Store，控制任务复制后发送。正常串行路径未发现复制期间持续写入、提前free或同时Opus编码的证据。

共享Opus由mic实时使用，CONNECTING经capture_guard关闭uplink并等旧循环结束，wake_process才reset/编码preroll；set LISTENING在preroll结束后，当前有逻辑独占。不要把“两个任务调用同句柄”直接判为并发bug。但未来解耦必须明确单owner或独立preroll encoder，不能破坏这个隐含约束。

官方AE530起使用独立临时encoder把冻结wake cache编码成packet队列，APP在通道建立后发送，避免与实时encoder争用。当前在hello之后才编码/发送，会增加用户第一句话之前的盲区。

冻结后至listen开启期间，当前feed仍继续但output关闭、preroll不更新，用户立即说出的第一句话开头可能不被保留（C，需音频/时间线验证）。最小A/B先量化wake→uplink耗时并提示监听就绪；若要消除此窗口，再独立设计有界过渡PCM缓存，不复制旧session队列。preroll尾部不足960样本不编码：最多959样本约60ms，这是当前完整帧策略，不是UAF。

## 13. RTC/Audio生命周期

START主路径：RTC请求 → wake_process持capture_guard确认当前capture循环结束 → suspended=true →关闭uplink/UDP → 再持guard调用WW deinit → INTERNAL cleanup worker设置stopping，等待fetch EXITED →销毁AFE/model →删除cleanup →RTC ACK。worker不反向取得capture_guard，fetch不取guard，未找到这里必然的锁环。保留INTERNAL worker，不能迁PSRAM。

**STOP方向缺少硬件释放屏障（D）：**RTC1039调用video_stop；VS1009仅关门控、清队列、设IDLE并返回，实际H264 close/del在codec任务下一次50ms receive超时分支（VS2230附近）。RTC1055随后恢复AFE，期间没有等H264释放的ACK。HTTP DELETE/peer关闭可能恰好耗时足够，但不是保证。最小patch：由codec完成close/del后发确认，stop等待该确认，再恢复AFE，保留原核/优先级/缓冲布局。

请求层`s_rtc_request_guard`串行化RTC请求，ACK后才继续资源申请；control正在hello等待时检查rtc_requested，但preroll循环没有取消点，RTC可能等整段编码/网络结束（C）。不要用直接删除任务绕过。

初始化/失败路径：`stop_worker_tasks`直接删除已有任务；这是初始化失败回滚，不是常规RTC停止。仍有删除时持锁/codec in-flight的C类风险，后续用协作退出；本轮无UAF/double-free实机证据。

## 14. Buffer ownership

| 对象 | 写入者→读取者 | 内存与释放 | 判断 |
|---|---|---|---|
| capture_raw/pcm/opus | mic；preroll阶段复用opus | PSRAM工作缓冲，服务cleanup释放 | 当前逻辑独占；拆任务后必须copy/转移 |
| feed_buffer | mic→AFE | PSRAM，AFE实例deinit释放 | 尾样本保留，deinit由guard排除feed |
| AFE fetch data | ESP-SR→wake_detect | 借用，下一fetch前copy | 不把借用指针放跨任务队列，当前做到 |
| output_stream | wake_detect→mic | PSRAM 7680B，约240ms mono | output_lock，generation，满丢新整块，不阻塞fetch |
| preroll ring/临时copy | wake_detect→wake_process | 64000B环+一次copy | 正常路径冻结后读；注释“回调同任务读取”已过期 |
| playback Opus queue | UDP RX→dec | 24个内嵌包，PSRAM，入队copy | 无session generation字段 |
| playback PCM池 | dec→spk→free | 3×4096B PSRAM，ready深度2 | 不reset指针队列是正确的；缺少代际标签 |
| voice_event | MQTT/fetch→wake_process | 12项copy，含session字符串 | 缺generation，无session事件可穿透 |
| UDP packet | mic/wake_process→sendto | 调用栈临时buffer，同步调用 | 不依赖原Opus缓冲返回后寿命 |
| A/B windows | capture写→export只读 | 约2MB PSRAM，导出后保留到重启 | 明确诊断占用，不误报为逐轮泄漏 |

## 15. 锁与并发风险

| 锁/规则 | 实际链路 | 结论 |
|---|---|---|
| capture_guard | mic：read后取锁→feed→Opus→crypto_mutex→sendto；状态机/RTC也取它 | C：网络延迟直接阻塞read和状态切换；不是仅“可能间接”等待，而是同步调用证实 |
| output_lock | fetch/send、mic/read、门控reset；队列调用wait=0 | 未找到反向获取capture_guard路径；正常不构成ABBA |
| lifecycle_lock/done | control持guard等待worker，worker等待fetch退出 | 正常退出顺序合理；API阻塞不可简单视为死锁 |
| crypto_mutex | 分配seq+加密+sendto都在锁内 | 保序有理由；不应简单移出sendto造成并发序号乱序；应把整个发送职责迁到独立owner |
| UDP stop | lifecycle锁→shutdown→crypto锁→close→等待RX退出 | 已有防fd复用/GCM UAF措施；RX未退出拒绝start，不能重复修旧问题 |
| MQTT session | getter检查原子ready后无锁copy；reopen/disconnect/hello修改同一结构 | D：atomic bool不能保护结构体；可发生混合session/key快照；发生过与否需注入验证 |
| voice事件代际 | 非空session要求匹配，但空session直接接受 | C/D：确认存在绕过路径；迟到无session TTS/END可改变新会话，具体服务端是否发送需日志 |
| 播放门控 | packet/PCM只有内容，没有generation；仅检查CHANNEL_ACTIVE | C：跨stop/start只靠瞬时active不足；需结合排空竞态验证旧块是否进入新轮 |

当前未发现可以据源码直接断言“每次必死锁”“已双重释放”“preroll已UAF”的证据，不为满足清单强行判定。

## 16. 明确bug/确定缺陷

1. **正常连续对话混入发送屏蔽诊断（D/P0验收阻断）**：CD.h=1，CD122返回true，XA跳过UDP却仍增加成功采集计数。作为专用A/B符合原实验目的，作为正常固件则行为错误。不能仅把宏改0：XA2161会因此重新启用旧`lifecycle_prepare`，导致测试音/TX窗口复活；应一次独立patch明确“正常模式两个侵入式探针均关闭”。
2. **MQTT会话快照缺少同步（D/P1）**：MQ108/143/214/398多上下文读写，ready只保护标志不保护复制。最小做法解析到局部结构，短锁原子替换/复制/清空，锁外publish；加本地generation避免旧事件沿用。不得在session锁内阻塞网络。
3. **RTC_STOP未等待H264实际释放（D/P1）**：资源恢复顺序没有实现用户要求的完成屏障，不以“某次恢复成功”抹掉竞态。

另外，缺session消息被接受、排空快照不原子、无SNDTIMEO均为确定代码事实；是否导致历史故障属于C/E，不把未知因果写成已经复现。

## 17. 架构偏差

- C/P1：采集同步承担Opus和UDP，且持guard；与官方和Waveshare的采集职责都不同，非硬件造成。
- B：关闭说话期间唤醒/上传、RTC期间销毁AFE、每次唤醒新hello、30秒无STT超时，可作为产品策略保留；必须明确listen协议和超时体验。
- C/P1：MQTT无request/generation关联，旧hello/无session事件无法完全区分；不能只加客户端计数就宣称能识别所有服务端迟到包，服务端无关联字段时需要协议边界。
- C/P2：3:2简化重采样与rate_cvt不同；待采集连续性稳定后独立音质A/B，本轮禁改。
- A：硬件slot/MMR/REF差异必须保留；不因官方MR判当前错误。
- E：67/93ms是否为旧TTS、30秒无STT是否由数据质量、服务端ASR或session错误造成，现有源码不能一锤定音。

## 18. 建议修改项与单变量验收

| 优先级/项目 | 官方为什么这样做 | 当前真正差别/硬件原因 | 是否构成实际bug | 最小修改与独立A/B |
|---|---|---|---|---|
| P0 正常模式关闭侵入式探针 | 正常输入不主动屏蔽网络 | 第二个continuous窗口本地sink；非硬件 | 特定窗口确定不发送，不等于全部历史故障 | 独立patch同时门控A/B与旧lifecycle实验，保留被动计数；连续3轮对话，确认无sink、无测试音、真实UDP计数对应 |
| P1 采集与上行解耦 | read时限不能受Opus/网络影响 | mic在guard内做全部；非硬件 | 确定可阻塞，是否raw丢样本尚待测 | 有界PCM交付→唯一Opus owner→有界发送队列/独立发送上下文；不改I2S/算法/core策略，先明确任务预算；对比同声源read P99/overflow/音质，不能只看平均 |
| P1 session快照与事件代际 | 官方控制事件主循环、相关buffer有generation；不意味官方协议无所有竞态 | 本项目MQ跨任务结构复制无锁；事件空sid放行 | 快照同步缺陷明确，旧事件因果待复现 | 局部parse+短锁快照+代际标签；缺sid按协议明确策略；注入迟到hello/TTS/END及断线，不许误开uplink |
| P1 RTC释放屏障 | 资源owner完成后才交生命周期 | stop函数返回早于codec close/del；与codec型号无关 | 不满足已要求的先释放后恢复 | codec stop完成ACK后恢复AFE；10轮stop/start记录H264释放先于AFE创建，不改内存布局 |
| P1 播放排空与代际 | AS playback_generation和队列/in-flight在同锁下判断 | 当前receive和in-flight存在间隙，无块代际；非硬件 | 潜在早开mic/旧音频混轮 | 统一pending计数和generation，不扩大队列；注入stop与末包交错、新session，验证实际write结束后才listen |
| P2 preroll等待盲区/控制取消 | 官方独立编码wake cache，控制与实时owner分开 | 当前hello后串行reset/encode/send，RTC取消粒度粗 | 可能丢醒后紧接的一句话开头 | 先测wake→listen延迟和音频覆盖；随后单独决定过渡PCM缓存/独立preroll owner；不直接复制软件REF |
| P2 重采样与诊断注释清理 | rate_cvt做规范带限转换；记录单位清晰 | 当前轻量3:2、旧注释仍称callback读preroll等 | 音质差异非raw噪声已证根因 | 连续性稳定后单独频谱/试听A/B；清理过期注释不改算法链 |

本轮没有改以上任何业务行为。建议先处理P0，再每项一个patch、一次A/B、一个commit；不将拆任务、改采样、改增益、改AFE混在一起。
