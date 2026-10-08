# Lummiss App BLE + WiFi 配网协议

固件采用 Espressif 官方 Unified Provisioning 协议。ESP32-P4 运行 NimBLE Host 和配网管理器，板载 ESP32-C6 通过 ESP-Hosted/SDIO 提供 BLE Controller 与 2.4 GHz WiFi。

## 固件行为

- C6 没有已保存的 WiFi 凭据时，启动 BLE 配网并暂停 OTA、UI、摄像头和视频任务，避免配网阶段与现有 USB/编解码链路争用资源。
- C6 已保存凭据时，跳过 BLE 配网并直接连接路由器。
- App 下发的凭据验证成功后，固件先向 App 返回成功状态；约 5 秒后停止 BLE 广播并断开配网连接，再启动其余业务。
- WiFi 断线后每 2 秒自动重连。清除凭据并重启后会再次进入 BLE 配网。

## App 使用的固定参数

| 参数 | 值 |
| --- | --- |
| BLE 名称 | `LUMMISS_XXXXXX`，后 6 位来自设备 MAC |
| 自定义 Service UUID | `021a9004-0382-4aea-bff4-6b3f1c5adfb4` |
| Transport | `ble` |
| Network | `wifi` |
| Security | Security 2（SRP6a + AES-GCM） |
| Username | `lummiss` |
| PoP | 每台设备独立的 8 位十六进制字符串 |
| WiFi 频段 | 2.4 GHz |

二维码内容遵循 Espressif Provisioning App 的 JSON 格式：

```json
{
  "ver": "v1",
  "name": "LUMMISS_A1B2C3",
  "username": "lummiss",
  "pop": "89ABCDEF",
  "transport": "ble",
  "network": "wifi"
}
```

上面的设备名和 PoP 只是格式示例。开发阶段，固件首次启动会生成 PoP，保存在 P4 NVS 的 `lummiss/prov_pop`，并在串口输出本机二维码 JSON。量产时应把每台设备的 PoP 写入制造数据、打印在机身二维码或保存到后端，App 不应使用统一口令。

## App 配网流程

1. App 扫描二维码或从后端取得设备名、用户名和 PoP。
2. 扫描 BLE，按完整设备名及 Service UUID 匹配设备并建立连接。
3. 使用 Security 2、用户名 `lummiss` 和该设备 PoP 建立安全会话。
4. 调用 WiFi 扫描接口，向用户展示 2.4 GHz AP；也可直接输入 SSID。
5. 向设备发送 SSID 和密码，执行 apply/provision。
6. 轮询配网状态。收到 WiFi connected/success 后显示成功，并允许设备自行断开 BLE。
7. App 转入局域网设备发现或后端绑定流程。设备此时开始 OTA、UI、摄像头和视频业务。

App 不应把 BLE 断开本身当作失败。配网成功后固件会主动停止广播并断开连接；应以协议返回的 connected/success 为最终判据。

## App SDK

- Android：`com.github.espressif:esp-idf-provisioning-android`，使用 BLE transport 和 Security 2。
- iOS：Espressif `esp-idf-provisioning-ios`，同样选择 BLE 和 Security 2。
- 调试工具：ESP-IDF `esp_prov.py` 可验证完整协议。Windows 上官方工具的系统配对步骤可能因蓝牙驱动返回错误，但不代表设备端 GATT 或配网协议失败；手机 App 仍应按 SDK 标准流程实现。

## 重新配网入口

固件已经提供 `network_manager_forget_wifi_credentials()`。后续可把它接到实体按键长按或 App 的“清除网络”命令；调用成功后执行 `esp_restart()`，设备下次启动会广播 BLE。此操作会删除 C6 Flash 中保存的 WiFi 配置。

开发验证时可以在 menuconfig 中临时启用 `CONFIG_LUMMISS_FORCE_BLE_PROVISIONING`，在不删除现有 WiFi 凭据的情况下强制广播。正常固件必须关闭该开关。

## 当前硬件版本注意事项

当前板载 C6 固件报告 ESP-Hosted 2.3.x，而 P4 Host 组件为 2.7.4，因此启动时会有版本不一致警告。BLE 配网、WiFi 连接以及成功后停止广播已经在当前组合上实测通过。当前配置在配网结束后保留空闲 BLE 协议栈，避免跨版本远端 HCI teardown；升级 C6 固件后需要重新验证启动、配网、断线重连和 BLE 释放路径。
