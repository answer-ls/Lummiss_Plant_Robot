# ES7210 官方参考与当前工程审计

审计日期：2026-09-27。本轮只读源码、比较已有实机记录，未修改固件、未编译或烧录。

## 证据边界

**本板实测映射固定为 SLOT0=MIC1、SLOT2=MIC2、SLOT1=REF，SLOT3不用。** 官方板的接线、增益掩码和 slot 注释不能覆盖本板实测。重排仍为 `[S0,S2,S1] -> [M1,M2,R]`。物理 ADC 增益 mask 与 TDM slot mask 是两种编号，不能互换。

接受已验证的 WAV header、独立通道重采样、MMR 拆分和延迟对齐后的 AFE→uplink 一致性。不重新修改 AFE/WakeNet/AEC/BSS，也不把本轮结果解释为已经排除所有采样连续性问题。

参考版本：

- [xiaozhi-esp32 固定提交](https://github.com/78/xiaozhi-esp32/tree/8ce50d27cd7c72c777673f46cbfc3ef7454d1b5c)，提交时间 2026-09-26，以下“官方”指此版本的 BOX/BOX3/BoxAudioCodec。
- [BoxAudioCodec 源码](https://github.com/78/xiaozhi-esp32/blob/8ce50d27cd7c72c777673f46cbfc3ef7454d1b5c/main/audio/codecs/box_audio_codec.cc)。官方 manifest 声明 esp_codec_dev `~1.6.2`、IDF `>=6.0.1`；不是官方某台实机的锁定构建。
- 当前工程 esp_codec_dev 1.5.11、ESP-IDF 5.5.5、ESP32-P4。对照 [1.6.2 注册版本](https://components.espressif.com/components/espressif/esp_codec_dev/versions/1.6.2) 对应源码提交 `af7b72fb2b73d4f513d3cede01c13518ab216735`。
- 参考源码及差异保存在 `logs/es7210_official_audit/`。本地 ES7210、ES8311、I2S data interface、codec core 等六个核查文件与上游 1.5.11 一致；其中 ES7210 源码/头文件、I2S data interface 在参考 1.6.2 中也相同。不能直接归因于“本地改坏了 ES7210 驱动”。

## 1. 配置逐项比较

位置缩写：

- O：参考 `main/audio/codecs/box_audio_codec.cc`：构造、`CreateDuplexChannels`、`EnableInput/EnableOutput`。
- OB：参考 `main/boards/espressif/esp32-s3-box/config.h` 和 `esp32-s3-box-3/config.h`。
- P：当前 `src/demo/components/xiaozhi_audio/xiaozhi_audio.c`，`audio_hw_init()`，1091行起。
- D：`src/demo/managed_components/espressif__esp_codec_dev/platform/audio_codec_data_i2s.c`。
- E：同组件 `device/es7210/es7210.c`。

风险表示误配影响/后续核查优先度，**不是已证明故障概率**。

| 项目 | 官方 BOX/BOX3 | 当前工程实际配置 | 是否一致 / 风险 |
|---|---|---|---|
| 主控、IDF | S3；manifest >=6.0.1 | P4 rev3.x；5.5.5 | 不同，MEDIUM；同宏不等于同硅实现 |
| 输入/输出 sample_rate | OB：24000/24000 | P：`XIAOZHI_CODEC_SAMPLE_RATE=24000` | 一致，LOW |
| RX data bits | O：16 | P：16 | 一致，LOW |
| RX slot bits | O：AUTO，按16bit数据解析为16 | P：AUTO→16；实机寄存器也为16 | 一致，LOW；不是4×32 |
| RX slots | mask0xF、total AUTO→4 | total=4、mask0xF | 一致，LOW |
| RX协议 | Philips TDM、STEREO配置、4槽帧 | 同 | 一致，LOW |
| TX协议 | STD Philips；初始16data/16slot/2slot | 同；codec打开后16data/32slot/2slot | 两侧codec共享帧宽协调，LOW |
| 总线主从 | I2S0 master；ES7210/ES8311 slave | 同；P4 RX内部跟随TX共享时钟 | 一致的设计；TX停用风险HIGH |
| RX/TX controller | 同一个I2S0、同一个data_if | `XIAOZHI_I2S_PORT=I2S_NUM_0`，同一个data_if | 一致，LOW |
| MCLK来源 | I2S GPIO MCLK，DEFAULT时钟源 | GPIO28，DEFAULT；当前P4版本对应PLL默认源 | 板/SoC不同，MEDIUM；未实测频率 |
| mclk_multiple | 256 | 256 | 一致，LOW |
| MCLK理论 | 24k×256=6.144MHz | 同 | 计算值，不是仪器值 |
| BCLK理论（稳定配置） | RX4×16=64bit/帧；1.536MHz | RX4×16，TX2×32；1.536MHz | 一致，LOW |
| LRCK/WS理论 | 24kHz | 24kHz | 一致，LOW |
| RX bclk_div字段 | 8 | 8 | 一致；不能把它直接当实测MCLK/BCLK比 |
| RX ws_width | AUTO半帧→32bit clock | 同；寄存器32 | 一致，LOW |
| TX ws_width | 初始16，codec协调后32 | 初始16，实机稳定32 | 一致的适配机制，LOW |
| bit_shift | true，Philips一位延迟 | true | 一致，LOW |
| BCLK/WS/MCLK inversion | 全false | 全false | 一致，LOW；边沿波形未测 |
| ws_pol / bit_order_lsb / big_endian | false / false / false | 同 | 一致，LOW；MSB先行 |
| RX left_align | 初始false；D重配时true | 同 | 不是本项目独有差异 |
| RX skip_mask | false | 默认false | 一致 |
| DMA descriptor/frame | 6 / 240 | 6 / 240 | 一致，LOW |
| codec输入mask | channel=4，mask=0x3；应用取2通道 | channel=4，mask=0xF；应用取完整4槽 | 不同但符合本板双麦，不能改成官方mask |
| 应用输入/AFE | BOX默认1麦+REF，两通道MR | 2麦+REF，三通道MMR | 预期差异；不能据此改回MR |
| ES7210使能 | MIC1/2/3/4 | MIC1/2/3，MIC4不使能 | 不同，LOW；两者都超过2路，进入TDM |
| 两实体麦PGA | 默认BOX只指定物理MIC1=30dB；非双麦平衡配置 | MIC1=30、MIC2=30、MIC3参考=0 | 不可照抄，MEDIUM |
| ES8311 PA补偿 | 5.0/3.3参数 | 3.3/3.3，0dB补偿，无PA GPIO | 不同，影响播放/参考电平；不证明raw静音噪声根因 |
| codec打开时机 | 输入/输出第一次使用时各自open | 启动时固定先输出后输入open | 不同，MEDIUM，见调用链 |
| 采集任务工作 | 输入与Opus/网络队列分工 | capture路径包含重采样/AFE feed以及上行处理 | 不同，MEDIUM；需连续性证据 |

当前引脚来自 `src/demo/components/board/include/board_pins.h`：MCLK28、BCLK29、WS30、DOUT31、DIN32、SCL33、SDA34。保持不变。

### ES7210 寄存器配置

E 中 `es7210_open/set_fs/set_bits/mic_select` 决定以下行为，比较过的两版 ES7210 源码一致：

| 字段 | 官方参考驱动 | 当前实际 / 证据 |
|---|---|---|
| TDM enable | 超过2个启用MIC写0x12=0x02 | 三路启用；回读0x12=02 |
| 16bit / normal I2S | `set_bits(16)`，normal格式低位 | 回读0x11=60 |
| master/slave | 零初始化master_mode=false | 显式false；0x08=10，master bit未置位 |
| MCLK选择参数 | 板级未显式指定，零初始化；驱动只在master分支处理选择 | 显式PAD、div256；slave模式不会执行master分频表；不据此认定差异 |
| HPF | 驱动固定0x20=0A、21=2A、22=0A、23=2A | 实读相同；官方板未另行配置HPF；不能仅凭值宣称某截止频率 |
| MIC bias | 驱动0x41/42=70 | 实读相同；驱动注释称2.87V，非本板测量 |
| 模拟配置 | 驱动0x40=43 | 实读43 |
| PGA MIC1/2 | 0x43/44，低4bit为增益，bit4为启用 | 实读均1A：启用+30dB |
| PGA参考MIC3 | 0x45，同上 | 实读10：启用+0dB |
| MIC4 | 官方启用；当前不用 | 当前0x46=00 |
| ADC digital gain/volume | 官方代码未显式配置独立数字增益 | 当前也无独立数字音量设置；不能把PGA寄存器当数字增益 |
| 输入选择/映射 | MIC启用mask；板级输入不通用 | 当前物理映射以实测0/2/1为准；未发现运行时切换ADC输入源 |

注意官方增益调用顺序：`es7210_codec_new()`内曾写全通道30dB；但之后`esp_codec_dev_open()`的`_update_codec_setting()`会应用新device的默认全局mic_gain=0，再由`BoxAudioCodec::EnableInput()`单独把物理MIC1设置30dB。BOX默认`reference_gain_channel=-1`，不会另调参考。上述为源码推导，**不是官方板寄存器实读**，不能把构造阶段的30dB当最终四路增益。

## 2. 真实初始化与生命周期

官方：

```text
Application启动 → board.GetAudioCodec() → BoxAudioCodec构造
  CreateDuplexChannels
    new_channel(TX+RX, I2S0 master)
    init TX STD → init RX TDM → enable TX → enable RX
  创建共享data_if
  创建ES8311 DAC codec + output device
  创建ES7210 codec + input device
AudioService::Initialize → codec->Start（读取音量等，不等于codec open）
AudioService::Start → 音频任务运行
EnableWakeWordDetection → InitializeAudioEngine（AFE按需初始化）
首次ReadAudioData → EnableInput(true) → codec input open → 设置MIC1增益
首次AudioOutputTask播放 → EnableOutput(true) → codec output open → 设置音量
```

首次输入和输出谁先发生取决于启动提示音/采集调度；不能画成官方恒定“input open先于output open”。构造codec则明确**先ES8311，后ES7210**。I2S RX/TX在构造阶段已启用，逻辑device启用是另一层。

当前：

```text
app_main完整业务：网络/OTA等已启动
xiaozhi_audio_start → wake_word_init（当前AFE）→ 创建service task
service_task → audio_hw_init
  new_channel(TX+RX) → init TX STD → init RX TDM → enable TX → enable RX
  创建共享data_if和I2C控制接口
  创建ES8311 codec → 创建ES7210 codec
  创建output/input device
  open output（16bit mono 24k）→ open input（4×16bit 24k maskF）
  两麦30dB，参考0dB，输出音量
→ lifecycle_prepare启动窗口 → audio_codec_init（这里是Opus）
→ capture/playback等任务 → lifecycle_start
```

主任务和service异步，因此UI等启动可能与`AUDIO_INIT_DONE`窗口重叠。此窗口不是“全机仅ADC”的实验环境。

**共享时钟的关键点：** D 的`set_fs/check_fs_compatible/get_bits`协调双工帧位数。输入4×16=64bits，输出2槽最终调整成每槽32bits，仍是16bit有效数据。当前实机 `rx_conf1`解码为data16/slot16/ws32，`tx_conf1`为data16/slot32/ws32。因此不能仅看到TX32bit就判RX按32bit解包，也不能把初始TX16slot视为生命周期最终值。

官方TTS开始本身没有一套固定重配动作；若是首次逻辑开启输出，则底层open可disable/reconfig/enable。输出已经启用时只是写PCM。官方功耗管理在双工输入仍启用时不关闭输出；D另有`out_disable_pending`，避免关闭TX导致RX失去共享时钟。不是每次TTS结束恢复另一套采样率。当前正常会话也没有每次TTS修改硬件格式，详见第二份报告。

## 3. 已有实机readback自动比较

来源：`logs/lifecycle_20260927_173525/lifecycle.json`，不是重新读取板子。比较结果：`logs/es7210_official_audit/es7210_readback_diff.json`。可读全寄存器导出：同目录 `ES7210_REG_DUMP.md`。

- 9阶段×前后，共18份快照；已采地址无I2C读取失败。
- ES7210已采寄存器全部相同，包括0x43=1A、44=1A、45=10、46=00。
- I2S/clock已采10个寄存器除失败TX_OFF窗口的TX启动位外，全部相同。
- 采样寄存器范围00..0D、10..23、40..4C；不是全芯片所有寄存器，也不是无间断追踪，不能排除两次快照之间的短暂扰动。
- TX_OFF bytes=0/error263，无有效录音。当前源码已使用诚实名称`TX_NO_APP_WRITE`，不能用旧数据充当新测试结果。
- LISTENING/POST_TTS crossed=1，不作纯状态频谱比较。

| 有效纯阶段 | MIC1 RMS | MIC2 RMS | MIC2/MIC1 | 寄存器变化 |
|---|---:|---:|---:|---|
| AUDIO_INIT_DONE | 77.0 | 170.2 | 2.21 | 无 |
| TX_ZERO | 101.5 | 217.7 | 2.15 | 无 |
| TX_PLAY_OUTPUT | 26.2 | 100.2 | 3.83 | 无 |
| WAKE_IDLE | 131.4 | 291.6 | 2.22 | 无 |
| SPEAKING | 144.2 | 294.3 | 2.04 | 无 |
| CONTINUOUS_LISTENING | 307.9 | 611.6 | 1.99 | 无 |

完整37/47/53/50/60Hz频带幅度、DC、分段能量在原目录`pure_state_metrics.csv`。这些是该分析定义下的频带RMS，并非示波器单频峰值。初始化窗口已低频集中；不能宣布“杂音第一次出现在TX_ZERO或TTS后”。非相同声学输入下，RMS升高也不能直接等同噪声底升高。

## 4. 官方双麦资料能证明什么

BOX当前默认实现是MR，不是当前MMR的直接替代。可参考[双麦issue #2229](https://github.com/78/xiaozhi-esp32/issues/2229)与[PR #2230](https://github.com/78/xiaozhi-esp32/pull/2230)。审计时PR为**open、未合并**，不能称作官方已验证修复。其可取之处是区分physical MIC gain mask与slot重排；其中板级mapping不能搬到本板。

另核查Waveshare厂商1.75板参考：其`XiaozhiAudioProcessor.cpp`有MMR，`bsp_extra_codec_set_voice_fs()`有close/open流程。它是另一块板、另一套业务及驱动组合；“用了相同ES7210”不构成配置等价证据。

## 5. 三类结论与下一步（未实施）

### 关键差异

官方BOX是单麦+参考、按需open；当前是实测双麦+参考、启动即同时open。主控/IDF不同，业务并发与采集任务职责不同。关键4×16 TDM、24k、256×MCLK、shared I2S和ES7210基础寄存器没有找到决定性错配。

### 症状解释能力

- 两麦约2倍：实读PGA同为30dB，不能再解释成某路软件多写6dB。幅度差在raw存在，但仅凭当前数据不能定位到哪个模拟环节，也不能归罪MMR。
- 完整业务低频增大：运行环境和采样连续性有待验证；没有发现状态切换持久改写ES7210/时钟的证据。初始化窗口已低频集中，排查应早于TTS状态切换。
- TX_ZERO与连续监听RMS增大：观测成立，因果未证实。需要同声源、同距离、同长度、静音段对照；不能凭不同说话片段认定TX写PCM改变了ADC增益。

### 优先验证的三个单变量

1. **专用直采与完整业务的运行环境差异。** 固定同一audio_hw_init、slot、增益、采样长度和声学输入；从纯采集基线逐次只启用一个业务模块，记录该模块启动前后raw及寄存器。先找最早变坏的模块边界，不先变采样格式。当前业务初始化窗口就有异常特征，这是优先于TTS的原因。
2. **共享TX的“无人写入”与“主动全零写入”。** RX/TX controller及格式保持，比较`TX_NO_APP_WRITE`和`TX_ZERO`，同时核实TX自动清零状态、RX读满/溢出/采样累计。绝不调用TX disable冒充合法独立关TX实验。若两组不同，继续定位TX队列/时钟运行状态；若相同，降低这条假设优先级。
3. **PGA等值而幅度不等的可重复性。** 相同固定声源采集基线，再只将物理MIC1 PGA降低一个3dB档并实际回读，MIC2/REF/slot/通道enable不变，比较各槽幅度响应。该实验验证增益作用对象与传递关系，不是用AGC掩盖差异；不使用曾令全部slot归零的disable方案。

尚未找到三个“确定的软件配置错误”；上述是按现有证据排序的验证，而非一次性修复建议。本轮未新增固件日志，因为用户明确要求先审计不改代码；已有真实readback已自动diff，后续缺失时序数据需单独诊断构建。
