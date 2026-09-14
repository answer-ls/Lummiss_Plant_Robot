# 800×600 浏览器 20 FPS 实机验证

日期：2026-09-12。开发板 ESP32-P4 v1.3，ESP-IDF 5.5.5，COM17。

> **⚠️ 本文的根因结论已于同日被后续一轮日志推翻，请以
> `../../../PROJECT_HANDOFF.md` 第 7 节和附录 A.5/A.6 为准。**
>
> 保留下来的部分：A.1 的缓冲容量对照数据、`uvc_host.c` double-free 那段源码依据、
> 复测工具说明。**已被推翻的部分**：
>
> | 本文原来的说法 | 后续实测 |
> | --- | --- |
> | 丢帧根因是 URB 落 PSRAM 后与 H.264/JPEG 争仲裁 | 完整档位下 URB 本就落 PSRAM，而**关掉编解码链路丢帧就从 23.8% 掉到 2.1%**；URB 位置不是稳态丢帧的变量 |
> | `callback_gap_max` 从 9 ms 抬到 22 ms 是 WiFi/LVGL/SD 抢带宽的代价 | 后续完整档位日志里它**稳定在 9 ms**，与丢帧率不相关（丢帧 23.8% 和 2.1% 两种状态都是 9 ms）。这是窗口内最大值统计，不适合当判据 |
> | 丢帧在 t≈34.5 s 跳变与 ESP-TLS 超时 / GIF 轮播重启重合，是一条二分线索 | **已证伪**：后续日志里那些事件在丢帧仅 2.1% 的窗口内照常发生 |
> | 下一步逐档二分定位额外丢帧来源 | **作废**：判据（`callback_gap_max`）对档位不敏感，只会得到噪声 |
>
> 保留下来的机理描述中，"PSRAM 仲裁延迟 ISOC 回调"这一套解释同时被推翻——若成立，
> `callback_gap_max` 应在丢帧时同步升高，而实测并非如此。缩容到 8×16 KB 为什么能把丢帧
> 从 35% 压到 10%，**目前没有解释**。

## 测试方法

相机保持 800×600 MJPEG、设备协商 30 FPS；编码上限 20 FPS、目标码率 4 Mbps。
各组保留相同 YUV CPU 分块转换、JPEG 结构校验、UVC 组帧和启动恢复流程。
短测约 60～70 秒，汇总时排除设备启动后前 15 秒。表中帧率为串口窗口均值，
短测不能替代长期稳定性验收。空包、skipped、invalid 等累计数需要比较增量。

原始日志、固件快照与构建输出：`test_results/20260912_cache/`（本机保留，不入 Git）。

| 组别 | UVC FPS | UVC 丢帧 | H.264 FPS | H.264 耗时 | 结论 |
| --- | ---: | ---: | ---: | ---: | --- |
| 原固件，USB 96×32 KB PSRAM | 19.43 | 35.23% | 17.12 | 8.04 ms | 复现持续 skipped；空包在本轮保持不变 |
| 仅启用 16 KB 缓存回写分块 | 20.22 | 32.58% | 17.47 | 8.28 ms | 未解决 |
| 再将 H.264 输出槽由 720000 B 缩至 128 KB | 19.75 | 34.17% | 17.48 | 8.28 ms | 节省约 2.25 MiB PSRAM，未解决 |
| 再将 H.264 DMA burst 从 128 B 改为 16 B | 17.28 | 42.37% | 11.50 | 21.44 ms | 负优化，已恢复默认 128 B |
| USB 8×16 KB 内部 RAM，默认 H.264 burst（**codec-only 档位**） | 30.05 | 0% | 19.99 | 8.15 ms | 11 个稳态窗口 skipped/invalid/丢整帧均为零 |
| USB 8×16 KB PSRAM（反向对照，codec-only 档位） | 27.0 | 10.2% | 19.9 | 8.27 ms | 丢帧回到 7~15%，callback_gap_max 1→8 ms |
| **完整档位（FULL：WiFi+UI+SD+WS），8×16 KB + `DMA_CAP_MEMORY_IN_PSRAM=y`** | 20.5~30.0 | 2.6~32%（均值 ~16%） | 15.1~20.1 | 8.3 ms | 见下文「完整档位复测」 |

注意：USB 请求的 16 KB 被 MPS=3072 向上对齐为 18432 B，每个 URB 包含 6 个
ISOC 服务周期，8 个数据缓冲合计 147456 B（144 KiB），另有描述符和控制缓冲。
**上表最后两行的「内部 RAM」方案只在 codec-only 档位成立，完整档位下不可用**——
见下文。

## 完整档位复测：内部 RAM 方案在 FULL 档位不可用

第七组（8×16 KB 内部 RAM）当时跑的是 `CAMERA_TEST_UVC_H264_ONLY`（档位 6）。
把同样配置搬到完整档位（档位 0，WiFi + LVGL + SD GIF + WebSocket）后，设备直接进入
**开机重启循环**，不是丢帧变多：

