# P4双麦完整链路源码对比

2026-09-28。采用Ponytail最小化原则：先追真实路径，只输出差异与最小验证，不重构音频系统。未修改AFE/WakeNet/AEC/BSS/Opus/MMR/slot mapping，也未改任务、驱动或构建配置。硬件初始化详见 [ES7210对比](E:/Lummiss_Plant_Robot/WAVESHARE_P4_ES7210_DIFF.md)。

## 0. 参考边界与源码导航

首参考为Waveshare P4-4B，不再把BOX当双麦等价基线。固定提交`5a7a9a2823d77983c15b44a72934a7699e5d2d8b`；以下R路径均相对`firmware/brookesia/`：

| 标记 | 文件/函数 |
|---|---|
| R1 | [components/XiaozhiApp/XiaozhiApp.cpp](https://github.com/waveshareteam/ESP32-P4-WIFI6-Touch-LCD-4B/blob/5a7a9a2823d77983c15b44a72934a7699e5d2d8b/firmware/brookesia/components/XiaozhiApp/XiaozhiApp.cpp)：inputAudio 2589、encodeAudio 2750、初始化1626起 |
| R2 | [components/XiaozhiApp/XiaozhiAudioProcessor.cpp](https://github.com/waveshareteam/ESP32-P4-WIFI6-Touch-LCD-4B/blob/5a7a9a2823d77983c15b44a72934a7699e5d2d8b/firmware/brookesia/components/XiaozhiApp/XiaozhiAudioProcessor.cpp)：feed 208、AFE config 448、processAudio、handlePcm |
| R3 | [components/bsp_extra/src/bsp_board_extra.c](https://github.com/waveshareteam/ESP32-P4-WIFI6-Touch-LCD-4B/blob/5a7a9a2823d77983c15b44a72934a7699e5d2d8b/firmware/brookesia/components/bsp_extra/src/bsp_board_extra.c)：bsp_extra_i2s_read 83 |
| P1 | [当前xiaozhi_audio.c](E:/Lummiss_Plant_Robot/src/demo/components/xiaozhi_audio/xiaozhi_audio.c:1460)：resample_mic_24k_to_16k、capture_task 1481 |
| P2 | [当前wake_word.c](E:/Lummiss_Plant_Robot/src/demo/components/xiaozhi_audio/wake_word.c:299)：wake_word_init、wake_word_feed 429、detection_task |
| D | [共同codec data_if](E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_codec_dev/platform/audio_codec_data_i2s.c:693)：_i2s_data_read |
| I | IDF5.5.5 `components/esp_driver_i2s/i2s_common.c:1398`：i2s_channel_read，约630/720行RX DMA完成回调 |

下载源码位于`logs/waveshare_p4_audit/`。参考库版本：codec_dev1.5.11、audio_codec2.4.1、audio_effects1.3.0~1、esp-sr2.4.7、Hosted2.12.11。当前lock：codec_dev1.5.11、audio_codec2.4.1、esp-sr2.3.1、Hosted2.7.4。参考源码存在不等于已经获得其同声源长期实机稳定性数据。

## 1. I2S read → 四槽提取 → 重采样 → MMR → feed

### 参考的实际调用链

```text
xiaozhi_input任务(core0/prio8)
  XiaozhiApp::inputAudio()
    bsp_extra_i2s_read(device_tdm,1920,&bytes_read,100)
      esp_codec_dev_read → _i2s_data_read
        i2s_channel_read(...,&实际bytes_read,1000)
    按240个frame拆三个planar通道
    3个独立esp_ae_rate_cvt_process(24k→16k)
    校验三个输出长度一致 → 按M1,M2,R交错
    XiaozhiAudioProcessor::feed()
      input_buffer持久累积 → while够一块: AFE feed → 保留余量
另一个processing task：AFE fetch → 拼960 mono samples → PCM队列
另一个encoder task：取队列 → Opus → sendAudio
```

参考R1:2640..2714，实际索引（常量在59..62行）：

```cpp
mic1 = device_tdm[frame * 4 + 0];
mic2 = device_tdm[frame * 4 + 2];
echo = device_tdm[frame * 4 + 1];
device_planar[frame] = mic1;
device_planar[240 + frame] = mic2;
device_planar[480 + frame] = echo;
// 每个channel分别传入自己的mono resampler
afe_interleaved[frame * 3 + channel] =
    chat_planar[channel * max_output_samples + frame];
```

以上片段按源码常量展开，不是仅引用README。

### 当前真实路径

```text
xiaozhi_mic(core0/prio6)
  esp_codec_dev_read(...,raw,1920) → 同一codec封装 → i2s_channel_read
  lifecycle_raw(raw,1920)                    ← 重采样前的观测点
  等待s_capture_guard
  raw统计 → resample_mic_24k_to_16k(raw,pcm,160)
  audio_probe_input(raw,pcm)                 ← 同批raw和MMR
  wake_word_feed(pcm,480 int16)
    持久feed_buffer累积 → 够3072 int16时AFE feed
  while有上行mono帧：读AFE输出 → Opus编码 → cloud_udp_send_opus
  释放s_capture_guard → 下一次I2S read
```

当前没有先造三个planar临时数组，但循环`ch`只访问同一slot，通道独立；不等于“把MMR当单声道重采样”。展开源码：

```text
slot[0]=0; slot[1]=2; slot[2]=1
out[3*(2*p)+ch]     = raw[4*(3*p)+slot[ch]]
out[3*(2*p+1)+ch]   = (raw[4*(3*p+1)+slot[ch]]
                     +raw[4*(3*p+2)+slot[ch]])/2
```

加法先int32，最终int16；整数除法向零截断。输出仍为`[M1,M2,R]`。slot映射符合本板实测，不修改。若原始raw确实已低频异常，**后面的数学重采样不可能逆向修改先前已经复制的lifecycle raw**；其CPU耗时/阻塞则可能间接影响下一次RX及时性，必须区分。

## 2. 重采样算法不是同一种

| 项目 | Waveshare R1:1642..1671、2682 | 当前P1:1460..1478 |
|---|---|---|
| 实现 | Espressif `esp_ae_rate_cvt` | 自写3输入→2输出 |
| 配置 | 24k→16k、mono、16bit、complexity2、SPEED | 固定24k→16k，输出160 |
| 通道独立 | 三个不同handle，三次process | 三个独立slot索引，不混样本 |
| filter taps | 公开头文件/文档未给taps；核心在预编译库 | 两个相位权重分别[1,0,0]与[0,1/2,1/2]，无长滤波器 |
| FIR/polyphase名称 | 无公开算法源码证实，不能擅自标为某阶FIR/polyphase | 可明确描述为按3:2分组抽取/相邻平均；非完整抗混叠滤波器 |
| 连续状态 | handle跨输入批次保留；setAudioModes请求reset | 无跨块filter history |
| frame边界 | 240frame输入，使用API实际输出数且三路等长 | 固定240→160；240整除3，当前正常整批不会重置错相 |
| 输出chunk | 常规速率对应160，但实际由process返回；不硬假定每批160 | 恒定160/ch |
| 异常处理 | 任一通道失败/0输出/长度不等则整批不feed | 依赖上层完整read；无可变长度resampler接口 |

原始库证据：[rate conversion header](https://github.com/espressif/esp-adf-libs/blob/89aa3c399c60045880854ffbd314258b49c671e1/esp_audio_effects/include/esp_ae_rate_cvt.h)、[算法模块文档](https://github.com/espressif/esp-adf-libs/blob/89aa3c399c60045880854ffbd314258b49c671e1/esp_audio_effects/docs/README_RATE_CVT.md)。没有公开taps就明确未知，不用API名称冒充实现细节。

逐样本自校验只能证明当前实现/拆分一致，**不证明抗混叠质量与库实现等价**。这是实质音质差异，但由于本次异常已经在24k raw，不能把替换resampler列为本轮首修。也不据此添加软件高通。

## 3. AFE与feed节奏

| 字段 | 参考R2 | 当前P2 / 已有实机 |
|---|---|---|
| input_format / mic / ref | MMR / 2 / 1 | 一致 |
| sample rate | 16k | 16k |
| AFE type/mode | FD / LOW_COST | SR / HIGH_PERF |
| AEC | aec_init=true，具体aec_mode未另写，由库配置生成 | true；实机SR_HIGH_PERF |
| NLP | 未显式设置，不能猜最终值 | 未显式设置 |
| SE/BSS | 未显式覆盖se_init；需参考实机pipeline确认具体实现 | 实机SE(BSS) |
| NS | 显式false | 使用SR配置默认，需完整运行配置判断；本轮不改 |
| VAD | true，MODE0，min_noise100ms，可选VAD模型 | 默认配置；已确认pipeline有VAD |
| WakeNet | true，筛选模型 | true，当前加载模型 |
| AGC | 显式false | 未显式覆盖 |
| memory | MORE_PSRAM | MORE_PSRAM |
| 内部AFE任务偏好 | 应用未覆盖core/priority | core1/prio10 |
| feed_samples/ch | runtime get_feed_chunksize() | 当前实机1024 |
| feed_channels | 应用固定3 | runtime核验3 |
| feed_i16 / bytes | runtime值×3 / ×6 | 3072 int16 / 6144 bytes |
| fetch_samples | 使用result->data_size；未硬编码为固定fetch值 | runtime512 |
| Opus分块 | handlePcm累积960mono | output stream供960mono |

**不能从参考源码编造其运行时feed/fetch具体数值。** 参考日志未提供；FD/库版本不同也不能沿用当前1024/512作为其实测值。

当前理论：16000/1024×5=78.125次feed，16000/512×5=156.25次fetch。**78/156是正确节奏**，不是一半欠喂。当前`wake_word_feed()`持续保留不足一块的尾部；参考R2:242同样while满足整块继续feed，erase已消费部分。两者在累积原则上一致。10ms输入批次使1024/ch的feed间隔约60/70ms；不可重新改成156次feed/5秒。

当前fetch任务core1/prio11；参考外层processingTask是未绑核的xTaskCreate/prio3/8KiB。参考AFE内部任务优先级未显式设置，不能用外层prio3推断整个AFE只有prio3。

## 4. actual bytes_read：追到最底层后的结论

参考R3的关键实际代码：

```c
(void)timeout_ms;
*bytes_read = 0;
ret = esp_codec_dev_read(record_dev_handle, audio_buffer, len);
if (ret == ESP_OK && bytes_read) *bytes_read = len;
```

所以参考上层看似检查`bytes_read>0`，实际上成功就获得1920，**不是真实短读检测**。传入100ms也没有生效。

双方共同D：

```c
if (in_reconfig) { memset(data, 0, size); sleep(10); return OK; }
ret = i2s_channel_read(rx_chan, data, size, &bytes_read, 1000);
return ret == 0 ? OK : DRV_ERR;
```

actual bytes_read在此丢失，timeout/invalid-state被折叠成DRV_ERR。双方上层失败都丢弃本批，不把失败后的半缓冲交给AFE；当前等待10ms再读，参考同样失败后等待10ms。

IDF5.5.5 I：正常RUNNING情况下循环直到requested字节读完；队列等待失败明确返回TIMEOUT。因此**不能只凭忽略bytes_read就断言正常路径存在ESP_OK短读**。状态退出/重配分支仍是应记录的边界，但本次正常语音切换未发现持续重配。

更值得关注的区别是“读满”和“连续”：

- IDF RX ISR在队列满时可丢弃旧完成缓冲，并触发on_recv_q_ovf（如有注册）。
- read函数队列接近满时会丢开当前已读位置、换取DMA缓冲，避免读已被覆盖的数据。
- 因而可以返回完整1920B，但历史样本已丢，**ret=OK且bytes=1920并不能证明录音连续**。
- 当前FULL没有专用raw探针那样的溢出回调；累计raw_frames统计的是已消费帧，不是硬件连续采样序号。
- 当前`raw_adc_probe.c::read_block()`直接检查got==bytes、注册on_recv_q_ovf，专用测试与FULL的可观测性不同。参考FULL也未发现连续硬件sample counter或RX溢出计数。

风险评级：隐藏实际长度是双方共有的诊断盲区，不能虚构成参考有/当前无的HIGH差异。当前把编码/网络放在下一次RX read之前是**HIGH优先级连续性风险**，但是否已经丢样需实测，不等于已确定低频峰原因。

## 5. FULL并发与调度

| 项目 | Waveshare P4 | 当前工程 |
|---|---|---|
| capture | core0/prio8/6KiB；read→resample→feed | core0/prio6/40KiB；read→feed→编码/发送 |
| Opus上行 | core0/prio2/40KiB，等PCM队列 | capture内，持s_capture_guard执行 |
| playback | core0/prio6/24KiB，解码/输出路径 | 解码core1/prio9，输出core0（独立任务） |
| AFE fetch外层 | 不绑核/prio3/8KiB | core1/prio11/4KiB |
| I2S read请求 | 240×4×2=1920B，每批10ms | 相同 |
| DMA ring | 6×240frame；RX每desc1920B | 相同；约60ms总容量，并非保证可阻塞60ms |
| DMA完成队列 | IDF desc_num-1=5项 | 同IDF；有消费者位置/中断时序因素 |
| 不足缓冲补零 | 普通失败不补；只有共同data_if重配分支填零 | 同 |
| 连续sample计数 | 未发现硬件连续计数 | raw_frames为消费计数；不等价硬件连续性 |
| Wi-Fi/Hosted | C6/SDIO，Hosted2.12.11，mempool prefer PSRAM | C6/SDIO，Hosted2.7.4，本地DMA pool策略有修改 |
| LVGL | 桌面UI持续，MIPI显示 | UI/动画/SD等业务，SPI LCD |
| Camera | Camera App run才startPreview，pause/close停；MIPI CSI | UVC，当前宏autostart=0、YOLO=0，RTC指令才启动 |
| 网络/视频 | 小智App流程；不是本项目WHIP+UVC完整流水线 | 额外MQTT/天气/RTC等模块；是否工作要看当次事件 |

**不能把FULL三个字等同“UVC/H264此刻必然运行”。** 当前`main/test_profile.h:65`的CAMERA_UVC_AUTOSTART=0，PERSON_DETECT=0；无RTC启动证据时，不把摄像头列作正在抢CPU的既定事实。参考Camera也不是开机一直跑。

当前P1 capture持锁期间调用`cloud_udp_send_opus()`：其等待crypto mutex最多100ms，再在锁内sendto；源码只设置SO_RCVTIMEO，未看到专门的SO_SNDTIMEO。不能保证这条路径每次都在10ms内返回。参考encodeAudio也有网络锁，但它属于独立低优先级编码任务，不直接阻止下一次inputAudio读取。

除了自己做编码/网络，当前还等待`s_capture_guard`、执行诊断复制、等待AFE feed。参考同样feed可能阻塞，不代表其彻底无阻塞。当前AFE内部core1与UI/动画及可选视频任务共核，FULL附加负载可能经AFE回压间接阻止core0下一次read。

当前lifecycle寄存器snapshot在worker中且不持采集mutex执行I2C；未发现“持life mutex读全部寄存器”的错误，不能凭诊断模块存在就判其根因。音频导出在录音后进行，不能把导出串口流量自动算作先前录音污染。

两边CPU/DMA/PSRAM都有竞争可能，但没有本轮实测调度时长、RX溢出、完整heap峰值，不能把“竞争可能”写成“DMA已损坏”。共同malloc buffer是CPU copy目标，非I2S硬件直接DMA；当前PSRAM用户buffer本身不构成格式错误证据。

## 6. CoreP4第二参考与BOX第三参考

[CoreP4源码](https://github.com/78/xiaozhi-esp32/blob/8ce50d27cd7c72c777673f46cbfc3ef7454d1b5c/main/boards/m5stack/corep4/m5stack_corep4.cc#L342)确实P4+ES7210+ES8311+24k+reference，调用通用BoxAudioCodec构造，没有传双麦layout扩展。该固定提交默认仍是MR两路逻辑输入，不能把它作为完整MMR算法等价样板。GPIO来自另一板，保留本板定义。BOX/BOX3仅保留codec/shared-clock管理参考。

## 7. 真正一致与真正不同

真正一致：同codec_dev版本；三路ADC使能；RX4×16 TDM与TX STD共享时钟；6/240 DMA；auto_clear别名；完整4槽10ms读取；参考与本板软件选取0/2/1；按独立通道处理后MMR；持久accumulator、不足一块保留；Opus960mono与AFE chunk分离。

真正不同：两麦24dB vs30dB、参考24dB vs0dB；参考输出stereo而当前mono；参考三个库重采样器而当前分组抽取/平均；FD_LOW_COST vs SR_HIGH_PERF及SR版本；采集/编码网络分工、capture优先级、AFE任务位置；音频App启停与当前常驻服务；FULL业务集合和Hosted版本/内存策略。

这些差异中，PGA绝对值、重采样数学算法、AFE参数均**不能单独解释“同一当前audio_hw_init下，专用直采清楚而FULL raw先变差”**。两麦等PGA下的幅度比也不能通过抄参考24dB消除。参考未发现单麦数字补偿；原理图核查见另一报告。

## 8. 仅三个最值得做的单变量验证（本轮未实施）

共同观测：同固定声源/距离/采样长度，保存24k四槽原始PCM；记录read间隔max、返回ret/actual bytes、RX overflow、已消费样本数与墙钟时间；不改slot/gain/AFE/resampler。若raw已经连续且异常仍存在，及时降低调度假设优先级。

### 1）采集任务承载同步上行：优先级HIGH

**证据：** 当前capture持锁做Opus+UDP，网络锁最多等待100ms，RX DMA总量约60ms；参考独立input/encoder。FULL监听进入上行才持续走这条路径；raw直采不走。

**假设：** 一次慢编码/发送使DMA完成缓冲被替换，读满但丢时间段，产生断续/频谱污染。并非已经证明37/47/53Hz由此产生；如果AUDIO_INIT_DONE阶段已异常，它不能解释全部问题。

**最小A/B：** 先只增加RAM中的耗时/溢出计数，录完打印。若瓶颈定位为send，再在短诊断窗口中只将send动作替换为本地计数，编码/AFE/门控保持；窗口结束前不拿服务端会话变化后的数据作同状态比较。不要同时换优先级或resampler。最终解耦实现另行批准，不本轮重构。

**预期：** 若B的read大间隔/overflow消失且raw改善，支持同步发送回压；若只上行改变而raw不变，不成立。若编码耗时而非send超预算，下一次单独验证编码，不混在同一轮。

### 2）capture调度余量：优先级MEDIUM-HIGH

**证据：** 当前core0/prio6，参考core0/prio8；当前FULL有Hosted及其他网络任务，同一DMA容量；raw专用阶段没有同等业务负载。

**假设：** 非read处理本身很短，但task ready到再次获得CPU有长延迟。与第1项主动阻塞是不同机制；提高优先级不能解决锁内send阻塞。

**最小A/B：** 保持任务划分/核/缓冲不变，仅诊断构建把capture priority6→8。同时统计ready调度延迟、read间隔与overflow，并观察网络输出有无退化。不把其他任务一并挪核。

**预期：** B调度延迟和raw不连续同时改善才支持；如果等待主要发生在mutex/feed，prio变化无效应撤回，不把“更高prio”当最终修复。

### 3）FULL中core1业务对AFE供数的回压：优先级MEDIUM

**证据：** 当前AFE内部偏好core1/prio10、fetch core1/prio11，capture同步feed；FULL同时有UI/动画等工作，参考按App管理资源且AFE运行模式/版本不同。当前无RTC时不默认加入视频负载。

**假设：** core1某段执行/锁/内存访问延迟使feed阻塞，下一次RX读取推迟；不需要假设AFE算法直接生成raw噪声。

**最小A/B：** 保留AFE全部参数，只暂停一个当次确实在运行的业务，首选animation，LVGL和网络不同时停。记录feed耗时、CPU1 idle、overflow与raw。若animation原本未运行，跳过该变量，选日志证实运行的一个UI刷新来源；无依据不测UVC。

**预期：** 只有该业务暂停后feed阻塞与raw连续性同步改善，才支持回压。若完整raw时间轴连续且频谱不变，应停止把调度当解释，回到业务启用与raw变化的时序边界，不调AFE阈值掩盖。

## 9. 简化建议的边界

本轮最有价值的简化方向是减少实时capture路径职责，接近参考的“只读、转换、feed”；但没有把它直接实施为新队列/新任务。现有accumulator已满足功能，不再另造一个。slot/mmr与78/156节奏保持；源码中“物理映射待确认”的历史注释已经过时，本轮不改代码，报告以最新实测为准。

根因尚未最终证明。相比上一轮，本轮把候选从“参数可能不一样”收敛到可测的**主动阻塞、CPU调度延迟、AFE回压**，并明确给出了其无法解释的初始化阶段限制。没有把源码参考冒充现场稳定性证明。
