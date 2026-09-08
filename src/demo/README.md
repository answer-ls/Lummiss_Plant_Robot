# Lummiss 硬件可行性测试工程

本工程基于 ESP-IDF 5.5.5，面向 ESP32-P4。开发板原始示例保留在 `../esp_draw_bit`，本工程只保留当前硬件测试需要的依赖。

项目整体进度、已知问题和后续任务见根目录 `../../PROJECT_HANDOFF.md`。

## 当前默认测试：ST7789 屏幕

GMT020-02-8P 按 240×320、RGB565、4 线 SPI 驱动，接线如下：

| 屏幕引脚 | ESP32-P4 |
| --- | --- |
| GND | GND |
| VCC | 3V3 |
| SCL / SCLK | GPIO20 |
| SDA / MOSI | GPIO32 |
| RST / RES | GPIO3 |
| DC | GPIO2 |
| CS | GPIO1 |
| BL / BLK | 3V3 |

屏幕程序使用 LVGL 8.4 循环显示 EXP-01 中性待机、EXP-02 微笑和 EXP-03 开心三个表情。中性表情带随机眨眼和轻微呼吸，微笑表情使用弧线眼睛，开心表情带脸颊、张嘴和上下弹动。BL 直接接 3V3，因此程序不能控制背光。

当前 `.vscode/settings.json` 已默认选择屏幕模式，用户可直接在 VS Code 中构建和烧录。屏幕程序已在 ESP-IDF 5.5.5 下编译通过，实机显示参数仍需烧录确认。

开发板上的 ESP32-P4 实测为 v1.3，工程已选择 v1.x 芯片分支。若烧录日志仍显示镜像要求 `v3.1 - v3.99`，说明使用了修改前的旧镜像，应先重新构建；不要添加 `--force`。

配置中必须保留 `-G Ninja`。如果日志显示 `Building for: NMake Makefiles`，说明 VS Code 没有重新加载本工程的设置；重新加载窗口后再构建。

```powershell
Set-Location E:\Lummiss_Plant_Robot\src\demo
idf.py -B build_screen_verified -DLUMMISS_TEST=screen build
idf.py -B build_screen_verified -p COM17 flash monitor
```

## 切换到 USB 摄像头测试

摄像头程序验证 ESP32-P4 高速 USB 口连接的 LRCPG720p UVC 摄像头。该摄像头此前已实测 640×480 MJPEG 30 FPS 稳定收帧。

```powershell
Set-Location E:\Lummiss_Plant_Robot\src\demo
idf.py -B build_camera_verified -DLUMMISS_TEST=camera build
idf.py -B build_camera_verified -p COM17 flash monitor
```

摄像头正常时，串口会出现 `Supported[...]`、`Stream started`，且每 5 秒输出的 `RX frames` 和 `bytes` 持续增长，`fps` 约为 30，`last` 大于 0，`empty=0`。
