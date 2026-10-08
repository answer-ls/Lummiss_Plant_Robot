# WHIP 单向视频首阶段（640×480）

本阶段沿用现有 USB UVC 摄像头、JPEG 硬解、YUV 转换和 H.264 硬编，YOLO 仍从同一摄像头抽帧。`media.webrtc.start` 通过 MQTT/MCP 下发 `sessionId`、`publishUrl`、`streamKey`、`roomName`、`video`；首阶段 `video` 必须为 H264 640×480@20。设备原样 POST 服务端的 `publishUrl`，不使用旧 `rtc_signal`。音频仍走原有 UDP Opus，不加入 WHIP。

启动顺序：MCP 快速回 `accepted` → 后台创建 `esp_peer` 视频发送轨道 → 收集 ICE 候选（明确结束标志或 12 秒收集超时）→ HTTPS POST SDP → 检查 HTTP 201、保存 Location、设置 Answer → ICE/DTLS-SRTP 连接成功 → 开启 H.264 编码门控 → 将原始 Annex-B 帧交给 `esp_peer` 的 RTP/SRTP 发送路径。`stop` 只停编码、DELETE Location、关闭 PeerConnection；UVC/YOLO 继续运行。

构建使用 `sdkconfig.bletest` 和 `build`，依赖 `espressif/esp_peer=1.5.3`、`espressif/esp_libsrtp=1.0.0`，并启用 mbedTLS DTLS-SRTP。当前完成了编译和链接；**尚未实机验证 HTTP 201、LiveKit 画面、30 分钟连续运行和 20 次启停**。烧录与串口监视由用户执行。

联调时先确认服务端下发的不是旧版 `credentialPath/deviceTicket/offerer=APP` 指令，且 `video.width=640`、`height=480`、`fps=20`。串口应依次看到 `state IDLE -> STARTING`、`ICE gathering complete`（或有候选的收集超时）、`WHIP response HTTP=201`、`state STARTING -> CONNECTING`、`ICE/DTLS state=CONNECTED`、`state CONNECTING -> STREAMING`，其后每 5 秒输出 `[WEBRTC]` 统计。日志不打印 URL、streamKey、Authorization 或完整 SDP。旧 `VIDEO_STREAM` 统计仍可用于对比编码与发送 FPS，`CAMERA`、`PERSON_DETECT`、`AFE_DIAG` 则验证原链路未退化。

目前 `esp_peer` 公共 API 未提供收到 RTCP PLI/FIR 的回调，本阶段连接时主动请求 IDR，GOP 为 20（20 FPS 下 1 秒）。PLI 计数与即时 IDR 仍需组件支持。另有**明确的 Profile 风险**：当前 `esp_h264` 组件的 `h264_nal.c` 为硬件编码器固定写入 `profile_idc=66`、约束标志 `0xC0`，按 640×480@20 查表得到 level 3.0，因此预期 SPS 是 `42c01e`，与服务端要求的 `42e01f` 不完全相同。不能凭编译结果宣称符合要求，也不能只改 SDP 伪装编码数据。本阶段先在串口打印 offer 是否包含 `42e01f`、`packetization-mode=1`，联调时须抓实际 SPS 并与服务端确认能否接受或调整编码器生成方式。当前完整系统在 YOLO 后内部 DMA 空间很紧，WHIP 建链时要观察 `MEM[PEER_BEFORE]`、`MEM[PEER_OPEN]`、`MEM[WHIP_POST_OK]`，若出现 `ESP_ERR_NO_MEM`，先定位分配来源。
