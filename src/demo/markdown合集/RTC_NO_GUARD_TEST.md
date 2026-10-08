# RTC 无占位预留方案（2026-09-24）

保留 1280×720、编码目标 20 fps。首次无 Guard 实测使用固定 QP 51；后续画质修正改为固定 QP 36。固定 QP 不承诺 1.5 Mbps 上限。

## 画质修正

用户日志 7712b7e3 显示推流约 37 秒，稳定段编码/发送约 16–17 fps，码率约 0.28–0.43 Mbps，alloc_fail_count=0、tx_wait_timeout=0、send_fail=0。用户反馈画面很糊，QP=51 是过度压缩的明确因素。本轮仅将固定 QP 改为 36，并将码流统计中的 target 标注改为 fixed_qp；启动日志明确 bitrate_cap=none。

DMA largest 约 3456 B，仍低于安全余量；日志还有两次 JPEG 解码错误和 UVC 丢帧，尚未解决。QP36 已编译，但清晰度、实际码率及持续推流稳定性须重新实测，不能用上版低码率结果替代验证。

## 已实施

- 删除 H264 Guard 分配、释放及注册 API；删除 WHIP HTTP RTCRAM 占位分配。
- main.c / rtc_prepare_resources：停止动画并回收动画资源，初始化编解码任务。
- webrtc_whip.c / controller_task：收到 RTC 指令后实际打开 H264，首次启动 UVC，等 CAMERA_READY 后创建 Peer。只有 DTLS 就绪并进入 STREAMING 才接收编码输入。
- video_streamer.c / video_streamer_start：等待 codec 初始化完成后创建实际编码器。失败、取消、超时调用 stop_session 和 video_streamer_stop；codec 任务回收参考缓冲。
- test_profile.h：关闭开机 UVC 丢包测试；首次 RTC 请求才启动摄像头。UVC 启动后继续保留，供拍照及后续预览复用；本轮未添加 USB 栈反复销毁。后续预览先打开编码器再创建 Peer。
- display_driver.c：暂停 LVGL 显示刷新定时器，保留 UI 事件处理；退出 RTC 恢复刷新。动画保持阻塞。
- mempool.c / mempool_alloc、mempool_free：STA TX 最多两个在用或缓存的实际 DMA 缓冲，首次使用时分配；计数信号量耗尽最多阻塞 20 ms，超时返回错误，不无限扩池。缓存仍为 STA 两块、SDIO 一块。统计 tx_wait_timeout 和 alloc_fail_count，区分背压超时与分配失败。
- xiaozhi_audio.c / audio_work_buffers_init：CPU 重采样 PCM 和 Opus 用户输出迁到 PSRAM；I2S 硬件 DMA ring 未改。AUDIO_USER_PSRAM 打印实际大小。

## 验证

日志 b9a30c9a：LCD flush 门控生效，未见 LCD 提交失败或 WDT；DTLS 后 DMA free/largest=1879/1856 B，开始推流约 0.8 秒后 largest=1536 B，触发预算保护而停止。Peer open 阶段仍有 19312 B 普通请求落在内部 DMA 可用地址。

本轮修改 esp_peer/src/media_lib_weak.c 的 malloc/calloc/realloc/free 适配为 SPIRAM|8BIT。协议数据走 PSRAM，FreeRTOS mutex 和硬件 DMA 分配不变；不做 INTERNAL 回退。链接 map 确认这些符号来自已修改的适配器；固件编译通过。实际迁移字节数需对比下一次 PEER_LIVE_SUM 和 AFTER_DTLS，不能假定全部 19312 B 都来自这三个接口。

日志 185cfdb2：QP36 生效，但 WHIP POST 期间 SPI LCD 提交失败，LVGL 在 draw_buf_flush 等不到完成通知，导致 CPU1 WDT。确认 LVGL 的对象失效逻辑会恢复刷新定时器，单纯 pause 定时器无法阻止 RTC 期间刷新。已在 display_driver 的实际 flush 回调处增加 RTC 门控，跳过 SPI 并通知 LVGL 完成；恢复时整屏重绘。另修正 esp_lvgl_port 普通绘制路径忽略 draw_bitmap 错误的问题，提交失败时解除 flushing 等待。本项目每次绘制尺寸不超过 SPI 最大传输尺寸。QP36 保持不变，此修复已编译，待实机验证。DMA 瞬时不足仍未证明完全解决。

已编译 src/demo/build/lummiss_main.bin；未烧录或执行实机测试。

实机依次验证：首次开启、停止后第二次开启、摄像头不可用时取消、持续预览十分钟。观察浏览器连续运动、STA TX wait_timeout、alloc_fail_count、DMA free/largest、UVC 帧率和 WDT。失败保护仍启用；不将接口发送成功等同于浏览器解码成功。

源码在 managed_components 下的修改是本地组件补丁，升级依赖前需保留。


## 2026-09-24 RTC 事件刷新与空闲缓存限制

- display_driver.c：首页 1s 检测内容变更，无变更不设置控件；RTC 仅事件执行一次刷新，然后暂停刷新定时器。DMA free<8192 或 largest<4096 时保留事件延后，避免低余量时主动提交 LCD。该准入检查不是内存预留，也不能保证并发分配绝不失败。
- expression_manager.c / anim_bin_player.c：HOME 切换和资源释放后暂停 50ms 表情及 5ms 动画定时器；播放器无请求时永久等待通知。STOP 恢复定时器但不重播旧请求。
- mempool.c：保留 STA TX 两块按需分配、SDIO 本地一块缓存，全局 CAS 限制空闲缓存最多三块。降低单池上限立即释放已有多余空闲块。real_free_count=0 在从未超过缓存限额时正常；不人为制造释放。
- 固定最小池评估：目前维持两块 STA TX 实际业务块按需创建并复用，不额外预占 DMA；冷门池不缓存。缓存上限不等于所有池在用块的总上限。
- 新迁移：表情队列载荷 8*sizeof(expression_event_t)（当前192B）转 PSRAM，StaticQueue_t 留内部；MCP 电机任务参数 sizeof(motion_job_t)（当前12B）转 PSRAM。后者仅动作创建期间存在，不宣称长期节省。既有音频、动画帧/索引/文件缓冲迁移保留。
- 保持720p、现有编码规格、调度点及DMA临界停止阈值；不改硬件DMA缓冲。
- 编译输出 src/demo/build；本轮未烧录，持续预览、RTC停止恢复与实际heap变化仍待板端日志验证。
