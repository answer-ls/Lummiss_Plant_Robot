# 阶段1：RTC 模型释放栈位置 A/B

本次只实施用户指定的第一步：把 `wake_process` 的 40960 字节栈从
`SPIRAM|8BIT` 改为 `INTERNAL|8BIT`，core1/priority8 不变。
这是一份诊断固件，不是长期内存方案。没有拆分上行任务或更换 resampler。

## 已有证据与待验证项

IDF 5.5.5 的 `components/spi_flash/cache_utils.c` 在
`spi_flash_disable_interrupts_caches_and_other_cpu()` 入口断言
`esp_task_stack_is_sane_cache_disabled()`。
当前 RTC 释放路径在 PSRAM 栈的 `wake_process` 中调用模型 munmap，
与用户给出的 backtrace 相符。源码支持该解释，但编译成功不能代替板端 A/B。

原有停止顺序保持：设置 RTC suspended（capture guard 内），结束语音会话，
取得 capture guard，调用 deinit；deinit 等待 fetch 退出再销毁 AFE 和模型。
本轮不改变锁、等待、销毁或 ACK 的业务顺序。

## 如何验证

使用 IDF 扩展烧录 `src/demo/build_rtc_mem_b`；`src/demo/build` 同步构建。
串口日志保存到项目 `logs/wake_stack_stage1/`，不自动烧录。

1. 启动应看到 `WAKE_STACK_AB stage=1 stack_caps=INTERNAL|8BIT`。
2. 从 WAKE_IDLE 开启 RTC；查看 `WAKE_CLEANUP begin` 的实际栈归属为 INTERNAL。
3. 应依次看到 `srmodel_deinit begin`、`srmodel_deinit done`、`total_us` 和 RTC 后续启动日志。
4. 预览至少60秒，检查浏览器是否连续显示；停止并重复10轮。
5. 保留每轮 `rtc_suspend=1/0` 内存及 stack_high_water，分别比较相同阶段。
   high-water 使用 ESP-IDF FreeRTOS API 返回的字节单位。
6. 任意 assert、WDT、启动失败都保存原日志/backtrace，不自动重试掩盖第一次故障。

该诊断额外长期占用约40 KiB INTERNAL，可能导致后续 H264/Peer 分配失败。
若 munmap 断言消失但 RTC 因内存不足失败，必须分别记录：前者支持栈位置解释，
后者说明不能保留这个诊断内存布局，不能据此宣布 RTC 完整测试通过。

## 后续阶段门槛

- 栈位置 A/B 得到实机结果后，再实现小型 INTERNAL 生命周期上下文，恢复
  `wake_process` PSRAM 栈；复核 feed/fetch/销毁同步及初始化失败清理路径。
- 阶段1正式方案验证后，独立进行消费者迁移及 encoder 唯一所有权改造。
- 阶段2稳定后才替换 rate converter；本轮不改算法。

## 本次改动与版本边界

- `xiaozhi_audio.c`：wake_process 栈 caps；启动标记；RTC释放/恢复后内存快照。
- `wake_word.c`：实际栈归属、释放前后模型日志、耗时、高水位；不改变销毁逻辑。
- 本文：阶段1测试步骤和验收边界。

修改前源码保存在 `logs/wake_stack_stage1/*.before.c`，仅本次差异保存为
`logs/wake_stack_stage1/stage1-only.patch`。现有工作区两个音频文件包含大量
先前未提交修改，本轮不将这些历史修改混入 `fix` 提交。
阶段1正式修复与独立 commit 尚未完成；阶段2/3也未实施，不标为已完成。
