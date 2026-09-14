# 嵌入式前期联调文档：OTA 与 WebSocket（外部交付版）

> 文档版本：1.2
> 更新时间：2026-09-12
> 适用环境：`www.lummiss.com` 公网联调环境
> 适用对象：ESP32/嵌入式开发和测试人员
> 信息范围：仅包含设备接入所需协议，不包含服务端部署、网络拓扑、管理后台和密钥配置

## 1. 联调参数速查

| 项目 | 公网联调值 | 说明 |
|---|---|---|
| OTA 检查 | `POST https://www.lummiss.com/lummiss/ota/` | 固件启动后先调用 |
| Agent WebSocket | `wss://www.lummiss.com/server/lummiss/v1/` | 最终必须优先使用 OTA 响应的 `websocket.url` |
| 固件下载 | OTA 响应的 `firmware.url` | 不允许自行拼接或写死 |
| OTA 传输 | HTTPS，TLS 1.2/1.3 | 设备必须校验证书链 |
| WebSocket 传输 | WSS，TLS 1.2/1.3 | 设备必须校验证书链 |

设备只允许使用表中提供的 HTTPS/WSS 地址，不得改用 IP、明文 HTTP/WS、其他端口或自行推测服务路径。

### 1.1 当前在线检查结果

2026-09-12 公网联调检查结果：

- OTA 设备上报返回 HTTP 200；
- WebSocket Upgrade 握手返回 HTTP 101；
- 携带设备身份和 OTA 动态 Token 后，应用层 Hello 往返成功。

因此 OTA 和 WebSocket 公网链路均已连通。未携带设备身份和 Token 的探测连接可能收到提示并被关闭；完整设备鉴权必须使用真实 OTA 响应中的 Token。

以上状态是时间点记录。设备不能据此写死服务状态，正式联调前应重新检查。

### 1.2 本次端到端联调结论

2026-09-12 10:46（Asia/Shanghai）使用测试设备完成了 OTA、设备激活、WSS 鉴权和应用层 Hello 联调：

| 检查项 | 实测结果 | 结论 |
|---|---|---|
| OTA 设备上报 | 返回 HTTPS/WSS 公网地址和动态 Token | 通过 |
| 设备激活 | 绑定后 OTA 响应不再包含 `activation` | 通过 |
| WebSocket Upgrade | 客户端显示已连接 | 通过 |
| WebSocket 鉴权 | 携带三个业务请求头后连接保持正常 | 通过 |
| 客户端 Hello | 文本帧发送成功 | 通过 |
| 服务端 Hello | 返回 `session_id` 和下行音频参数 | 通过 |

联调请求头示例如下。当前已为外包联调绑定测试设备 `E2:67:19:06:41:4E`，Token 不写入文档：

| 参数名 | 参数值 | 类型 | 说明 |
|---|---|---|---|
| `Device-Id` | `E2:67:19:06:41:4E` | string | 已绑定的外包联调测试设备 MAC |
| `Client-Id` | `<设备持久化客户端标识>` | string | OTA 和 WSS 保持一致 |
| `Authorization` | `Bearer <OTA 动态 Token>` | string | Token 已脱敏，`Bearer` 后必须有一个空格 |

当前结论：公网 OTA、TLS、WebSocket、设备鉴权和 Hello 协议均已通过。尚未在本次记录中验收 Opus 音频上行、STT/TTS 下行和会话打断。

## 2. 设备身份约定

### 2.1 Device-Id

- 使用设备稳定、唯一的 MAC 地址；
- 推荐格式：大写十六进制并使用冒号分隔，例如 `AA:BB:CC:DD:EE:FF`；
- OTA 请求和 WebSocket 握手必须使用同一个值；
- 设备重启、升级后不得变化。

### 2.2 Client-Id

- 设备首次启动时生成 UUID，并持久化到 NVS；
- 示例：`550e8400-e29b-41d4-a716-446655440000`；
- OTA 请求和 WebSocket 握手必须使用同一个值；
- Token 与 `Client-Id + Device-Id` 绑定，更换 Client-Id 后原 Token 无法通过鉴权。

## 3. OTA 接口

### 3.1 请求

