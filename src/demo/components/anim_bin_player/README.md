# LUM1 动画播放器

播放器从 TF 卡逐帧读取未经压缩的 RGB565 动画。`screen_carousel` 只传入文件路径，
不会依赖文件头或帧表结构。

文件头和帧表中的整数固定使用小端字节序。ESP32-P4 显示链路启用了
`CONFIG_LV_COLOR_16_SWAP=y`，所以帧像素固定使用 RGB565-BE：红色 `0xF800`
在文件中写为 `F8 00`。`flags` 在版本 1 中必须为 0。

播放器初始化时在 PSRAM 中一次分配两个 153600 字节缓冲。LVGL 正在显示 A 时，
播放器任务只向 B 执行 `fread()`；LVGL Timer 切换到 B 并回送确认后，任务才允许
覆盖 A。停止播放时缓冲继续保留，因此不会发生逐帧 `malloc/free`。

播放器常驻一块 16 KB 内部 DMA 读缓存，分块读取后复制到非显示中的 PSRAM
Buffer。这样可避免 FatFS 为每帧临时申请约 150 KB 的 bounce buffer，并避免
内部堆不足时出现 `allocate_dma_buf` 失败。

只有 LVGL Timer 会调用 `lv_img_set_src()`。播放器任务负责文件校验、SD 读取和
帧时序，不直接访问 LVGL API。播放错误由 `screen_carousel` 检测并安全返回首页。

转换与校验示例：

```powershell
python tools\gif_to_rgb565_bin.py tools\sd_gif_out\exp_01.gif tools\gif\exp_01.bin `
  --width 320 --height 240 --resize contain --background 000000 --byte-order be
python tools\validate_anim_bin.py tools\gif\exp_01.bin --byte-order be
```

`CONFIG_LUMMISS_ANIM_FIRST_FRAME_ONLY` 仅供第一帧和颜色验证，正常固件必须关闭。
