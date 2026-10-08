# 同一次语音会话的逐级录音

当前固件为完整系统 CAMERA_TEST_FULL，双麦配置保持不变；不执行软件disable单麦实验或物理槽位定位播放。首次回答结束进入连续监听后，同一8秒观察窗口保存各级数据。不要同时开启RTC。

## 操作

烧录 IDF 扩展目录 build_rtc_mem_b（build 也同步编译）。关闭原串口监视器，在 `E:\Lummiss_Plant_Robot\src\demo` 执行：

```powershell
& 'C:\Espressif\tools\python\v5.5.5\venv\Scripts\python.exe' .\tools\capture_audio_probe.py --port COM7
```

脚本连接后手动按 P4 RESET。唤醒小智并进行一次对话，首次回答结束后立即说同一句测试语句，例如“现在几点了”。录音8秒，随后27秒继续观察，最后串口导出约8～10分钟；不要关闭脚本。每次重启仅录一次。

结果在根目录 logs/audio_probe_时间，包含：

- tdm_raw.bin、raw_4slot_24k.wav：未经重采样的交织四槽，4ch/24kHz/16bit/block_align=8。
- slot0_24k.wav 至 slot3_24k.wav：PC按8字节帧步长拆槽。
- mic1_16k_before_afe.wav、mic2_16k_before_afe.wav、ref_16k_before_afe.wav：固件在AFE入口前同步保存。
- mmr_16k_3ch.wav：3ch/16kHz/16bit/block_align=6。
- afe_mono.wav、uplink_pcm.wav：AFE返回数据、实际提交Opus编码器的PCM。
- timing.json、stage_timestamps.json：各级首末时间、每块字节偏移/长度/单调时钟、100帧抽样。
- stage_verification.json：WAV头、按独立通道复算重采样、MMR拆分、100帧抽样比较结果。

串口各流仍有长度/偏移/FNV校验。所有 resample_mismatch/mmr_split_mismatch/layout_mismatch 必须为0。

## 时间边界

raw与before_AFE由同一次capture调用、同一个锁、同一时钟判定窗口一起提交，保证逐批对应；不拿不同录音做前后比较。

AFE和Opus有自身缓冲/算法延迟，采用同一个墙钟观察窗口，但不宣称各文件样本0对应相同声学时刻。stage_timestamps.json保留每块信息，比较时必须对齐并取共同有效区间。若本轮未产生uplink，空文件不能用于评价音质，也不能填零伪造对齐。100个抽样帧在录音结束后打印，避免干扰采样。

## 当前代码审计

| 路径 | 原始读取 | 布局/步长 | WAV |
|---|---|---|---|
| 旧业务录音 | esp_codec_dev_read → data_if.read → i2s_channel_read | int16，4槽，每帧8字节；audio_probe_raw在重采样前 | PC 4ch/24k/16bit；槽偏移slot×2、步长8 |
| 直采测试 | 直接i2s_channel_read，检查返回字节数与RX溢出 | int16，4槽，每帧8字节 | PC拆槽，4ch/24k/16bit |
| 本轮业务录音 | 保留正常codec读取路径；raw与MMR同批缓存 | raw[4*n+0..3]=SLOT0..3；MMR[3*n+0..2]=历史路由槽0/2/1 | 两种格式分别明确写头 |

旧codec数据封装仅转交I2S，未做int32→int16变换或重排；它不向调用者暴露bytes_read，重配置期间还存在填零并返回OK的路径。仅凭源码存在这些路径不能认定本次曾触发。

当前重采样每三个24k源帧生成两个16k目标帧：某通道输出为该通道第一个样本、第二与第三样本的整数平均。每通道取自己的槽，没有把3ch交织数组作为单声道整体重采样。此次保留算法，用PC先拆单通道再独立复算（包括负数向零截断），与固件输出逐样本核对。

内存布局中MIC1/MIC2/REF是历史软件路由名称，并非已经完成物理映射认证。输出格式和布局回读在首次回答之后打印；实际RX不是16bit/16bit槽/4槽时拒绝生成误标WAV。

旧录音 logs/audio_probe_20260927_153937 已离线复核：四槽头为4ch/24k/16bit，共192000帧；旧mic1/reference/mic2/unused分别与原四槽WAV对应槽逐字节相同（证据 logs/audio_stage_path_audit.json）。因此旧文件的杂音不能直接归因于PC拆槽或WAV头单声道误标。旧raw记录点位于重采样之前，不能据新的独立直采正常就确定故障一定在重采样之后；仍需本轮同一运行窗口数据定位。

## 判读

先试听同一时间段的slot0/2与对应before_AFE。若raw清楚而before_AFE第一次异常，结合复算结果定位到重采样处理；若before_AFE和MMR均正常而AFE异常，再检查AFE输入解释/状态，而非直接修改参数。若本轮raw已异常，应回查正常业务运行时的输入状态，不能忽略这个反证。单纯低于300Hz能量高也可能来自人声基频，不独立作为噪声结论。

本轮没有修改WakeNet/VAD/AEC/BSS参数，也没有调整采样率、增益、时钟或任务优先级。
