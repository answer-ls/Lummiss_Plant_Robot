# I2S运行期变化审计

日期：2026-09-27。只读审计。当前实测slot0=MIC1、slot2=MIC2、slot1=REF，不使用官方板映射替换。

## 搜索范围与解释

检索`src/demo`项目源码及managed esp_codec_dev中的channel创建、enable/disable、STD/TDM clock/slot重配、删除，以及codec open/close/set_fs/set_bits/格式/输入输出使能。原始结果保存在`logs/es7210_official_audit/runtime_search.txt`和`full_runtime_search.txt`。

managed组件还包含其他codec和test_apps，出现搜索命中不代表当前业务执行。当前FULL路径以`xiaozhi_audio.c`为音频硬件所有者。以下区别直接调用、codec库间接调用、仅诊断模式调用。

## 当前初始化/关闭/诊断调用

| 路径 | 文件:行 / 函数 | channel及行为 | 当前正常会话触发？ |
|---|---|---|---|
| audio init | `components/xiaozhi_audio/xiaozhi_audio.c:1110`，audio_hw_init | new_channel同时创建I2S0 TX/RX | 一次 |
| audio init | 同文件1137/1163 | TX STD，RX TDM init | 一次 |
| audio init | 同文件1174/1176 | 先enable TX，再enable RX | 一次 |
| output open | 同文件1295 | esp_codec_dev_open输出，间接更新TX格式/clock | 初始化 |
| input open | 同文件1305 | 输入4槽open，库协调RX/TX帧宽并可能短暂disable/re-enable | 初始化 |
| cleanup | 同文件1042/1047 | input close，再output close | 停止服务/失败清理，不是每次TTS |
| cleanup | 同文件1076..1082 | TX disable/delete，RX disable/delete | 同上 |
| 硬件音频测试 | 同文件2362起，audio_hardware_test_force_official_i2s | RX disable、reconfig_tdm_slot、enable | profile9测试，不是FULL会话 |
| raw探针 | `components/xiaozhi_audio/raw_adc_probe.c:119/122` | RX disable，注册采集事件，再enable；TX保留 | 专用raw测试，不是FULL会话 |
| lifecycle当前版本 | `components/xiaozhi_audio/lifecycle_probe.c`，lifecycle_prepare | RX保持；TX不写、写零、写测试音；不重配格式 | 启动诊断 |

路径前缀均为`src/demo/`。条件编译的旧板无ES7210分支使用STD RX，不是当前板生效分支。

## codec库间接调用（不可遗漏）

文件：`src/demo/managed_components/espressif__esp_codec_dev/platform/audio_codec_data_i2s.c`。

| 函数/位置 | 行为 |
|---|---|
| channel使能辅助，201/203行 | i2s_channel_enable/disable |
| set_drv_fs STD，267/272行 | reconfig_std_slot + reconfig_std_clock |
| set_drv_fs TDM，387/391行 | reconfig_tdm_slot + reconfig_tdm_clock |
| check_fs_compatible / set_fs / get_bits | 比较帧总位数，必要时更新paired channel；TX为2×32、RX为4×16可合法共享64bit帧 |
| _i2s_data_enable | 保留双工RX所需TX，使用out_disable_pending；逻辑output关闭不一定马上物理停TX |
| _i2s_data_read | 重配窗口可填零返回；底层bytes_read未通过公共read API传出，需额外诊断才能严格证明读满 |

`esp_codec_dev.c::esp_codec_dev_open()`先调用data_if set_fmt/enable，再codec set_fs/enable。核查版本中前两者的返回值没有向上逐项传播，故“open返回成功”不足以证明每一次底层重配成功。**这也是参考库的行为，不是已证实本工程新bug。** 当前稳定硬件快照补充了实际配置证据。

ES7210 `es7210_set_fs()`内部设置bits/format，slave采样率配置分支直接返回，不在每帧读入时重新写分频；ES8311 set_fs同样由codec打开流程调用。正常TTS数据write不是set_fs。

