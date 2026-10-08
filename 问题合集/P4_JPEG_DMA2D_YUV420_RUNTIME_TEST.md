# P4 JPEG RX DMA2D YUV420 运行 A/B 测试

状态：**B 路径已有板端日志证明直出、编码及发送持续工作；浏览器颜色和连续 60 秒仍待确认，不能宣称完整通过。** 本轮保持 1280×720、20 fps、H264 参数、JPEG 完整性过滤、WebRTC、任务/核心/优先级和内存配置不变。未删除 CPU 转换函数或 YUV422 缓冲；未新增独立 DMA2D 模块。

## 唯一变量与固件

原始 A/B 的唯一变量是 `src/demo/components/video_streamer/video_streamer.c` 顶部 `VIDEO_JPEG_DMA2D_AB_PATH_B`：A 为 0，B 为 1。两份原始固件均从同一份代码、同一个 `src/demo/build_rtc_mem_b/sdkconfig` 构建。当前源文件及 IDF 扩展使用的 `src/demo/build_rtc_mem_b` 留在 **B 静音日志版**状态；下表的原始 A/B 镜像保留以供追溯。

| 组别 | JPEG 输出 | 转换 | 应用镜像 | SHA-256 |
| --- | --- | --- | --- | --- |
| A | `JPEG_DECODE_OUT_FORMAT_YUV422` | CPU `yuv422_to_h264_yuv420()` | `logs/jpeg_dma2d_ab_20260929/A_CPU/lummiss_main.bin` | `377B7624D84C1A68A1483872512054304AF1AB77C08341ECD16FD2303414F6A9` |
| B | `JPEG_DECODE_OUT_FORMAT_YUV420` | JPEG decoder RX DMA2D CSC；失败后回退 CPU | `logs/jpeg_dma2d_ab_20260929/B_DMA2D/lummiss_main.bin` | `E9A21C248CC185542B27D4AC0735FE1FB88BB046CD8A78B574617B0CA76236B2` |

对应 ELF 与构建日志也在各组目录和 `logs/jpeg_dma2d_ab_20260929/build_A.log`、`build_B.log`。A/B 均已通过编译和镜像大小检查；默认 `src/demo/build` 也已通过构建，但不作为 A/B 固件。

## 实施细节

- 首帧用 `jpeg_decoder_get_info()` 读取实际采样；现有日志已证明摄像头为 YUV422。B 组向 IDF JPEG 驱动传 `JPEG_DECODE_OUT_FORMAT_YUV420`，据 IDF v5.5.5 `jpeg_decode.c` 的 YUV422 分支，这会选择 `DMA2D_CSC_RX_YUV422_TO_YUV420`。应用日志证明传入的格式与返回结果，不是对 DMA2D 寄存器的独立读回。
- B 组直出成功时直接把 JPEG 输出缓冲送现有 `ESP_H264_RAW_FMT_O_UYY_E_VYY` 编码器，CPU 转换时间应为 0。首次成功直出打印前两行各 12 字节和行步长 `1920`，按偶行 `U Y Y`、奇行 `V Y Y` 解读。
- B 组若直出返回错误或大小不等于 1,382,400，则记录失败并对**同一帧**重新解成 YUV422，随后使用原 CPU 转换；本次启动余下帧停用 B。不能把这些回退帧算作 B 成功。JPEG 完整性过滤与 H264 错误处理保持原状。
- 原始 A/B 每个进入 H264 的帧打印一条 `JPEG_AB: FRAME`。该日志会刷屏，因此当前 B 静音日志版只保留启动后的前三帧详细信息；随后每 5 秒打印 `encoded_direct/encoded_cpu`，现有视频统计继续提供 JPEG/CPU/H264 耗时与编码/发送 fps，`TASK_LOAD_5S` 提供 CPU1/codec 占用。首次解码失败仍保留详细原因。
- 本轮删除的是旧的“硬件与 CPU 输出必须逐字节一致”首帧探针判断，不是 CPU 回退。硬件和 CPU 下采样的取整可能不同，逐字节不等不能单独判定布局错误。

