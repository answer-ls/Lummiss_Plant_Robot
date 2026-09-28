"""在 Windows 上使用 ESP-IDF 配网 CLI，但跳过操作系统 BLE 配对。"""

import os
import runpy
import sys
from pathlib import Path
from types import SimpleNamespace


idf_path = os.environ.get("IDF_PATH")
if not idf_path:
    raise SystemExit("请先设置 IDF_PATH，再运行此脚本。")

prov_dir = Path(idf_path) / "tools" / "esp_prov"
prov_script = prov_dir / "esp_prov.py"
if not prov_script.is_file():
    raise SystemExit(f"找不到 ESP-IDF 配网脚本：{prov_script}")

sys.path.insert(0, str(prov_dir))
from transport import ble_cli  # noqa: E402


# Windows 上 ESP-IDF v5.5.5 的 ble_cli 会强制调用 BleakClient.pair()。
# 本板已验证无需系统配对即可连接并读取配网 GATT 服务；Security 2 认证仍由原 CLI 执行。
# 只修改此进程中 ble_cli 的平台判断，不改 ESP-IDF 安装目录或系统蓝牙状态。
ble_cli.platform = SimpleNamespace(system=lambda: "WindowsWithoutOSPairing")

original_send_data = ble_cli.BLE_Bleak_Client.send_data


async def send_data_with_length(self, characteristic_uuid, data):
    # 仅记录长度和 MTU，不打印 Security 2 密钥、PoP 或 Wi-Fi 凭据。
    payload_len = len(data.encode("latin-1"))
    mtu = self.device.mtu_size
    print(f"BLE 写入: bytes={payload_len}, MTU={mtu}, 单次上限={mtu - 3}")
    return await original_send_data(self, characteristic_uuid, data)


ble_cli.BLE_Bleak_Client.send_data = send_data_with_length
runpy.run_path(str(prov_script), run_name="__main__")