```http
POST /lummiss/ota/ HTTP/1.1
Host: www.lummiss.com
Content-Type: application/json
Accept: application/json
Device-Id: AA:BB:CC:DD:EE:FF
Client-Id: 550e8400-e29b-41d4-a716-446655440000
```

请求头说明：

| 请求头 | 必填 | 说明 |
|---|---:|---|
| `Content-Type` | 是 | 固定为 `application/json` |
| `Accept` | 建议 | 固定为 `application/json` |
| `Device-Id` | 是 | 设备 MAC，参见第 2.1 节 |
| `Client-Id` | 是 | 设备持久化 UUID，参见第 2.2 节 |
| `Authorization` | 否 | OTA 接口不需要 Bearer Token |

最小请求体：

```json
{
  "application": {
    "name": "desktop-pet",
    "version": "1.0.0"
  },
  "board": {
    "type": "DESKTOP_PET_V1",
    "mac": "AA:BB:CC:DD:EE:FF"
  }
}
```

推荐完整请求体：

```json
{
  "version": 1,
  "flash_size": 16777216,
  "minimum_free_heap_size": 98304,
  "mac_address": "AA:BB:CC:DD:EE:FF",
  "uuid": "550e8400-e29b-41d4-a716-446655440000",
  "chip_model_name": "ESP32-S3",
  "chip_info": {
    "model": 9,
    "cores": 2,
    "revision": 1,
    "features": 0
  },
  "application": {
    "name": "desktop-pet",
    "version": "1.0.0",
    "compile_time": "2026-09-12T01:00:00Z",
    "idf_version": "v5.4",
    "elf_sha256": "<当前固件 ELF SHA-256>"
  },
  "ota": {
    "label": "ota_0"
  },
  "board": {
    "type": "DESKTOP_PET_V1",
    "ssid": "<当前 Wi-Fi SSID>",
    "rssi": -52,
    "channel": 6,
    "ip": "192.168.1.30",
    "mac": "AA:BB:CC:DD:EE:FF"
  }
}
```

字段要求：

- `application.version` 必须提供，使用可比较的版本号，例如 `1.0.0`；
- `board.type` 必须与后台固件型号一致，当前桌宠使用 `DESKTOP_PET_V1`；
- JSON 字段名区分大小写；
- SSID、Token 等敏感信息不能完整写入云日志。

### 3.2 已绑定设备响应

```json
{
  "server_time": {
    "timestamp": 1789174800000,
    "timeZone": "Asia/Shanghai",
    "timezone_offset": 480
  },
  "firmware": {
    "version": "1.0.1",
    "url": "https://www.lummiss.com/lummiss/otaMag/download/<uuid>"
  },
  "websocket": {
    "url": "wss://www.lummiss.com/server/lummiss/v1/",
    "token": "<opaque-token>"
  }
}
```

处理规则：

1. 同时检查 HTTP 状态码、JSON 是否可解析以及顶层 `error` 字段；
2. 保存 `server_time`，用于校正设备时间；
3. 只使用响应中的 `websocket.url` 和 `websocket.token` 建连；
4. `firmware.url` 非空且不是无效占位地址时，才进入升级流程；
5. 固件下载到备用 OTA 分区，验证镜像后再切换，启动失败必须回滚。

### 3.3 未绑定设备响应

```json
{
  "server_time": {
    "timestamp": 1789174800000,
    "timeZone": "Asia/Shanghai",
    "timezone_offset": 480
  },
  "activation": {
    "code": "460609",
    "message": "<智控台绑定地址>\n460609",
    "challenge": "AA:BB:CC:DD:EE:FF"
  },
  "firmware": {
    "version": "1.0.0",
    "url": "<包含 NOT_ACTIVATED_FIRMWARE_THIS_IS_A_INVALID_URL 的不可下载占位地址>"
  },
  "websocket": {
    "url": "wss://www.lummiss.com/server/lummiss/v1/",
    "token": "<opaque-token>"
  }
}
```

设备收到 `activation` 后应显示或播报 `activation.message`，并等待用户完成绑定。包含 `NOT_ACTIVATED_FIRMWARE_THIS_IS_A_INVALID_URL` 的地址是占位值，严禁下载。

