# Waveshare P4-4B：ES7210 / ES8311 / I2S 源码差异

日期：2026-09-28。只做源码审计，没有修改、编译或烧录固件。完整数据路径与验证优先级见 [音频链路报告](E:/Lummiss_Plant_Robot/WAVESHARE_P4_AUDIO_REFERENCE_DIFF.md)。

## 版本与来源

参考仓库锁定 `waveshareteam/ESP32-P4-WIFI6-Touch-LCD-4B` 提交 `5a7a9a2823d77983c15b44a72934a7699e5d2d8b`。审计的是 `firmware/brookesia/components/`，**不是 archive/**。

- [Waveshare app](https://github.com/waveshareteam/ESP32-P4-WIFI6-Touch-LCD-4B/blob/5a7a9a2823d77983c15b44a72934a7699e5d2d8b/firmware/brookesia/components/XiaozhiApp/XiaozhiApp.cpp)
- [bsp_extra](https://github.com/waveshareteam/ESP32-P4-WIFI6-Touch-LCD-4B/blob/5a7a9a2823d77983c15b44a72934a7699e5d2d8b/firmware/brookesia/components/bsp_extra/src/bsp_board_extra.c)
- BSP不是该仓库内一个猜测的实现：manifest固定 `waveshare/esp32_p4_wifi6_touch_lcd_4b=3.0.1`；[注册版本](https://components.espressif.com/components/waveshare/esp32_p4_wifi6_touch_lcd_4b/versions/3.0.1)指向组件仓库提交 `69b3e7ba512e3676519196f5d91680445600a101`。
- [BSP真实源文件](https://github.com/waveshareteam/Waveshare-ESP32-components/blob/69b3e7ba512e3676519196f5d91680445600a101/bsp/esp32_p4_wifi6_touch_lcd_4b/esp32_p4_wifi6_touch_lcd_4b.c)
- 参考与当前均用 `esp_codec_dev=1.5.11`。参考README使用IDF5.5.5流程，manifest允许`>=5.5,<6.0`；没有参考板本次实机ELF/lock，不把manifest范围当唯一实际构建版本。
- 当前文件：`src/demo/components/xiaozhi_audio/xiaozhi_audio.c`（下称 P），`audio_hw_init()`约1091行。
- 所有下载源码/原理图均在 `logs/waveshare_p4_audit/`，没有引入工程依赖。

**本板映射始终以实测为准：S0=MIC1、S2=MIC2、S1=REF。参考板即使使用同一排列，也不构成替换本板物理定义的理由。**

## 一、初始化调用链

参考真实调用：

```text
main/main.cpp:496 bsp_extra_codec_init()
  bsp_audio_init_voice_24k()
    bsp_audio_init_tx_std_rx_tdm()
      bsp_audio_init_channels(): new(TX,RX) → init TX STD → init RX TDM
                                → enable TX → enable RX → shared data_if
  bsp_audio_codec_speaker_init(): es8311_codec_new → output device
  bsp_audio_codec_microphone_init(): es7210_codec_new → input device
  bsp_extra_codec_set_fs(): close input/output → open output(默认16k,stereo)
XiaozhiApp音频初始化，XiaozhiApp.cpp:1626
  bsp_extra_codec_set_voice_fs(24000,16,4,0xF,physical MIC mask0x7)
    close input → close output
    open output(stereo,24k) → unmute
    open input(4ch,maskF,24k)
    physical MIC1/2/3 全部设置24dB
```

当前：

```text
audio_hw_init()
  new(TX,RX) → init TX STD → init RX TDM → enable TX → enable RX
  shared data_if → ES8311 codec → ES7210 codec → devices
  open output(mono,24k) → open input(4ch,maskF,24k)
  全局MIC gain30dB → 仅physical MIC3参考降到0dB
正常TTS阶段只write PCM，不重新open codec
```

参考的16k默认输出阶段不是最终语音配置。参考额外close/open也不是当前必须模仿的操作，更不能据此在每次TTS前重启I2S。

## 二、ES7210真实参数与增益

参考BSP `bsp_audio_codec_microphone_init()`，515行：

```c
es7210_codec_cfg_t es7210_cfg = {
    .ctrl_if = microphone_ctrl_if,
    .mic_selected = (audio_mode == BSP_AUDIO_MODE_TX_STD_RX_TDM) ?
                    BSP_AUDIO_ES7210_CONNECTED_MIC_MASK : 0,
};
```

connected mask在BSP头文件为MIC1|MIC2|MIC3；当前也为同三路。未填写的字段零初始化，master_mode=false。当前额外显式填写PAD/256，但共同驱动只在master分支使用MCLK source选择，不能把参数文字不同直接当时钟不一致。

参考 `bsp_extra/include/bsp_board_extra.h:25`：`CODEC_DEFAULT_ADC_VOLUME=24.0`；`bsp_extra_codec_set_voice_fs()`171行对physical connected mask=0x7设置24dB。**参考不是30/30/0，而是24/24/24，包括参考ADC。**

| 项目 | Waveshare P4语音最终配置 | 当前项目 | 证据/影响 |
|---|---|---|---|
| ES7210 master | false（默认） | false（显式） | 一致 |
| MIC1/2/3使能 | 是/是/是 | 是/是/是 | 一致 |
| MIC4使能 | 否 | 否 | 一致 |
| MIC1 PGA | 24dB | 30dB | 不同，6dB绝对增益；不解释本板两麦比例 |
| MIC2 PGA | 24dB | 30dB | 同上 |
| MIC3 REF PGA | 24dB | 0dB | 不同，参考链路电平需各板独立核实，不照抄 |
| 0x43/44/45预期 | 18/18/18（源码推导） | 1A/1A/10（已有实读） | 区分参考预测和本板实测 |
| MIC bias | 驱动41/42=70 | 实读70/70 | 同一驱动默认；不是仪器电压值 |
| HPF | 驱动20=0A,21=2A,22=0A,23=2A | 实读一致 | 无板级额外设置，截止频率未据源码确定 |
| 模拟配置 | 40=43 | 43 | 同驱动 |
| ADC数字增益 | 未发现单独数字音量配置 | 未发现单独数字音量配置 | 不把PGA说成数字volume |
| TDM enable | 3个MIC→12=02 | 同，实读02 | 一致 |
| word length | set_bits(16)→11高位60 | 同，实读60 | 一致 |
| serial format | normal I2S低位0 | 同 | 一致 |
| slave时钟分频 | config_sample slave直接返回 | 同 | 不应依据master分频表计算本板实际BCLK |

共同codec源码：`src/demo/managed_components/espressif__esp_codec_dev/device/es7210/es7210.c`：`es7210_mic_select()`188行、`set_bits()`271行、`_es7210_set_channel_gain()`347行、`es7210_open()`、`es7210_set_fs()`。

重要调用细节：codec构造曾给三路30dB；`esp_codec_dev_open()`之后会应用device默认全局gain，再由应用写最终gain。因此报告比较的是**最后生效的应用设置**，不是只摘构造函数里的30。

ES8311参考与当前都是DAC/slave/use_mclk=true/invert=false；参考有PA GPIO和5.0/3.3硬件补偿，当前无PA GPIO、3.3/3.3为0dB补偿。这影响播放/参考幅度，不能解释没有播放时两麦为何相差约2倍。

## 三、I2S逐字段

参考 `bsp_audio_init_voice_24k():403`、`bsp_audio_init_channels():320`；当前 P `audio_hw_init():1091`。

| 字段 | 参考 | 当前 | 结论 |
|---|---|---|---|
| new_channel | 一次创建TX+RX，master，同controller | I2S0，同 | 一致架构 |
| init顺序 | TX STD → RX TDM | 同 | 一致 |
| enable顺序 | TX → RX | 同 | 一致 |
| GPIO共享 | MCLK/BCLK/WS共享，TX只dout，RX只din | 同 | 一致；GPIO编号保留各自板定义 |
| sample rate | voice24k | 24k | 一致 |
| mclk_multiple | 256 | 256 | 一致 |
| clk_src | DEFAULT | DEFAULT | 同IDF/P4修订配置才可进一步比较底层源 |
| RX data_bit_width | 16 | 16 | 一致 |
| RX slot_bit_width | AUTO→16 | AUTO→16 | 一致 |
| total_slot | BSP_AUDIO_TDM_SLOT_COUNT=4 | 4 | 一致 |
| RX slot_mask | 0xF | 0xF | 一致 |
| RX ws_width | AUTO→半帧32 | 同 | 一致 |
| RX bit_shift | Philips=true | 同 | 一致 |
| RX left_align | 默认false；codec重配true | 同 | 一致路径 |
| RX bclk_div | 8 | 8 | 一致 |
| clock inversion | GPIO宏默认false | 全false | 一致 |
| DMA desc/frame | IDF默认6/240 | 显式6/240 | 一致 |
| auto_clear | `.auto_clear=true` | `.auto_clear_after_cb=true` | IDF5.5.5中union别名，一致 |
| auto_clear_before_cb | 默认false | 默认false | 一致 |
| TX初始 | STD stereo16data/16slot | 同 | 一致 |
| codec输出格式 | stereo channel2，mask默认both | mono channel1→data_if转换为2slot、maskleft | 不同有效TX槽选择 |
| 稳定共享帧 | RX4×16，TX2×32，64clocks/frame | 同；已有实机寄存器佐证 | 数据位16不等于TX slot16 |

理论WS24k、BCLK1.536MHz、MCLK6.144MHz，仍非实测。参考默认sdkconfig包含早期P4修订设置，另有rev3_x配置；未取得其测试固件，不能声称参考与本板硅修订/时钟树完全一样。

## 四、idle TX结论

双方都依赖TX master维持shared RX时钟。IDF5.5.5 `i2s_common.h:69`明确`auto_clear`是`auto_clear_after_cb`别名；`i2s_common.c`TX DMA完成路径对已发缓冲清零。没有应用write时最终继续输出零，**不需要应用不断写零才能保留时钟**。

- 未播放：参考不专门循环补零；当前`TX_NO_APP_WRITE`从channel运行/auto_clear角度一致。
- 正常播放：参考将单声道PCM复制到stereo后write，当前mono write由slot配置处理；数据布局不同，不能跨工程直接比较TX字节数。
- 播放结束：双方语音设备仍open时，已有排队PCM先播完，再由auto_clear归零；“TTS停止事件”不保证当刻无尾音。
- 参考退出音频App调用`bsp_extra_codec_dev_stop()`会关闭devices，不等于其在每句TTS后停TX。

没有发现当前独有的“未写入时固定重复旧PCM”配置。也不能据auto_clear设置证明从未underrun；仍需区分有意空闲与播放供数断续。**不再建议disable TX却要求RX继续的实验。**

## 五、两麦补偿与原理图

参考采集链只取样、统计、重采样和interleave，**参考实现未发现数字补偿**：无单麦乘2、每通道归一化、校准表或AFE前gain balance。两实体麦PGA等值24dB。

检查固定提交中的 `schematic/ESP32-P4-WIFI6-Touch-LCD-4B.pdf` 第1页MIC区域：两路都标2.2µF耦合、33pF支路、10/30pF输入网络、同ADC_MICBIAS12；未看出为了产生2倍幅度而设计的不同标称网络。图中器件标号MIC1接标注MIC2_P、器件MIC2接MIC1_P，再次说明器件丝印、ADC编号、slot编号不能混用。图上没有充分的麦克风型号/装配声孔朝向信息，无法据此宣称两麦灵敏度/方向完全相同，更不能推断本板模拟电路。

原图和可读局部保存在 `logs/waveshare_p4_audit/reference/schematic/` 与 `mic_circuit.png`。本板30/30实读已成立，所以“不等的软件PGA”不再列为解释。
