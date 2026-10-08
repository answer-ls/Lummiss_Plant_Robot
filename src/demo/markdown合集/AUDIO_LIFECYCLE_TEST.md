# 原始采集生命周期诊断（当前测试）

不修改AFE、重采样、MMR、Opus、增益、时钟格式或任务优先级。关闭上一轮audio_probe_start，只新增原始RX旁路记录和只读寄存器快照。

当前顺序：audio_hw_init完成 → AUDIO_INIT_DONE → TX_NO_APP_WRITE → TX_ZERO → TX_PLAY_OUTPUT（低幅1kHz）→ 恢复TX开启并发送静音 → 正常语音任务 → WAKE_IDLE/LISTENING/SPEAKING/POST_TTS/CONTINUOUS_LISTENING。

**这是带启动TX对照的诊断运行，不等同于未经扰动的自然启动。** 不能把经历了TX试验后的WAKE_IDLE与原始初始化简单视为只差一个语音状态。若发现启动TX操作引入变化，先定位该操作，再决定独立的无TX对照复测。

## 能执行与不能执行的对照

- TX_OFF：当前架构不支持。TX先初始化，ESP-IDF i2s_tdm.c 的双工配置将后初始化的RX切成slave，依赖TX主时钟。此前停止TX造成RX超时。此次不停止TX或RX、不切换时钟主从。
- TX_NO_APP_WRITE：替代对照的真实名称。保持TX主时钟和DMA运行，应用不调用write；现有auto_clear_after_cb保持开启。采集1秒真实RX；不能据此宣称TX硬件停止，也不能认为它与主动发送全零在物理数据线上必然有差异。
- TX_ZERO：TX开启并持续发送全零PCM，同时读RX。
- TX_PLAY_OUTPUT：相同长度1kHz正弦，通过原输出配置发送；没有改DAC音量。真实扬声器发声仍由现场确认。
- TX正常播放但功放mute：当前板BOARD_HAS_PA_GPIO=0，CTRL未连接P4 GPIO，**软件无法独立实现**，日志标记PA_MUTED=UNSUPPORTED。未以DAC静音或全零数据冒充。

## 窗口与快照边界

每个状态/事件第一次出现后保存24000帧（1秒，四槽16bit，共192000字节）。不更改原语音状态机停留时间；若短状态在1秒内离开，crossed>0，录音明确属于跨状态窗口。POST_TTS是播放排空后的事件，不是独立持续状态，可能与连续监听窗口高度重叠。缺失状态保留空文件、mark=0；不补零，不视为完整测试。

状态事件精确记录软件单调时钟。I2C快照由独立任务延后读取，BEFORE表示请求开始快照，不保证它在首次样本之前；须看快照实际时间与LC_EVENT。接收完成时间不等于ADC转换时间，已有DMA队列可能包含切换前数据。短状态或明显调度延迟时，只能缩小范围，不能武断认定某一条指令是根因。

每个窗口开始/结束保存ES7210实际I2C寄存器（负值表示读失败），P4 I2S RX/TX conf/conf1/TDM，及HP_SYS_CLKRST的ctrl11..14。没有写ES7210寄存器。运行期间raw取自正常esp_codec_dev_read完成后的缓冲，保持原读取链；启动独占测试使用直接i2s_channel_read检查实际字节数。

## 执行

用IDF扩展烧录build_rtc_mem_b（build也同步）。关闭串口监视器，在src/demo目录执行：

```powershell
& 'C:\Espressif\tools\python\v5.5.5\venv\Scripts\python.exe' .\tools\capture_lifecycle.py --port COM7
```

脚本连接后按P4 RESET。启动短暂测试音后，正常唤醒、对话、回答结束后继续说话。不要开启RTC。首次连续监听录音完成后至少35秒导出；若缺少状态，最多等3分钟后导出已有结果。串口导出约3～5分钟。所有文件在根目录logs/lifecycle_时间。

每组有tdm_raw.bin、raw_4slot_24k.wav、slot0.wav至slot3.wav；lifecycle.json保存事件/寄存器快照/短读/跨状态标记；serial.log保存完整日志。

离线分析（提供numpy/scipy/matplotlib的Python）：

```powershell
& 'C:\Users\Lenovo\.cache\codex-runtimes\codex-primary-runtime\dependencies\python\python.exe' .\tools\analyze_lifecycle.py E:\Lummiss_Plant_Robot\logs\lifecycle_实际时间
```

生成comparison.json及各组waveform_spectrum.png：逐槽RMS/peak/DC、300Hz以下功率、37/47/53Hz±1Hz峰值与功率、相对初始化及窗口内寄存器变化。只对完整1秒数据比较同窗频谱；I2C读失败不能解释成寄存器真实变化。谱峰本身不能单独认定噪声，需要对应试听和状态时间。


## 本轮时序/VAD诊断

VOICE_TRACE的us是本次启动的绝对单调微秒，不是UTC。WAKE_DETECTED时尚无新session，标记PENDING，用wake序号关联SESSION_BOUND后的session_id；round区分同一会话的连续监听。记录preroll起止、成功包数、listen/detect和start本地发送结果、上行使能、首包实时Opus、首条匹配session的STT、TTS_START_RX及SPEAKING_ENTER。未匹配session的STT单列，不作为本轮首条。

客户端无法证明服务端具体消费了哪些音频：若STT早于首包live，可排除本轮live来源，但仍须区分preroll、detect文本或服务端缓存/欢迎语。若STT晚于live，两种来源都可能；需要服务端日志最终确认。不为制造长LISTENING而延迟TTS或改变状态机。

VAD_EDGE仅记录有效AFE结果的真实转换。VAD_DIAG每5秒列speech/silence帧数及begin/end次数和valid_fetch。状态机的30秒结束条件基于STT，不依赖本地VAD；若有效fetch持续且全为speech，表示AFE持续判speech；若无有效fetch，状态可能只是旧值。没有改变VAD算法或门控。

分析器仅对完整且crossed=0窗口输出频谱。pure_state_metrics.csv包含两麦RMS/DC、37/47/53/50/60Hz±1Hz积分RMS幅值（PCM counts）、三个频带能量占比及RMS比值；comparison.json还包含绝对频带均方能量和所有快照差异。历史目录内旧PNG不代表本轮纳入比较，以pure_state_comparison字段/CSV为准。