本次实测中，设备完成绑定后，OTA 响应中的 `activation` 字段已消失，说明激活成功。绑定后的响应仍可能返回上述无效固件占位地址，表示当前没有与 `board.type` 匹配且版本高于设备当前版本的升级包；这不影响 WebSocket 建连。固件端必须把该地址视为“无需升级”，不得发起下载。

### 3.4 错误响应

该接口部分业务错误仍可能返回 HTTP 200，因此不能只判断 HTTP 状态码。

```json
{"error":"Device ID is required"}
```

```json
{"error":"Invalid device ID"}
```

常见原因：

| 现象 | 原因 | 设备处理 |
|---|---|---|
| `Device ID is required` | 缺少 `Device-Id` | 修正请求头，不要无限重试 |
| `Invalid device ID` | MAC 格式错误 | 修正设备身份，不要无限重试 |
| 无 `websocket` | 后端传输配置异常或返回 MQTT 模式 | 记录错误并停止进入 WS 流程 |
| `websocket.token` 为空 | 鉴权关闭或 Token 生成失败 | 联调环境默认应开启鉴权，联系后端确认 |
| 无 `firmware` | 已绑定但未开启自动升级 | 按“无需升级”处理 |
| 固件 URL 为占位值 | 未绑定、无新版固件或未配置 OTA | 不下载 |

### 3.5 调试请求

只能使用后端分配的测试设备 MAC。不要随意使用生产设备 MAC。

```bash
curl -X POST "https://www.lummiss.com/lummiss/ota/" \
  -H "Content-Type: application/json" \
  -H "Accept: application/json" \
  -H "Device-Id: AA:BB:CC:DD:EE:FF" \
  -H "Client-Id: 550e8400-e29b-41d4-a716-446655440000" \
  -d '{"application":{"name":"desktop-pet","version":"1.0.0"},"board":{"type":"DESKTOP_PET_V1","mac":"AA:BB:CC:DD:EE:FF"}}'
```

OTA 健康检查：

```bash
curl "https://www.lummiss.com/lummiss/ota/"
```

## 4. WebSocket 建连

### 4.1 握手地址与请求头

固件从 OTA 响应读取地址，当前期望值为：

```text
wss://www.lummiss.com/server/lummiss/v1/
```

握手必须携带：

```http
GET /server/lummiss/v1/ HTTP/1.1
Host: www.lummiss.com
Device-Id: AA:BB:CC:DD:EE:FF
Client-Id: 550e8400-e29b-41d4-a716-446655440000
Authorization: Bearer <OTA 返回的 websocket.token>
Upgrade: websocket
Connection: Upgrade
Sec-WebSocket-Key: <由 WebSocket 库生成>
Sec-WebSocket-Version: 13
```

业务代码只需主动设置以下三个头：

| 请求头 | 值 |
|---|---|
| `Device-Id` | 必须与 OTA 请求一致 |
| `Client-Id` | 必须与 OTA 请求一致 |
| `Authorization` | `Bearer`、一个空格、OTA 返回的原始 Token |

`Upgrade`、`Connection`、`Sec-WebSocket-Key`、`Sec-WebSocket-Version` 由 WebSocket 库生成，不要手工拼装。

服务端虽然兼容通过 URL 查询参数传身份和 Token，但正式固件禁止这么做，避免 Token 出现在代理访问日志中。

### 4.2 Token 说明

- Token 由 OTA 服务生成，固件只负责保存和携带；
- Token 与 `Client-Id`、`Device-Id` 绑定；
- Token 是不透明凭证，固件不得解析、修改或依赖其内部格式；
- Token 有效期由服务端控制，固件不得假定固定有效时长；
- 建议每次开机先请求 OTA 获取新 Token；鉴权失败后清除旧 Token 并重新请求 OTA；
- 串口、云日志和崩溃转储不得输出完整 Token。

### 4.3 鉴权失败表现

鉴权失败时，当前服务端可能先发送一条非 JSON UTF-8 文本：

```text
认证失败
```

随后关闭连接。固件应把“握手失败或连接关闭”作为失败依据，清除 Token 后重新走 OTA；不能因为收到非 JSON 文本而复位。

