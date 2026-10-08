# 小智独立参考测试：当前为双麦 MIC1 + MIC2 + REF

当前使用 Waveshare Brookesia XiaozhiApp 的独立双麦参考架构，适配自研 PCB：MIC1=SLOT0、MIC2=SLOT2、硬件 REF=SLOT1。三路分别重采样后按 MMR 送入 AFE；I2S 保持四槽采集。

此前 MIC1 单麦测试保存在 `logs/xiaozhi_mic1_single_baseline_20260930/`；当前双麦配置使用本工程的 ESP32-P4 v1.3 构建配置。

本目录独立构建、独立分区，不引用 `src/demo` 的音频组件。此前 MIC2+REF 单麦参考方案已由用户实测确认连续对话有效；当前双麦 MMR 配置待在 v1.3 芯片上复测。详见 TEST_RESULTS.md。

## 来源与修改边界

参考：Waveshare `ESP32-P4-WIFI6-Touch-LCD-4B`，commit `5a7a9a2823d77983c15b44a72934a7699e5d2d8b`，`firmware/brookesia/components/XiaozhiApp`。

`UPSTREAM.json` 保存基线源码哈希；`UPSTREAM_ADAPTATION.diff` 是此前单麦版本的源码差异记录。保留原参考 input/send/playback/control/AFE 任务、独立 `esp_ae_rate_cvt`、PCM accumulator、Opus、唤醒缓存及对话状态机。没有移植 Lummiss 自定义音频流程。

修改范围：

- 用 `board_audio` 替换整板 BSP，只初始化供电控制 GPIO8、I2C 和音频 codec。
- 禁用 Brookesia/LVGL 界面，用串口显示状态、绑定码和 STT/TTS 文本；原版绑定提示音保留。
- 增加被动统计；不替换 PCM，不禁用网络发送。
- 网络适配当前 C6：ESP-Hosted 2.7.4、WiFi Remote 1.3.0，SDIO CLK18/CMD19/D0..D3=14..17、RESET54，10MHz。没有复制主工程修改过的 managed_components。
- WiFi Remote 的 Kconfig 使用 IDF `5.5` 版本入口，实际编译 SDK 仍为 v5.5.5。

原版 AudioProcessor 采用 `AFE_TYPE_FD / AFE_MODE_LOW_COST`，保持参考配置，不强行套用主工程的 AFE 参数。

## 当前 PCB 配置

| 项目 | 设置 |
|---|---|
| I2C | SDA34 / SCL33 |
| I2S | MCLK28 / BCLK29 / WS30 / DOUT31 / DIN32 |
| RX | 24kHz、16bit、4-slot TDM |
| 物理映射 | S0=MIC1，S2=MIC2，S1=硬件 REF，S3 未使用 |
| AFE 输入 | MIC1/MIC2/REF 三通道分别 24k→16k，然后 `[M1,M2,R]` 交织；MMR |
| 输入 gain | 沿用参考 BSP 24dB；不对两个 MIC 做独立补偿 |
| 输出音量 | 沿用参考 BSP 80 |

启动时打印 ES7210 实际 I2C 寄存器读回和 P4 RX slot 寄存器。映射来自当前 PCB 实测，不从开发板型号推断。本轮应看到 `Ready: input=MMR` 和 AFE 自身的 2 microphone + 1 playback、总 3-channel 日志。

## 构建与烧录

当前构建目标为 ESP32-P4 v1.3，最低支持 v1.0，最高支持 v1.99；不可用于 v3.x 芯片。

在根目录运行 `powershell -ExecutionPolicy Bypass -File .\src\xiaozhi_dual_mic_reference_test\build_reference.ps1`。
构建日志：`logs/xiaozhi_reference_build.log`。

在 IDF 扩展中打开**本目录**，选择本目录的 `build`，然后使用扩展烧录功能。不要使用主工程 `build_rtc_mem_b`。未执行自动烧录。

也可在已经导入 IDF 环境的 PowerShell 中：

```powershell
cd E:\Lummiss_Plant_Robot\src\xiaozhi_dual_mic_reference_test
& 'C:\Espressif\tools\python\v5.5.5\venv\Scripts\python.exe' "$env:IDF_PATH\tools\idf.py" -B build -p COM7 flash monitor
```

首次必须完整烧录 `flash_args` 的全部镜像：bootloader、partition table、应用、srmodels 和 storage 提示音。**独立分区与主工程不同，恢复主工程时也必须完整烧录主工程及其模型，不能只刷 app。** 不执行 erase_flash，不自动清空 NVS。

WiFi 默认读取已保存的 STA 配置。若启动显示“没有已保存的WiFi”，在 `menuconfig → Reference board` 填写测试 SSID/密码，再构建。密码只保存在本地被忽略的 sdkconfig 和固件中，不填入跟踪的 defaults。

## 官方服务绑定

使用原生 `https://api.tenclass.net/xiaozhi/ota/`，没有配置 Lummiss 服务地址。联网后按 `REF_BIND code=...` 的码在小智官方控制台绑定测试设备；参考提示音也会播报码。

保留参考 AUTO：服务返回 MQTT 配置时使用 MQTT+UDP，否则使用 WebSocket。日志如为 WebSocket，则 UDP 测试不适用，不能记为 UDP 通过。

## 观测与测试

每 5 秒 `REF_DIAG` 输出 feed/fetch 次数、最后 VAD 状态、WakeNet 检出数、成功上行包数、发送 API 错误数、M1/M2/REF raw RMS 与样本数。计数均为该窗口增量，不是累计值。

`uplink` 来自原版发送任务实际调用成功，不是本地 sink。MQTT 路径的组件调用最终执行 UDP `send()`；API 错误包含通道未就绪、锁超时等，不能全部解释成 socket 故障；API 成功也不代表服务器已经完成 STT。原版缓存音频和实时音频都计入成功上行。

1. 确认 DHCP、codec、模型、MMR 初始化和绑定完成。
2. 在相同距离和音量下说“你好小智”10次；每次等回到可唤醒状态，记录成功次数。
3. 唤醒后说一句话，确认 `REF_CHAT user`、`assistant` 和实际扬声器播放。
4. 回答结束后不再说唤醒词，继续对话至少5轮；逐轮核对上行包、STT、TTS。
5. 串口日志统一保存到根目录 `logs/`。请提供完整启动到失败的日志，而非仅最后一条报错。

验收状态：本次双麦 MMR 在 v1.3 芯片上的稳定唤醒、首轮 STT/TTS、连续对话均待实测；历史单麦结果见 TEST_RESULTS.md。

## 历史单麦启动修正

首版单麦日志中的 `Allocate audio input buffer: ESP_ERR_NO_MEM` 不能视为真实内存不足证据：单麦 MR 只有两个重采样器，旧三通道下标检查导致误报。当前双麦 MMR 配置使用三个重采样器。
