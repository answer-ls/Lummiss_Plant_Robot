# 独立 ES7210 原始采集测试

本工程仅使用 ESP-IDF v5.5.5、I2C、I2S RX、UART 和 PSRAM缓存。没有链接主工程组件，不启动 ES8311/TX、AFE、WakeNet、Opus、网络、LVGL、SD 或 WebRTC。GPIO8仅保持当前PCB供电。主工程的两个构建目录及配置不变。

## 官方参考与适配边界

参考 [Waveshare ESP32-P4-WIFI6-Touch-LCD-5 / 10_Mic_Record](https://github.com/waveshareteam/ESP32-P4-WIFI6-Touch-LCD-5/tree/d144b34d8f384e84d2442e8733523560216a2f63/examples/arduino/examples/10_Mic_Record)，固定提交 `d144b34d8f384e84d2442e8733523560216a2f63`。

这不是把官方Arduino sketch原封不动编译：为兼容本机IDF工具链，将Wire替换成IDF I2C master、旧I2S接口替换成IDF RX TDM接口。`main/es7210.c`来自该示例的`es7210.cpp`，保留授权声明、寄存器初始化流程和时钟系数表；除传输层、去掉Arduino条件编译、打印API外，格式寄存器0x12由0x00改为0x02以输出本PCB四槽TDM。原始参考文件保存在项目根目录`logs/hardware_audio_reference/`。

保持官方16kHz、16bit、MCLK=256fs、DMA 8×64帧以及MIC1/MIC2的0dB增益；**不使用业务固件的24kHz/输入增益**，因此不要直接比较两份固件的响度。无软件增益、滤波、重采样、MMR重排。

| 信号 | 当前PCB |
|---|---|
| I2C SDA / SCL | 34 / 33，100kHz，探测0x40~0x43 |
| MCLK / BCLK / WS / DIN | 28 / 29 / 30 / 32 |
| DOUT 31 | 不使用；没有TX句柄/播放端 |
| RX格式 | Philips、MSB先、延迟1bit、16bit数据/16bit槽、4槽、WS宽32bit、非反相 |
| 预期时钟 | MCLK=4.096MHz，BCLK=1.024MHz，WS=16kHz；这是配置计算值，未经仪器测量 |

三组分别调用参考驱动`es7210_mic_select(1/2/3)`选择ADC MIC1、MIC2、双麦，REF/ADC3及ADC4不选用。四槽格式和读取步长始终不变，每组重新执行ADC初始化并真实I2C回读配置。原始布局为 `int16_t frame[n][0..3] = SLOT0..3`；沿用本板实测SLOT0对应MIC1、SLOT2对应MIC2，其他槽也完整保存。单麦测试应检查目标槽有声且未选槽近静音；若全部槽归零、声音跑到其他槽或格式回读不符，先判选择/时钟/映射实验无效，不能判硬件坏。

## 烧录独立工程

不要在demo目录点击原来的烧录任务，否则烧录的是主业务固件。可用IDF扩展单独打开本目录，或直接运行下面命令（把COM7改成P4实际端口）。无需重新编译，使用已生成的flash_args：

```powershell
cd E:\Lummiss_Plant_Robot\src\hardware_audio_test\build
& 'C:\Espressif\tools\python\v5.5.5\venv\Scripts\python.exe' 'D:\espidf5.5.5\.espressif\v5.5.5\esp-idf\components\esptool_py\esptool\esptool.py' --chip esp32p4 -p COM7 -b 460800 --before default_reset --after hard_reset write_flash '@flash_args'
```

这会替换板上应用、bootloader及分区表为测试工程。无需erase_flash。恢复业务时从demo对应构建目录重新完整烧录。串口占用时先关闭IDF monitor。

## PC导出与试听

```powershell
& 'C:\Espressif\tools\python\v5.5.5\venv\Scripts\python.exe' 'E:\Lummiss_Plant_Robot\src\hardware_audio_test\tools\capture_hardware_audio.py' --port COM7
```

1. 关闭其他串口监视，脚本连接后按P4 RESET。
2. 等待MIC1提示，准备相同固定声源，按回车；保持声源至少11秒。
3. 固件排空0.5秒启动数据，然后采集160000帧，即10秒/四槽/1280000字节。期间不打印、不导出、不计算RMS。短读/超时保留原样并标无效；RX队列溢出也标无效。
4. 停止RX后统计并导出，115200波特率约3分钟/组。不要复位。随后脚本提示MIC2、DUAL，分别重复同样声源、距离和时长。
5. 可用`--groups 3`只做双麦；默认三组。所有输出统一存入根目录`logs/hardware_audio_时间/`。

每组文件：`tdm_raw.bin`（原始交织字节）、`raw_4slot.wav`（4ch/16kHz/16bit）、`slot0.wav`到`slot3.wav`（逐槽提取，单声道）、`summary.json`。整次串口输出保存为`serial.log`。导出验证offset、总长、FNV32；JSON另外保存SHA256。没有补零、重采样或拼接重试。

固件和PC分别统计每槽DC、RMS、去DC的AC RMS、绝对peak、clipping（等于32767/-32768的样本数）、zero count。WAV拆槽只移除其他槽，不改变目标槽任何样本。

## 判定范围

- `valid=true`、每槽160000样本、10秒、overflow=0，才具备连续性检查的基本条件；还需试听并核对单/双麦实际响应。
- 如果原始单槽已出现同类噪声，说明噪声在该独立配置的ES7210/I2S RAW阶段或更早就存在，不能再归因于AFE/Opus；仍不能只凭录音区分模拟、时钟或数字格式原因。
- 如果三组清晰，只证明该16kHz独立配置正常；不能直接证明24kHz完整业务、播放并行时也正常。之后再单变量比较采样率/业务生命周期。
- MIC1/MIC2幅度不同不能单独判坏；0dB下过小/全零先核对通道启用和格式。没有实机数据前，本工程不宣称硬件正常或异常。
