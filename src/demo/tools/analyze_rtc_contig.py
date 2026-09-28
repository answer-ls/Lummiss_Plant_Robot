"""分析生命周期日志中的连续内存变化；结果统一保存到项目 logs。"""
import argparse
import json
import re
from datetime import datetime
from pathlib import Path

MIN_REF = 92224
RECORD = re.compile(
    r"MEM_CONTIG\[(?P<tag>[^]]+)\] us=(?P<us>\d+) "
    r"INT=(?P<int_free>\d+)/(?P<int_largest>\d+) "
    r"DMA=(?P<dma_free>\d+)/(?P<dma_largest>\d+) "
    r"PSRAM=(?P<psram_free>\d+)/(?P<psram_largest>\d+)"
)


def analyze(text):
    # 串口多任务输出顺序不一定等于采样顺序，按设备时间排列。
    rows = []
    for match in RECORD.finditer(text):
        rows.append({k: v if k == "tag" else int(v) for k, v in match.groupdict().items()})
    rows.sort(key=lambda row: row["us"])
    crossings, cleanup = [], []
    previous = pending = None
    for row in rows:
        if previous and previous["int_largest"] >= MIN_REF > row["int_largest"]:
            crossings.append({"from": previous, "to": row})
        if row["tag"] == "WAKE_CLEANUP_DELETE_BEFORE":
            pending = row
        elif row["tag"] in ("WAKE_CLEANUP_DELETE_AFTER", "WAKE_CLEANUP_RETAINED_A"):
            if pending:
                cleanup.append({"mode": "B" if row["tag"].endswith("AFTER") else "A",
                                "before": pending, "after": row,
                                "free_delta": row["int_free"] - pending["int_free"],
                                "largest_delta": row["int_largest"] - pending["int_largest"]})
            pending = None
        previous = row
    return {"rows": rows, "crossings": crossings, "cleanup": cleanup,
            "already_below_at_first_sample": bool(rows and rows[0]["int_largest"] < MIN_REF),
            "note": "相邻采样之间的变化不能独自归因于某个分配；free增长不等于largest增长。"}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    args = parser.parse_args()
    result = analyze(args.log.read_text(encoding="utf-8", errors="replace"))
    # 每个文件只分析一次启动；复位后时间归零，应分成独立日志。
    output = Path(__file__).resolve().parents[3] / "logs" / (
        "rtc_contig_analysis_" + datetime.now().strftime("%Y%m%d_%H%M%S_%f") + ".json")
    output.write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8")
    print(f"snapshots={len(result['rows'])} crossings={len(result['crossings'])}")
    for item in result["crossings"]:
        print(f"largest crossed: {item['from']['tag']} -> {item['to']['tag']}")
    for item in result["cleanup"]:
        print(f"cleanup {item['mode']}: free_delta={item['free_delta']} largest_delta={item['largest_delta']}")
    print(output)


if __name__ == "__main__":
    main()