## 烧录和采集

此前用户选择自行烧录；本轮未主动烧录。A、B 都只需更新 app 分区 `0x10000`，沿用当前板上的 bootloader、partition table 和数据分区。关闭占用 COM7 的串口监视后，在 `src/demo` 目录执行，先 A 后 B，每组冷启动，打开浏览器预览并连续运行至少 60 秒，观察颜色与运动：

```powershell
$py = 'C:\Espressif\tools\python\v5.5.5\venv\Scripts\python.exe'
$esptool = 'D:\espidf5.5.5\.espressif\v5.5.5\esp-idf\components\esptool_py\esptool\esptool.py'
& $py $esptool -p COM7 -b 460800 --before default_reset --after hard_reset --chip esp32p4 write_flash 0x10000 'E:\Lummiss_Plant_Robot\logs\jpeg_dma2d_ab_20260929\A_CPU\lummiss_main.bin'
# A 的 60 秒运行完成并保存日志后，再刷 B：
& $py $esptool -p COM7 -b 460800 --before default_reset --after hard_reset --chip esp32p4 write_flash 0x10000 'E:\Lummiss_Plant_Robot\logs\jpeg_dma2d_ab_20260929\B_DMA2D\lummiss_main.bin'
```

也可以用 IDF 扩展烧录 B，因为 `src/demo/build_rtc_mem_b` 当前就是 B。扩展的普通 Flash 任务可能连同模型等分区一起写入，耗时较长；A 需指定保存的 A 应用镜像。每组串口日志保存至项目根目录 `logs/jpeg_dma2d_ab_20260929/`。

## 运行结果待填

首次 B 板端日志（用户提供的约 119 秒处片段）显示：`actual=DMA2D_YUV420`、JPEG `ESP_OK`、输出 1,382,400 B、H264 返回 0、CPU 转换 0；五秒统计编码和发送均约 13.7 fps，CPU1 约 66%。该片段证明 B 路径正在运行，但未提供浏览器颜色及连续 60 秒的完整证据。

为避免逐帧串口打印干扰测试，已另存 B 静音日志版 `logs/jpeg_dma2d_ab_20260929/B_DMA2D/lummiss_main_quiet.bin`，SHA-256 为 `81FEDD83B3E639662025D122B3D2807BA87D556291C9A6DB14AB68550E43CD34`；其 ELF 同目录。IDF 扩展的 `src/demo/build_rtc_mem_b` 也已更新到该版，`src/demo/build` 同步编译通过。未自动烧录。

| 指标 | A：CPU | B：JPEG RX DMA2D |
| --- | --- | --- |
| 实际路径 `JPEG_AB FRAME actual` | 待实机 | 待实机；必须持续为 `DMA2D_YUV420` |
| JPEG 返回值 / 输出大小 | 待实机；预期 1,843,200 B | 待实机；预期 `ESP_OK / 1,382,400 B` |
| JPEG 平均 / 最大耗时 | 待实机 | 待实机 |
| CPU 转换平均 / 最大耗时 | 待实机；历史约 30 ms/帧 | 待实机；预期 0 |
| H264 平均 / 最大耗时及成功率 | 待实机 | 待实机 |
| 阶段总耗时、CPU1/codec 占用 | 待实机 | 待实机 |
| encoded/sent fps | 待实机 | 待实机 |
| 浏览器颜色、连续运动、60 秒 | 待实机 | 待实机 |
| WDT、DMA/heap 错误 | 待实机 | 待实机 |

**判定要求：** B 的 `jpeg_ret=ESP_OK`、输出 1,382,400 B、`actual=DMA2D_YUV420`，H264 连续成功；浏览器无绿/紫屏或明显色偏、画面连续运行 ≥60 秒，且无 WDT、DMA/heap 错误。若触发 `CPU_FALLBACK_OR_A`，说明 B 未通过，即使预览继续可用也不能算成功。串口逐帧日志本身会占用 CPU1/串口带宽，A/B 均保留同样日志以便对比；绝对 fps 可能低于关闭诊断后的水平。