本次联调曾出现以下顺序：

```text
已连接
认证失败
已断开连接
```

原因是 `Authorization` 的值只填写了 Token，缺少 `Bearer ` 前缀。改为以下完整格式后鉴权成功：

```text
Authorization: Bearer <完整 Token>
```

人工调试时还应确认 Token 未被输入框截断或改写。如果仍然认证失败，应重新调用 OTA 获取新 Token，并检查 WSS 的 `Device-Id`、`Client-Id` 是否与生成该 Token 的 OTA 请求完全一致。

### 4.4 Postman/接口调试工具测试步骤

1. 新建 WebSocket 请求，地址填写 `wss://www.lummiss.com/server/lummiss/v1/`；
2. 在握手 Header 中添加 `Device-Id`、`Client-Id` 和 `Authorization`；
3. `Authorization` 填写 `Bearer <OTA 返回的完整 websocket.token>`；
4. 连接成功后，以 Text 文本帧发送第 5 节的客户端 Hello；
5. 收到带 `session_id` 和 `audio_params` 的服务端 Hello，才表示传输、鉴权和应用层协议全部通过。

调试工具自动生成 `Upgrade`、`Connection`、`Sec-WebSocket-Key` 和 `Sec-WebSocket-Version`，不要手工填写。

## 5. WebSocket 应用层 Hello

WSS 建连成功后，设备立即发送文本帧：

```json
{
  "type": "hello",
  "version": 1,
  "transport": "websocket",
  "features": {
    "mcp": true,
    "aec": false,
    "emoji": true
  },
  "capability_manifest": {
    "variantCode": "DESKTOP_PET_V1",
    "manifestVersion": 1,
    "capabilities": [
      {"code": "audio.play", "version": 1},
      {"code": "motion.body_rotate", "version": 1},
      {"code": "motion.precise_rotate", "version": 1},
      {"code": "camera.capture", "version": 1}
    ]
  }
}
```

说明：

- 未实现 MCP 时必须将 `features.mcp` 设为 `false`；
- 未实现回声消除时将 `features.aec` 设为 `false`；
- 没有屏幕或表情能力时将 `features.emoji` 设为 `false`；
- 当前版本不建议在客户端 Hello 中发送 `audio_params`，以免覆盖服务端下行音频声明。

服务端返回文本帧：

```json
{
  "type": "hello",
  "version": 1,
  "transport": "websocket",
  "session_id": "b6a8e2e8-39d9-4695-8a25-28bf41ac32af",
  "audio_params": {
    "format": "opus",
    "sample_rate": 24000,
    "channels": 1,
    "frame_duration": 60
  }
}
```

设备必须保存 `session_id` 用于日志关联，并严格按服务端返回的 `audio_params` 初始化下行 Opus 解码器。

### 5.1 本次 Hello 实测记录

客户端在 2026-09-12 10:46:20 发送：

```json
{
  "type": "hello",
  "version": 1,
  "transport": "websocket",
  "features": {
    "mcp": false,
    "aec": false,
    "emoji": false
  }
}
```

服务端随即返回：

```json
{
  "type": "hello",
  "version": 1,
  "transport": "websocket",
  "audio_params": {
    "format": "opus",
    "sample_rate": 24000,
    "channels": 1,
    "frame_duration": 60
  },
  "session_id": "<服务端动态会话 ID>"
}
```

该结果证明 WSS 地址、证书链、设备身份、Bearer Token 鉴权以及 Hello 消息格式均可正常工作。

## 6. 语音消息流程

### 6.1 上行音频：设备到服务端

当前固定参数：

| 参数 | 值 |
|---|---|
| 编码 | 原始 Opus packet |
| PCM | signed 16-bit little-endian |
| 采样率 | 16000 Hz |
| 声道 | 单声道 |
| 帧长 | 60 ms |
| 每帧 PCM 样本 | 960 |
| 每帧 PCM 字节 | 1920 |

推荐首版使用按键说话 `manual` 模式：

```json
{"type":"listen","state":"start","mode":"manual"}
```

然后连续发送 WebSocket Binary 帧：

