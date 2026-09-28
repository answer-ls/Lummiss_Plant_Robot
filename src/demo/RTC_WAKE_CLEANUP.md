# RTC 模型生命周期任务

> 这是上一版静态 cleanup 方案的记录。当前连续内存定位及 A/B 固件请以
> [RTC_CONTIG_TEST.md](E:/Lummiss_Plant_Robot/src/demo/RTC_CONTIG_TEST.md) 为准：B 组已改成按需创建并同步回收。

本轮优先恢复视频启动。取代 `WAKE_STACK_STAGE1.md` 中临时把整个控制任务搬进
INTERNAL 的诊断版本；不实施小智上传任务拆分或 rate converter 改造。

## 修改

- `xiaozhi_audio.c`：`wake_process` 恢复 40960B PSRAM 栈，仍为 CPU1/P8；
  RTC 清理仍同步完成后才 ACK，保留 `RTC_AUDIO_MEM` 内存快照。
- `wake_word.c`：新增固定 `wake_cleanup`，CPU1/P8，8192B INTERNAL 静态栈，
  静态 TCB 和两个静态信号量；空闲阻塞于任务通知，不轮询。
  worker 只负责模型加载及完整 AFE/模型清理，不参与音频采集、编码和发送。
- `wake_word.h`：明确生命周期调用的同步契约。

模型加载也使用此任务：RTC STOP 会重新 mmap，不能只修 START 的 munmap。
所有初始化失败分支统一调用同步 deinit，不再从 PSRAM 栈直接释放模型。
正常 AFE 配置、创建和输入算法未修改。

与上一版相比，移走 40960B INTERNAL 任务栈，增加 8192B INTERNAL 静态栈，
栈净减少32768B；还需计入新增 TCB/信号量对象。最大连续块不会按这个差值线性增长，
H264 所需约92224B连续块是否满足必须看实机日志。

## 同步与所有权

```text
RTC START
  -> wake_process 标记 suspended，capture_guard 排除 feed/输出读取
  -> 请求 wake_cleanup，调用方阻塞等待
     -> 标记 stopping，等待 wake_detect/fetch 的 EXITED
     -> 销毁 AFE/缓冲，esp_srmodel_deinit / munmap
     -> 完成信号
  -> wake_process 释放 capture_guard，记录内存，RTC ACK
  -> H264 / Camera / Peer 按原 RTC 流程继续
```

worker 不拿 capture_guard，不等待网络/RTC线程；完成之后不再访问本轮资源。
请求锁禁止覆盖未完成的工作，完成信号不用调用任务的通知槽。
上层仍负责串行化 init/deinit，并在销毁前停止所有 AFE API 使用。
重复 deinit 检查空句柄，不重复解除同一模型映射。
worker 固定保留，START/STOP 不创建/删除它，也不每轮分配栈。

## 验证

构建日志：项目 `logs/wake_cleanup_worker/build_rtc.log` 和 `build.log`。
两份固件分别在 `src/demo/build_rtc_mem_b`（IDF扩展）及 `src/demo/build`。
不自动烧录。

烧录后应出现：

```text
WAKE_PROCESS stack_caps=SPIRAM|8BIT stack_bytes=40960
WAKE_MODEL_LOAD ... stack_bytes=8192
WAKE_CLEANUP begin task=wake_cleanup stack_caps=INTERNAL
WAKE_CLEANUP srmodel_deinit done
WAKE_CLEANUP total_us=... stack_high_water=...
RTC_AUDIO_MEM rtc_suspend=1 ...
H264_REF_REGION ptr=<非零> ...
```

停止预览应再次看到模型加载完成及 `RTC_AUDIO restore=ESP_OK`。
测试至少10轮 START/STOP，每轮保留前后同阶段的 DMA/INTERNAL free/largest、
PSRAM free；预览至少60秒并确认浏览器持续显示。记录 cleanup 栈高水位（字节），
验证8KiB余量；没有关闭 WDT、栈保护或 cache 安全断言。

本轮编译/链接检查不能证明实机无 UAF、WDT 或内存不足。
若 H264 reference 仍失败，保留 `BEFORE/AFTER_H264_REF_ALLOC` 和完整生命周期
日志继续定位连续块，不自动重试或改小分辨率。
