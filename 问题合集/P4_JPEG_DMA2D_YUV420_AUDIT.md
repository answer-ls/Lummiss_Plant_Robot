# ESP32-P4 JPEG RX DMA2D 直出 YUV420 审计

审计对象：ESP32-P4 rev 3.2、ESP-IDF v5.5.5、当前 `src/demo/build_rtc_mem_b` 配置与 `src/demo/components/video_streamer/video_streamer.c`。本轮只读源码、配置和既有日志；未修改固件，也未做新的板端直出试验。

## 结论

| 问题 | 结论 |
| --- | --- |
| A. rev 3.2 硬件支持？ | **支持。** IDF 对 P4 最低修订 `< 3.0` 才屏蔽 YUV422 JPEG → YUV420 输出；P4 DMA2D HAL 有对应 RX CSC 模式。 |
| B. IDF 5.5.5 源码支持？ | **支持。** JPEG decoder 在 YUV422 采样且请求 YUV420 输出时选 `DMA2D_CSC_RX_YUV422_TO_YUV420`。 |
| C. 当前构建编译支持？ | **支持，但业务路径关闭。** `CONFIG_ESP_REV_MIN_FULL=301` 使官方分支进入编译；当前 `VIDEO_OFFICIAL_JPEG_YUV420_ENABLED=0`，运行中仍请求 YUV422 并执行 CPU 转换。 |
| D. JPEG 输出与 H264 输入布局兼容？ | **格式设计上兼容，当前设备尚未实测通过。** 两者都是行交错的 `O_UYY_E_VYY` 12 bpp，而非 I420 三平面；1280×720 时尺寸和行步长匹配。但官方接口未保证 DMA2D 下采样与现有 CPU 平均算法逐字节相同，现有直出核对开关关闭，故不能声称实机零拷贝已通过。 |
| E. 现在可以删除 CPU 转换吗？ | **不能。** 先在单变量实机试验中确认 `jpeg_decoder_process()` 返回成功、输出大小正确、画面色彩正确、H264 连续编码/预览稳定。通过后才可移除 CPU 转换与 YUV422 中间缓冲；同时要保留 JPEG 错误帧过滤。 |
| F. 官方参考在哪里？ | 见文末 `官方参考`。官方 m2m 管线确实连接 JPEG decoder YUV420 与 H264 encoder，但其 README 要求摄像头 JPEG 原生 **YUV420** 采样，因此它不能单独证明本项目 **YUV422→YUV420 CSC** 的实机行为。 |

## 源码与构建证据

1. 本地 IDF `D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_jpeg/jpeg_decode.c:449-452`：最低修订不低于 300 时纳入 `JPEG_DECODE_OUT_FORMAT_YUV420`。同文件 `:541-558`：`sample_method == JPEG_DOWN_SAMPLING_YUV422` 时设置 `DMA2D_CSC_RX_YUV422_TO_YUV420` 并调用 `dma2d_configure_color_space_conversion()`。`:647-670`：低修订才拒绝该组合。`components/hal/esp32p4/include/hal/dma2d_ll.h:506-510` 把该选项映射到硬件 RX 输出选择。
2. `src/demo/build_rtc_mem_b/sdkconfig:1613` 为 `CONFIG_ESP_REV_MIN_FULL=301`。现有启动日志报告芯片 rev 3.2。`301 >= 300`，因此上述编译保护允许该分支；这只说明固件具有能力，不表示业务已启用。
3. `video_streamer.c:2353-2359` 在实际 MJPEG 上调用 `jpeg_decoder_get_info()`。既有实机日志 `logs/h264_send_20260928_175901/serial.log` 报告“JPEG 采样格式：YUV422”；IDF 头解析把 JPEG SOF 采样因子 `0x21` 映射为 `JPEG_DOWN_SAMPLING_YUV422`（`jpeg_decode.c:704-715`）。因此本板摄像头不是官方 m2m README 假定的原生 YUV420 采样。
4. `video_streamer.c:36-39` 把官方直出开关置 0；`:2380-2384` 当前据此选择 YUV422 输出；`:2412-2440` 已有一次性官方直出/CPU 输出比较逻辑，但此构建不会运行；`:2487-2513` 当前继续 CPU 转换。因此不存在“只把枚举改为 YUV420 就自动去掉 CPU”的当前行为。

## 输出内存布局和尺寸

ESP-IDF 5.5.5 JPEG 文档的 **Pixel Storage Layout / YUV420** 图定义其解码器输出布局；本工程 H264 组件 `src/demo/managed_components/espressif__esp_h264/interface/include/esp_h264_types.h:60-76` 则明确把硬件输入 `ESP_H264_RAW_FMT_O_UYY_E_VYY` 定义为 packed YUV420：偶数行 `U,Y,Y` 重复，奇数行 `V,Y,Y` 重复。本工程在 `video_streamer.c:1584-1592` 选择这个 H264 输入枚举，在 `:665-695` 生成同样的行交错结构。

