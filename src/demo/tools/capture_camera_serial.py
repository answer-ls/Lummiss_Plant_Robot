"""有界采集摄像头串口日志；不发送命令、不主动复位开发板。"""

import argparse
from pathlib import Path
import sys
import time

import serial


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="COM17")
    parser.add_argument("--seconds", type=float, default=60)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.seconds <= 0:
        parser.error("--seconds 必须大于 0")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    # 在打开串口前指定控制线状态，避免用监视器的默认复位流程干扰对照。
    port = serial.Serial(port=None, baudrate=115200, timeout=0.5)
    port.dtr = False
    port.rts = False
    port.port = args.port
    port.open()
    deadline = time.monotonic() + args.seconds
    try:
        with args.output.open("wb") as log:
            while time.monotonic() < deadline:
                line = port.readline()
                if line:
                    log.write(line)
                    log.flush()
                    print(line.decode("utf-8", errors="replace"), end="", flush=True)
    finally:
        port.close()
    print(f"\n日志保存：{args.output.resolve()}")


if __name__ == "__main__":
    main()
