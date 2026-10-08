# ES7210 原始噪声隔离测试

> 此页为上一轮隔离实验记录。按最新要求，当前改为完整系统逐级录音，执行步骤以 AUDIO_STAGE_TEST.md 为准；不再运行 A/B disable 实验。

本固件启动档位为 `CAMERA_TEST_AUDIO_RAW`（13）。仅调用现有音频硬件初始化，然后由诊断任务独占 `i2s_channel_read`。不启动正常小智采集、MMR 排列、重采样、AFE、Opus、UVC、LCD/UI 或 SD 读写。ESP-Hosted 的组件启动钩子仍可能创建任务，不能把本测试称为完全关闭 Hosted 的实验。

## 执行

1. 用 IDF 扩展烧录 `src/demo/build_rtc_mem_b`。`src/demo/build` 同样提供此测试固件。
2. 退出 IDF 串口监视器，释放串口。
3. 在项目根目录运行（COM7 换为当前 P4 串口）：

```powershell
& 'C:\Espressif\tools\python\v5.5.5\venv\Scripts\python.exe' .\src\demo\tools\capture_raw_adc.py --port COM7
```

4. 脚本显示等待后，手动按 P4 RESET。四组依次为 BASE（当前三 ADC 配置）、A_MIC1、B_MIC2、C_MIC12。
5. 每组有10秒准备时间。看到 `RECORD_BEGIN` 后，前2秒安静，之后在相同位置、相同音量说同一句话，录音共8秒。不要播放扬声器音频。
6. 四组完成后才导出，115200 波特率约13分钟。脚本隐藏大批 base64 行，保留进度和原始日志。等待“四组完整导出”。

文件位于根目录 `logs/raw_adc_时间/`，每组包含 `SLOT0_raw_24k.wav` 至 `SLOT3_raw_24k.wav`、原始交织 WAV 和 timing.json。`serial.log` 保存实际 I2C/MMIO 回读和导出数据。校验失败或 RX_OVERFLOW 非零时不能视为有效对照。

## 单变量边界

- 不调用 codec 的按麦克风数量重新配置接口函数，四槽 TDM 参数保持不变。
- 只修改 0x43..0x46 的 ADC/PGA 通道使能位和 0x4B/0x4C 的单通道电源/复位位；每次写入立即 I2C 回读验证。
- PGA 增益、数字音量、HPF、接口格式及分频不变；共享 MIC bias/VREF 保持基线。REF 的 ADC/PGA 关闭，但不为了关闭它而改变共享偏置。
- 结束或出错时恢复被修改的寄存器。禁用通道的数字输出不能预先假定为零，需要结合四槽实际录音观察。
- 四槽输出不预先标 MIC1/MIC2/REF；由 A/B 隔离结果验证真实槽位。

## 格式与时钟证据

`REG` 全部来自 I2C readback，覆盖 0x00..0x0D、0x10..0x23、0x40..0x4C。包括 PGA、ADC 控制、ALC/数字音量、HPF、MIC bias、通道电源和时钟接口配置。

`P4_READBACK/P4_FORMAT` 来自 I2S0 寄存器；`P4_CLOCK_READBACK` 来自 HP_SYS_CLKRST；`P4_GPIO_READBACK` 来自 GPIO matrix。16bit 数据、16bit槽、4槽、槽掩码0xF不符时停止，避免错误标记 WAV。

当前代码请求 24kHz、MCLK 倍数256；理论 MCLK=6.144MHz、4×16bit BCLK=1.536MHz。TX 与 RX 共用时钟，必须结合运行时 TX/RX 寄存器，而不能只看初始化宏。ES7210 0x11 解码数据位宽、格式和 LRCK 反相，0x12 解码 TDM 模式；槽时钟长度不等同于数据位宽。

当前没有逻辑分析仪或示波器，因此实际 MCLK/BCLK/LRCK/DIN 频率、边沿、延迟和信号对齐仍未实测。寄存器与采样耗时只能检查软件设置及吞吐，不能替代波形验证。

寄存器含义依据 [ES7210 数据手册](https://files.waveshare.com/wiki/common/ES7210_DS.pdf) 和项目安装的 esp_codec_dev 驱动。保留寄存器原值便于复核。

## 离线比较

在具有 numpy/scipy 的 Python 中运行：

```powershell
& 'C:\Users\Lenovo\.cache\codex-runtimes\codex-primary-runtime\dependencies\python\python.exe' .\src\demo\tools\analyze_raw_adc.py .\logs\raw_adc_实际时间
```

输出 metrics.json：全段和前2秒静音段分别统计 RMS、DC、剔除 DC 后300Hz以下功率比例、Welch 频谱峰、削顶数。直接试听未处理 WAV。Welch 使用1秒窗，峰值分辨率约1Hz。

- A/B 都异常：问题最早已在 I2S 原始 RX 或其上游，继续区分公共 ADC/时钟/格式/模拟输入。
- A/B 正常、C 异常：优先检查多通道配置/槽位交互；不能单凭一次说话内容差异定因。
- 仅一路异常：优先核对该 ADC/PGA 及输入通道。
- 原始数据正常、以前 MMR 数据异常：再检查提取和重采样。

在取得这次有效录音和寄存器日志之前，不认定噪声根因，不调整 AFE/WakeNet 参数。恢复正常业务时把 test_profile.h 中 CAMERA_TEST_PROFILE 改回 CAMERA_TEST_FULL 并重编译。