对宽 `W=1280`、高 `H=720`，紧密存储时：

```text
每行字节数 = W × 3 / 2 = 1920
总字节数   = W × H × 3 / 2 = 1,382,400
偶数行 y，像素对 x：base = y × 1920 + (x/2) × 3
    [base+0] = U(x,y/2), [base+1] = Y(x,y), [base+2] = Y(x+1,y)
奇数行 y+1，同一像素对：base = (y+1) × 1920 + (x/2) × 3
    [base+0] = V(x,y/2), [base+1] = Y(x,y+1), [base+2] = Y(x+1,y+1)
```

这里 **不存在** 位于 `buffer + 921600` 的独立 U 平面，也不存在随后的独立 V 平面；不能按普通 `I420`/`YUV420P` 接线。`video_streamer.c:188-189` 的 H264 输入长度正是 1,382,400。JPEG decoder 在 `jpeg_decode.c:291-297` 根据处理宽高与 12 bpp 计算 `out_size`；1280、720 均为 16 的倍数，无额外 MCU 行列填充，故预期也为 1,382,400。驱动通过 RX DMA2D 描述符写输出缓冲（`:484-488`）。

**实机限制：** 当前没有抓取本板“YUV422 JPEG → 官方 JPEG YUV420 输出”的原始缓冲，所以虽可依据官方格式定义判断布局应可对接，不能证明当前 5.5.5、当前摄像头帧和当前 H264 的零拷贝画面已经正确。尤其 JPEG 异常帧曾有 RST/MCU 错误；不能把解码失败归咎于 CSC，也不能以 `out_size` 正确替代画面验证。CPU 路径的 U/V 是上下两行取平均，硬件 CSC 的舍入方式不一定相同；逐字节不一致也不必然代表布局错误，应检查 Y/UV 位置、色彩和连续编码结果。

## 缓冲生命周期判断

目前 JPEG YUV422 中间缓冲按 `VIDEO_YUV422_SIZE = 1,843,200` 分配，H264 输入缓冲为 1,382,400 字节（`video_streamer.c:186-189, 2016-2018, 2058-2066`）。若直出实测合格，可以让 JPEG RX 写入一块满足 `jpeg_alloc_decoder_mem()` 对齐/可访问约束、且 H264 接受的 1,382,400 字节缓冲，减少 CPU 转换和一个长期缓冲。但这属于下一轮业务代码修改；需要核实 JPEG 与 H264 对缓冲对齐、缓存同步和调用期间占有权的约束，不能在本轮直接删除任何缓冲。

## 官方参考

- [ESP-IDF v5.5.5 `jpeg_decode.c`](https://github.com/espressif/esp-idf/blob/v5.5.5/components/esp_driver_jpeg/jpeg_decode.c)：`jpeg_decoder_get_info()`、`jpeg_decoder_process()`、`jpeg_dec_config_dma_descriptor()`、`jpeg_dec_config_dma_csc()`、`jpeg_color_space_support_check()`。
- [ESP-IDF v5.5 JPEG 文档：Pixel Storage Layout](https://docs.espressif.com/projects/esp-idf/en/release-v5.5/esp32p4/api-reference/peripherals/jpeg.html#pixel-storage-layout-for-different-color-formats)：YUV420 输出字节布局及缓冲要求。
- [esp-video-components `esp_video/README.md`](https://github.com/espressif/esp-video-components/blob/master/esp_video/README.md)：ECO3+ JPEG decoder 的 `V4L2_PIX_FMT_YUV420` 支持表。
- [m2m `README.md`](https://github.com/espressif/esp-video-components/blob/master/esp_video/examples/m2m/README.md)：USB UVC MJPEG → JPEG HW decoder → H264 HW encoder 管线，以及要求摄像头 JPEG 为 YUV420 采样的限定。
- [m2m `example_v4l2.c`](https://github.com/espressif/esp-video-components/blob/master/esp_video/examples/m2m/main/example_v4l2.c)：`open_jpeg_decoder()` 将输出设为 `V4L2_PIX_FMT_YUV420`；`jpeg_decode_connect()`/`h264_encode()` 用 V4L2 格式及缓冲衔接。
- [m2m `m2m_main.c`](https://github.com/espressif/esp-video-components/blob/master/esp_video/examples/m2m/main/m2m_main.c)：`run_pipeline_test()` 依次连接相机、JPEG decoder、H264 encoder，并执行 `jpeg_decode()` → `h264_encode()`。

## 下一轮最小验证门槛（本轮未执行）

仅对一份通过现有 JPEG 完整性过滤的 YUV422 帧作单变量对照：相同输入分别解为 YUV422 与官方 YUV420，记录 `jpeg_decoder_process()` 返回值、`out_size`、行布局/颜色、耗时；将官方输出送现有 H264 编码并连续观察预览。CPU 输出可用于视觉和布局比较，但不能把严格逐字节相等作为唯一通过条件。验证通过后才考虑删除 CPU 函数和 YUV422 中间缓冲。
