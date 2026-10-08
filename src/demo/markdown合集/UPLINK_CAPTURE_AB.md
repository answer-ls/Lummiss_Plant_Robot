# 同步上行阻塞采集：一次性诊断固件

本轮不调整音频算法、增益、通道、DMA参数或已有任务的核/优先级。
暂时关闭旧 lifecycle/audio_probe 诊断，避免它们只在首轮录音或导出，污染A/B。
新增统计和原始音频缓存均在PSRAM；ISR只更新内部RAM原子计数。

## 操作

1. 使用IDF扩展烧录 `build_rtc_mem_b` 中本次固件；也已维护 `build`。
2. 关闭串口监视器。在 `src/demo` 执行：

```powershell
& 'C:\Espressif\tools\python\v5.5.5\venv\Scripts\python.exe' .\tools\capture_uplink_ab.py --port COM7
```

3. 按P4 RESET。分别进行两次唤醒对话；每次回答结束后，在相同位置播放相同固定声源至少5秒。两次都必须进入 CONTINUOUS_LISTENING。
4. 每次进入该状态500ms后开始4秒窗口。第一轮A真实发送，第二轮B只将UDP调用替换为packet/bytes计数；AFE和Opus照常。
5. 两段完成后自动导出，等待 `CD_DONE`，导出期间不属于测量。不要复位；约需数分钟。

串口和音频统一保存至根目录 `logs/uplink_ab_时间/`。
每个A/B子目录包含：原始 `tdm_raw.bin`、4声道24k/16bit WAV、四个单槽WAV、`reads.csv`、`summary.json`。
raw未经resampler、MMR、AFE或Opus。录制按照同一4秒墙钟目标；实际样本数和实际窗口时长保留，不通过补零或裁剪隐藏丢样。

## 统计口径与失效条件

- read时间戳位于 `esp_codec_dev_read()` 调用边界。此API包裹I2S读取；不宣称读满1920字节等于采样连续。
- `read_interval_us = next_begin - begin`；`post_read_processing_us = next_begin - end`，后者包含锁等待、处理、发送和任务抢占。
- 阶段耗时为每次调用的墙钟耗时，含抢占；并非纯CPU执行时间。排序计算精确nearest-rank P95/P99，仅在窗口封存后进行。
- `afe_feed_us`测量采集任务调用`wake_word_feed()`的总耗时，包含现有累积器和实际feed；不是仅对AFE库内部调用计算的平均值。
- `udp_send_us`测量整个`cloud_udp_send_opus()`，包括其内部锁等待、加密/打包和socket发送，不能单凭这一项进一步断言是socket阻塞。
- `rx_overflow_count` 为窗口内 `on_recv_q_ovf` 回调次数，不等于丢失样本数；不代表窗口外不存在溢出。
- A packet_count为真实UDP调用数；B为本地替代调用数。原有业务计数维持原行为，不能用其证明B实际发包。
- 跨状态、A发送错误、B混入窗口边界的真实发送、读失败、缓存截断、没有上传包均不能成为有效对照；串口丢块或hash不符直接报错。
- 无效窗口不自动重试；重新启动采集。固定声源一致性仍须操作者保证。
- 诊断仅改变指定窗口内UDP调用；B窗口外立即恢复真实发送。

numpy存在时工具计算RMS/DC/FFT峰和低于300Hz能量比例；没有numpy仍保存全部音频，明确标记频谱未计算。可在带numpy的Python中用 `--analyze <采集目录>` 离线重算。

只有有效对照中B的间隔/overflow明显下降，并且raw音质同时改善，才支持同步网络发送影响采集的假设。单独UDP耗时长、ESP_OK或读满长度，都不构成因果证据。

## 本轮代码位置

- `capture_diag.c/.h`：窗口、时间序列、RX回调、计数和导出。
- `xiaozhi_audio.c`：原处理路径外围测时，RX回调注册，B窗口替代UDP调用。
- `audio_probe.c`：本轮暂停旧探针；生命周期探针在初始化调用处暂停。
- `CMakeLists.txt`：加入诊断源文件。
- `tools/capture_uplink_ab.py`：接收/校验/保存/分析。
- `tools/test_capture_uplink_ab.py`：离线校验串口完整性和无效窗口处理。

这是临时诊断固件；两轮之后不再启动新的A/B，统计缓存留至重启。不得把尚未实测的最大耗时或音质变化写成已验证结果。