## 语音状态变化与硬件变化分开

| 状态/事件 | 当前业务行为 | RX/TX enable/clock/slot/codec sample rate |
|---|---|---|
| WAKE_IDLE | WakeNet/处理开关；capture持续取样 | 未找到状态入口重配 |
| CONNECTING | 建立会话、preroll、协议发送 | 未找到重配 |
| LISTENING | 开启语音处理和麦克风上行 | 未找到重配 |
| SPEAKING | 禁止上行/切换处理开关；播放task写PCM | 未找到重配 |
| POST_TTS | 生命周期诊断标记 | 未找到重配 |
| CONTINUOUS_LISTENING | 恢复处理与上行 | 未找到重配 |
| idle返回 | 关闭本轮会话，恢复唤醒 | 不等于audio_hw_cleanup |

入口：`xiaozhi_audio.c::set_voice_state()`及capture/playback/service任务。没有证据支持“每次TTS开始都把ES7210切成另一种TDM格式”。阶段之间短时错误仍需运行记录，不以静态搜索替代所有动态证明。

## 实机快照核对

来源：`logs/lifecycle_20260927_173525/lifecycle.json`。18份快照按`AUDIO_INIT_DONE BEFORE`比较：

- 已读ES7210寄存器没有变化、没有read错误。
- RX conf/conf1/tdm与记录的clock寄存器没有变化。
- 唯一硬件变化：旧TX_OFF实验TX conf从1080619540到1080619536，启动位清除；该组没有取得PCM。
- 稳定寄存器解码：RX16有效位/16slot/4slot/ws32；TX16有效位/32slot/2slot/ws32。
- 两麦PGA实读1A/1A，参考10；不能用“日志软件写30”替代此readback，但此处确实已有readback。

自动比较文件：`logs/es7210_official_audit/es7210_readback_diff.json`。不能由起止快照不变推断瞬时BCLK一定无抖动或RX永不溢出。用户暂时没有仪器，MCLK/BCLK/LRCK只有配置理论值，没有实测波形。

## TX_OFF为何不应继续强行实现

当前先初始化TX master，再初始化同controller的RX。IDF5.5.5 `components/esp_driver_i2s/i2s_tdm.c`双工分支让RX跟随已配置方向的时钟。直接停TX可能使RX无有效时钟；旧实验0字节timeout与此相符，不能作为无噪声样本。

当前替代名称`TX_NO_APP_WRITE`含义是“应用不提交TX PCM”，TX主时钟与自动清零机制仍保持。它不是物理TX关闭。独立停TX却继续驱动RX需要改变clock/controller拓扑，已超出本轮只读审计范围，不建议为该对照大改架构。

## 官方运行期参照

[AudioService源码](https://github.com/78/xiaozhi-esp32/blob/8ce50d27cd7c72c777673f46cbfc3ef7454d1b5c/main/audio/audio_service.cc)中：

- ReadAudioData首次启用输入；AudioOutputTask首次启用输出。
- EnableInput/EnableOutput通过同一mutex串行化对共享data_if的操作。
- 功耗检查中，双工输入仍开时避免关输出；输入超时请求也由采集任务复查活动标志后关闭。
- TTS不必每次重配；首次/重新open可间接重配，不能笼统称“官方永远不disable I2S”。
- 官方也不是永久无条件RX采集；当前业务持续读取方式与其功耗生命周期不同，但没有据此证明当前噪声来自持续采集。

## 本轮结论

已确认一个架构约束：RX依赖共享TX时钟，旧TX_OFF对照无效。未发现一个与LISTENING/SPEAKING切换绑定的持久ES7210/I2S改配点。下一轮应先定位专用直采与完整业务启动之间第一个raw变化边界，并记录采样连续性；不直接更换slot mapping、AFE或重采样。
