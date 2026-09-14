"""汇总串口的稳态窗口；跳过设备启动后的前 15 秒，不把累计计数当速率。"""

import argparse
import json
from pathlib import Path
import re
import statistics


PATTERNS = {
    "uvc_fps": r"UVC complete : ([\d.]+) fps",
    "uvc_drop_percent": r"UVC drop : ([\d.]+)%",
    "encoded_fps": r"encoded=([\d.]+) fps",
    "sent_fps": r"sent=([\d.]+) fps",
    "jpeg_ms": r"JPEG_DEC=([\d.]+)",
    "yuv_ms": r"YUV_CONV=([\d.]+)",
    "h264_ms": r"H264_ENC=([\d.]+)",
}


def summarize(path, skip_ms):
    samples = {key: [] for key in PATTERNS}
    counters = {key: [] for key in ("skipped", "empty", "invalid", "send_fail")}
    first_ms = None
    last_ms = None
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        timestamp = re.search(r"\((\d+)\)", line)
        if timestamp is None or int(timestamp[1]) < skip_ms:
            continue
        now_ms = int(timestamp[1])
        first_ms = now_ms if first_ms is None else first_ms
        last_ms = now_ms
        for key, pattern in PATTERNS.items():
            match = re.search(pattern, line)
            if match:
                samples[key].append(float(match[1]))
        for key in counters:
            match = re.search(rf"\b{key}=(\d+)", line)
            if match:
                counters[key].append(int(match[1]))
    return {
        "file": str(path),
        "first_device_ms": first_ms,
        "last_device_ms": last_ms,
        "windows": {
            key: {"count": len(values), "mean": round(statistics.mean(values), 3),
                  "min": min(values), "max": max(values)}
            for key, values in samples.items() if values
        },
        "counters": {
            key: {"first": values[0], "last": values[-1],
                  "delta": values[-1] - values[0]}
            for key, values in counters.items() if values
        },
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("logs", nargs="+", type=Path)
    parser.add_argument("--skip-ms", type=int, default=15000)
    args = parser.parse_args()
    print(json.dumps([summarize(path, args.skip_ms) for path in args.logs],
                     ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
