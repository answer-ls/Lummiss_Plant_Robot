# 双麦 AFE 固件测试（2026-09-27）

本次只调整音频采集和 AFE 数据路由。尚未烧录实测，编译成功不代表识别效果已验证。

## 数据路径

- ES7210 读取四槽：SLOT0=MIC1、SLOT1=MIC3 播放参考、SLOT2=MIC2、SLOT3 未使用。
- 同步将 24kHz 四槽数据重采样为 16kHz 三通道，顺序为 MIC1/MIC2/参考（MMR）。
- MIC1/MIC2 增益保持 30dB；物理 MIC3 参考增益设为 0dB。
- 唤醒与对话复用同一个 AFE，开启 AEC，fetch 的单声道输出才交给 Opus。
- 待机只做本地唤醒处理；保持原有唤醒后上传、播放期间停止上传的会话行为。
- WakeNet 切换由 fetch 任务执行，不再从网络任务并发 reset AFE。
- 原有 3:2 重采样算法保持不变，只扩展为同步多通道并重排槽位。

## 修改位置

- `components/xiaozhi_audio/xiaozhi_audio.c`：硬件输入通道、参考增益、工作缓冲、重采样、采集任务、5秒诊断。
- `components/xiaozhi_audio/wake_word.c/.h`：MMR AFE、WakeNet 控制、单声道输出缓存及接口。
- `tools/test_dual_mic_pcm.py`：编译实际 C 重采样函数，验证双麦/单麦路径的 960 点相位、槽位和边界。

用户工作缓存使用 PSRAM：10ms raw 为 1920B，重采样/编码共用缓冲为 1920B，输出流缓冲 7681B；AFE 内部双麦/AEC 的额外资源以实机初始化日志为准。

## 烧录与观察

IDF 扩展当前配置的构建目录是 `build_rtc_mem_b`，已编译；`build` 也已编译。
当前构建记录统一存放于工程根目录 `logs/`。ELF 身份以最新构建的 SHA256 为准。

1. 烧录后确认 `MIC_ROUTE ... AFE=MMR`、`AFE_ROUTE input=MMR 16000Hz output=mono AEC=1`，检查 AFE pipeline 的实际配置。
2. 分别靠近两只麦克风说话，观察 `mic1/mic2/ref_peak`，确认两路都有采样变化。播放时观察参考，留意持续接近满幅的异常。
3. 唤醒并连续对话三轮（例如询问“现在几点了”）；对话监听时 `AFE_DIAG out_samples` 应增长、`out_drop=0`，`MIC packets` 应增长。
4. 待机和播报时上行不增长；结束后仍能再次唤醒。保存对应 STT 文本，比较实际说话与识别结果。
5. 再与 RTC 预览同时运行，观察内部内存、AFE feed/fetch、丢帧和 WDT。不能仅凭编译或通道峰值认定识别问题已解决。

主机验证：`python tools/test_dual_mic_pcm.py`（需要 gcc）；双麦与旧单麦路径均通过。

## 首次实机未唤醒的后续修正

首次日志确认两只麦克风有信号，AFE 为 2MIC/MMR，但未检测到唤醒词。
feed=3072 表示每通道 1024 点（64ms），fetch=512 点（32ms），因此 feed 次数约为 fetch 一半属于正常情况。
原 60ms 读取块积累到 64ms 时，周期性出现 120ms 的供数间隔，超过 fetch 的 100ms 超时。
已改为 10ms 读取块（Opus 仍为 60ms），理论最大供数间隔降至 70ms；初始化期间等到首个 feed 再开始 fetch。
此外只将有效音频计入 fetch，新增 fetch_err/pcm_samples/pcm_peak/vad 统计，区分输入有声与 AFE 输出有效。
这些修改修复供数节奏和诊断缺口；尚不能断言周期性超时是无法唤醒的唯一原因，需新固件实测。

## AFE 输入守恒诊断

保持 MMR、AEC/BSS/VAD/WakeNet、任务优先级不变。采集为 10ms，Opus 仍为 60ms。
原 `feed=3072` 是交错 int16 样本数，不是字节数；新 `AFE_CHUNK` 分开打印 API 返回的每通道点数、交错点数和字节数。
chunk 必须遵循运行时 `get_feed_chunksize()`，不强制写死 512：返回 1024 时每次 feed 是 6144B，理论约 78.125 次/5s；返回 512 时是 3072B，理论约 156.25 次/5s。

新增 `AFE_FLOW` 每约 5s 汇总，样本统一按每通道计：
- `input_24k_samples` 约 120000，`resampled_16k_samples` 约 80000（按 `window_ms` 折算）。
- `afe_feed_calls` 为成功 feed 次数，失败另见 `AFE_DIAG feed_fail`。
- `afe_feed_samples + accumulator_remaining_samples` 应等于本窗重采样输入加上窗余量，正常 `balance=OK`。
- `afe_fetch_calls` 只统计有效 PCM；`ringbuffer_underflow` 表示 fetch 返回空、ESP_FAIL 或无数据的次数，是公开 API 可见的欠数指标，不是组件内部日志钩子。
- `wake_detect` 为本窗唤醒事件计数。

主机验证：`python tools/test_afe_accumulator.py` 直接编译实际 feed 函数，用 512/1024 两种 chunk，分别喂入 160/960/2048/4096 点批次，逐样本核验顺序与跨批次余量；8 组均通过。该测试不能代替板端时钟、实时调度和唤醒效果验证。