```text
E uvc: Could not allocate USB transfers
assert failed: heap_caps_free ... free() target pointer is outside heap areas
  uvc_transfers_free → uvc_device_remove → uvc_host_stream_open → camera_driver_run
```

两个原因叠加：

1. **144 KiB 挤不进 146 KiB**。启动日志 `esp_psram: Reserving pool of 146K of internal
   memory for DMA/internal allocations`——内部 DMA 池总共只有 146 KiB。codec-only 档位
   里摄像头是第一个来抢池子的，144 挤 146 刚好够；完整档位的 WiFi(SDIO)、LVGL 和
   LCD SPI DMA 会先占用同一个池，UVC 必然分配失败。
2. **分配失败被组件放大成 double free**。`uvc_host.c` 的 `uvc_transfers_free()`（约
   341 行）释放后既不置空 `xfers` 也不归零 `num_of_xfers`，而 `uvc_host_stream_open`
   的错误路径（约 880 行 → `uvc_device_remove`）会二次调用它。组件在
   `espressif__usb_host_uvc` 2.5.1 更新后没有本地补丁，所以这个坑重新打开了。

规避方式是让 URB 落 PSRAM：`CONFIG_USB_HOST_DWC_DMA_CAP_MEMORY_IN_PSRAM=y`。

### 完整档位实测（`CAMERA_TEST_FULL`，URB 8×16 KB + 落 PSRAM）

WiFi 在 t=11.6 s 拿到 `192.168.1.32` 并连上 WebSocket，8 个 USB transfer 分配成功。
约 70 秒稳态（表中 `callback_gap_max` 一列保留原样但**已确认不可用作判据**——后续同档位
日志里它恒为 9 ms）：

| 时刻 | UVC complete | UVC drop | encoded | callback_gap_max |
| --- | ---: | ---: | ---: | ---: |
| t=19.4 s | 28.5 | 5.3% | 19.7 | 22 ms |
| t=24.5 s | 29.3 | 2.6% | 20.1 | 22 ms |
| t=29.5 s | 28.6 | 4.6% | 19.7 | 22 ms |
| t=34.5 s | 24.5 | 18.0% | 17.3 | 22 ms |
| t=39.5 s | 22.7 | 24.5% | 15.1 | 22 ms |
| t=44.6 s | 20.5 | 31.8% | 15.9 | 22 ms |
| t=49.6 s | 24.3 | 19.2% | 17.9 | 22 ms |
| t=54.6 s | 21.9 | 27.2% | 16.9 | 22 ms |
| t=59.6 s | 25.5 | 14.7% | 19.1 | 22 ms |
| t=64.7 s | 24.9 | 17.2% | 19.1 | 22 ms |
| t=69.7 s | 26.5 | 11.9% | 19.3 | 22 ms |

读数：

- **20 FPS 目标未达成**，H.264 编码在 15.1~20.1 FPS 之间，只有负载最低的窗口才压线达标。
- ~~`callback_gap_max` 稳定在 **22 ms**（codec-only 是 9 ms），说明完整档位的 PSRAM
  争用比 codec-only 严重得多——这 13 ms 的增量就是 WiFi(SDIO)/LVGL/SD/GIF 抢带宽的代价。~~
  **已撤回**，见文首。后续完整档位日志里它是 9 ms，对档位和丢帧率都不敏感。
- ~~**丢帧在 t≈34.5 s 从 2.6~5.3% 跳到 12~32%**，时间点与
  `W esp-tls: Failed to open new connection in specified timeout` +
  `HOME_INFO: HTTPS 请求失败：ESP_ERR_HTTP_CONNECT` + `CAROUSEL: GIF 预读完成 [1/8]`
  的循环重启重合。这是一条明确的二分线索，但尚未隔离。~~
  **已证伪**，见文首。那些事件在丢帧仅 2.1% 的窗口内照常发生。
- **上传路径不是瓶颈**：全程 `sent == encoded`、`send_fail=0`。（这条仍然成立。）
- `encoded < complete` 是因为 `video_streamer_submit_jpeg()` 的 20 FPS 门控
  （`video_streamer.c:1374-1389`）把超出的帧计入 **`rate_limited`**，而这个计数器
  没被周期性日志打印——所以 `drop_input=0` 不代表没有丢帧。
  **已修**：2026-09-12 把 `rate_limit=` 加进了 `[VIDEO] Queue` 行。

## 源码依据

- IDF 的 `CONFIG_USB_HOST_DWC_DMA_CAP_MEMORY_IN_PSRAM` 同时控制 USB URB 数据缓冲和
  HCD DMA 描述符内存位置；关闭后两者使用内部 RAM。UVC 完整帧缓冲仍可留在 PSRAM。