```text
一个 WebSocket Binary payload = 一个完整 Opus packet
```

禁止附加 WAV 头、Ogg 容器、JSON、长度、序号或时间戳；禁止合并或拆分 Opus packet。

录音结束后发送：

```json
{"type":"listen","state":"stop"}
```

本地已有文本时可直接发送：

```json
{"type":"listen","state":"detect","text":"你好，请介绍一下自己"}
```

### 6.2 下行音频：服务端到设备

典型顺序：

```json
{"type":"stt","text":"你好，请介绍一下自己","session_id":"..."}
{"type":"tts","state":"start","session_id":"..."}
{"type":"tts","state":"sentence_start","text":"你好，我是 Lummiss。","session_id":"..."}
```

之后收到若干 WebSocket Binary Opus packet，最后收到：

```json
{"type":"tts","state":"stop","session_id":"..."}
```

下行采样率以服务端 Hello 为准，当前默认 24000 Hz、单声道、60 ms。上下行采样率不同，必须分别创建 Opus 编码器和解码器。

### 6.3 打断

设备发送：

```json
{"type":"abort"}
```

设备本地应立即停止扬声器播放、清空待播队列并停止危险动作，不应等待服务端确认。

### 6.4 心跳

应用层心跳格式为：

```json
{"type":"ping"}
```

开启后服务端响应：

```json
{"type":"pong","timestamp":"2026-09-12 09:44:53"}
```

当前联调环境可能忽略应用层 `ping`。底层 WebSocket Ping/Pong 由所用库维护；在双方未明确约定应用层心跳前，固件不能因收不到上述 `pong` 主动判死连接。

## 7. 重连与错误处理

推荐状态机：

```text
NET_DOWN -> OTA_FETCH -> WS_CONNECT -> SEND_HELLO -> ONLINE
    ^                         |                       |
    +------ BACKOFF <---------+-----------------------+
```

规则：

1. 开机、恢复网络和鉴权失败后先调用 OTA；
2. 网络失败使用带随机抖动的指数退避，例如 1、2、4、8、16、30 秒，上限 30 秒；
3. OTA 返回业务错误时区分“参数错误”和“临时网络错误”，参数错误不能无限快速重试；
4. WebSocket 鉴权失败时清除 Token，重新调用 OTA；
5. 普通断网可先使用本次 OTA 返回的地址和 Token 重连，连续失败后重新调用 OTA；
6. 所有 JSON 解析均设置长度和深度上限，未知字段忽略，未知 `type` 限频记录；
7. 断线、`abort`、动作超时必须触发固件本地安全停止。

## 8. 联调准备

- [ ] 联调使用已绑定的测试设备 `Device-Id: E2:67:19:06:41:4E`，并确认允许使用的 `board.type`；
- [ ] 嵌入式方确保 OTA 与 WSS 使用完全相同的 `Device-Id` 和 `Client-Id`；
- [ ] 甲方负责完成设备绑定，并在需要测试升级时准备对应型号的新版本固件；
- [ ] 嵌入式设备已烧录可信根证书或证书链；
- [ ] 双方日志中的 Token、Wi-Fi 密码及其他凭证均已脱敏。

## 9. 嵌入式验收清单

- [ ] Device-Id 和 Client-Id 已持久化，OTA 与 WSS 使用相同值；
- [ ] OTA 能解析已绑定、未绑定、无升级和业务错误四类响应；
- [ ] 固件不下载无效占位 URL；
- [ ] WSS 三个业务请求头完整，Token 未出现在 URL 和日志中；
- [ ] 客户端 Hello 和服务端 Hello 均可正确处理；
- [ ] 上行使用 16 kHz/单声道/60 ms 原始 Opus packet；
- [ ] 下行严格读取服务端 Hello，当前按 24 kHz/单声道/60 ms 解码；
- [ ] `listen.start -> Binary Opus -> listen.stop` 可完成一轮对话；
- [ ] `abort`、断网和动作超时都会本地立即停止；
- [ ] 断网恢复、Token 失效和服务重启均能自动恢复；
- [ ] 连续运行、弱网、重复连接和 OTA 回滚测试通过。
