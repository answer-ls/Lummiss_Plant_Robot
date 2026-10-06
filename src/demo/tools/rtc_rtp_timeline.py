"""导出浏览器 RTP 累计统计和 P4 ESP-Hosted 计数，供服务端逐包日志对齐。"""

import argparse
import csv
import gzip
import json
import re
from datetime import datetime
from pathlib import Path


METRICS = (
    "packetsReceived", "packetsLost", "framesReceived", "framesDecoded",
    "freezeCount", "totalFreezesDuration", "nackCount", "pliCount",
    "keyFramesDecoded",
)
HOST_RE = re.compile(
    r"I \((\d+)\).*?flowctrl_drop\[(\d+)\].*?out\(ok\[(\d+)\] drop\[(\d+)\]"
)


def read_browser(path):
    with gzip.open(path, "rt", encoding="utf-8") as stream:
        data = json.load(stream)
    rows = []
    for pc_id, connection in data["PeerConnections"].items():
        groups = {}
        for key, item in connection.get("stats", {}).items():
            prefix, sep, metric = key.rpartition("-")
            if not sep or metric not in METRICS or item["statsType"] != "inbound-rtp":
                continue
            groups.setdefault(prefix, {})[metric] = item
        for track_id, group in groups.items():
            if "packetsLost" not in group:
                continue
            anchor = group["packetsLost"]
            start = datetime.fromisoformat(anchor["startTime"].replace("Z", "+00:00"))
            end = datetime.fromisoformat(anchor["endTime"].replace("Z", "+00:00"))
            values = {key: json.loads(value["values"]) for key, value in group.items()}
            count = len(values["packetsLost"])
            for i in range(count):
                # getStats 转储只保留起止时间；中间采样时间是等间隔估算值。
                instant = start + (end - start) * i / max(count - 1, 1)
                row = {"pc_id": pc_id, "track_id": track_id,
                       "sample": i, "approx_utc": instant.isoformat()}
                row.update({name: series[i] if i < len(series) else ""
                            for name, series in values.items()})
                rows.append(row)
    return rows


def read_p4(path):
    rows = []
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        match = HOST_RE.search(line)
        if match:
            uptime_ms, flow_drop, out_ok, out_drop = map(int, match.groups())
            rows.append({"uptime_ms": uptime_ms, "flowctrl_drop": flow_drop,
                         "sdio_out_ok": out_ok, "sdio_out_drop": out_drop})
    return rows


def write_csv(path, rows):
    if not rows:
        return
    fields = list(dict.fromkeys(key for row in rows for key in row))
    with path.open("w", newline="", encoding="utf-8-sig") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--browser", type=Path, required=True)
    parser.add_argument("--p4-log", type=Path)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    browser_rows = read_browser(args.browser)
    write_csv(args.out / "browser_inbound.csv", browser_rows)
    if args.p4_log:
        p4_rows = read_p4(args.p4_log)
        write_csv(args.out / "p4_hosted.csv", p4_rows)
    print(f"浏览器采样={len(browser_rows)}；中间时间仅为估算，转储没有逐包RTP序号。")


if __name__ == "__main__":
    main()