- HCD 每条管道使用两个 DMA 描述符缓冲，其余 URB 在软件队列等待；不能把 96 个
  URB 的时间长度直接当成硬件免维护窗口。
- H.264 输入为 800×600×1.5=720000 B；此固件启动时可分配内部内存约 435 KiB，
  还需容纳系统与编码器工作区，不能直接把该整帧输入搬入内部 RAM。
- H.264 默认 DMA burst 编码为 4，即 128 B；对照编码 1 对应 16 B。
- `input_invalid` 包含缺 EOI 和结构检查不通过等多种情况；原 `missing_eoi_frames`
  未递增，不能根据原日志“EOI 缺失=0”证明帧尾完整。

## 复测工具

```powershell
python tools/capture_camera_serial.py --port COM17 --seconds 70 --output test_results/run.log
python tools/summarize_camera_serial.py test_results/run.log
```

采集工具不主动复位设备；日志汇总默认跳过设备启动后前 15 秒，分别报告窗口均值和累计数增量。

## 结论（已按 2026-09-12 第三轮日志修订）

**URB 大小和内存位置这两条结论要分开看。**

反向对照的数据（均在 codec-only 档位）：

| 变量组合 | UVC 丢帧 | 是否仍然成立 |
| --- | ---: | --- |
| 96×32 KB · PSRAM（原始） | ~35% | 数据成立 |
| 8×16 KB · PSRAM（反向对照） | ~10% | 数据成立 |
| 8×16 KB · 内部 RAM（**仅 codec-only 档位成立**） | 0% | 数据成立，但不可推广 |

- **URB 96→8（都在 PSRAM）**：35% → 10%。数据成立，**但原因未解释**——原先配套的
  "ISOC 回调被 PSRAM 仲裁推迟"机理已被推翻，见下。
- **PSRAM→内部 RAM（都是 8×16 KB）**：10% → 0%。这条只在 codec-only 档位成立。
- ~~机理证据：`callback_gap_max` 从内部 RAM 的 1 ms 涨到 PSRAM 的 4~8 ms。USB HCD 的
  DMA 访问与 H.264/JPEG 解码争 PSRAM 仲裁，ISOC 回调最长被推迟，期间等时包被跳过
  （`skipped` 与 `dropped` 严格 1:1，一个包毁一帧 MJPEG），整帧丢弃。~~
  **机理已撤回。** 后续完整档位日志里 `callback_gap_max` 在丢帧 23.8% 和 2.1% 两种状态下
  都是 9 ms——若回调推迟是原因，这个值应该同步升高。顺带修正：`skipped` 与 `dropped`
  并非严格 1:1，实测约 **1.74:1**（757 次跳过 / 435 个丢帧），因为 ISOC 错误会成串出现。
- **「落内部 RAM」这条清零路径不能用于完整档位**：144 KiB 挤不进 146 KiB 的内部
  DMA 池，会先分配失败、再被组件 double-free 放大成开机重启循环（见上文「完整档位
  复测」）。这条**仍然成立**，而且它与稳态丢帧是两个问题——完整档位下 URB 本来就落在
  PSRAM，而把编解码链路关掉，丢帧立刻从 23.8% 掉到 2.1%。
- **最终配置固定为 `8 × 16 KB URB + CONFIG_USB_HOST_DWC_DMA_CAP_MEMORY_IN_PSRAM=y`。**
  这条配置防的是重启循环，不是丢帧。完整档位已跑通，丢帧 19%~29%、H.264 只有
  15~20 FPS——**20 FPS 目标未达成**。

## 尚待完成

- **降低编解码链路的 PSRAM 流量**（当前唯一还在动的变量）。YUV 重排 15.3 ms 是三项里
  最大的一项，优先评估其替代路径；其次是 JPEG 解码 6.7 ms 的 PSRAM 读写量。
- ~~逐档二分定位完整档位的额外丢帧来源~~ **作废**：判据 `callback_gap_max` 对档位和
  丢帧率都不敏感（两种状态都是 9 ms），用它做二分只会得到噪声。要分离变量就用已验证
  有效的手段——直接开/关整条编解码链路（WebSocket 断线就是这个效果）。
- ~~打印 `rate_limited` 计数器~~ **已完成**：已加入 `[VIDEO] Queue` 行，同时给
  `invalid` 加了按原因拆分的归因行（`head/marker/segment/sof/no_sos/scan/no_eoi`），
  用于确认"ISOC 丢包打坏帧头、SOI/EOI 尚存"这个推断。
- **复测 WebSocket 断线修复**：写超时 100→2000 ms、重连 8000→2000 ms（见
  `../../../PROJECT_HANDOFF.md` 附录 A.6）。
- **给 `uvc_host.c` 重打幂等补丁**：`uvc_transfers_free()` 释放后置空 `xfers` 并归零
  `num_of_xfers`，让分配失败退回成一条错误日志而不是重启循环。
- 连续至少 10 分钟的串口、服务器接收与预览验证。
