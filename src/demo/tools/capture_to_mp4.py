#!/usr/bin/env python3
"""把已保存的 H.264 裸码流转成带时间戳的 mp4，用于确认“真流”是否流畅（无需 OpenCV 窗口）。

用法：
    python tools/capture_to_mp4.py                     # 默认处理 latest_stream.h264，取同步后 15 秒
    python tools/capture_to_mp4.py camera_xxx.h264 --seconds 20 --fps 20 --burn 0

背景：原始 .h264 没有时间戳，且开头可能是“服务器中途接入”的残缺 GOP——那些 P 帧
依赖之前已过去的 IDR，直接解码会报 non-existing PPS 直到第一个干净关键帧才恢复。
本工具先扫描出第一处完整的 SPS+PPS+IDR 并把前面的残缺前缀丢掉，再把后续每一帧以
固定 --fps 写入带时间戳的 mp4。用 Windows 默认播放器打开即按 20fps 均匀回放；
--burn 1 会把帧号烧进画面，可肉眼核对数字每秒跳 ~fps 次，证明是连续的独立帧。
局限：这验证的是编码内容是否平滑（编码器节奏）；送达抖动请对照服务端接收统计。
"""

from __future__ import annotations

import argparse
import sys
import tempfile
from pathlib import Path

import cv2  # type: ignore[import-not-found]


def next_start_code(data: bytes, pos: int) -> tuple[int, int] | None:
    """从 pos 找下一个 Annex-B 起始码，返回 (起始码偏移, 起始码长度)。"""
    while True:
        idx = data.find(b"\x00\x00\x01", pos)
        if idx < 0:
            return None
        if idx >= 1 and data[idx - 1] == 0:   # 前面还有 00 → 4 字节起始码
            return idx - 1, 4
        return idx, 3


def find_sync_offset(data: bytes) -> int | None:
    """返回第一处连续 SPS(7)+PPS(8)+IDR(5) 中 SPS 的字节偏移，即第一个可独立解码点。"""
    nals: list[tuple[int, int]] = []
    pos = 0
    while True:
        found = next_start_code(data, pos)
        if found is None:
            break
        off, sc = found
        nals.append((off, data[off + sc] & 0x1F))
        pos = off + sc + 1
    for i in range(len(nals) - 2):
        if nals[i][1] == 7 and nals[i + 1][1] == 8 and nals[i + 2][1] == 5:
            return nals[i][0]
    return None


def main() -> None:
    parser = argparse.ArgumentParser(description="H.264 裸码流 → 定时 mp4")
    default_path = Path(__file__).resolve().parent / "camera_captures" / "latest_stream.h264"
    parser.add_argument("path", type=Path, nargs="?", default=default_path,
                        help="H.264 文件路径")
    parser.add_argument("--fps", type=float, default=20.0, help="写入帧率，默认 20")
    parser.add_argument("--seconds", type=int, default=15,
                        help="从首个干净关键帧起取多少秒；0 = 全部")
    parser.add_argument("--burn", type=int, default=1, choices=(0, 1),
                        help="是否把帧号烧进画面便于核对帧率")
    parser.add_argument("--out", type=Path, default=None, help="输出 mp4 路径")
    args = parser.parse_args()

    if hasattr(cv2, "setLogLevel"):
        cv2.setLogLevel(0)

    data = args.path.read_bytes()
    sync = find_sync_offset(data)
    if sync is None:
        print(f"在 {args.path} 里找不到 SPS+PPS+IDR 完整关键帧，无法定位同步点", file=sys.stderr)
        sys.exit(1)

    trimmed = data[sync:]
    tmp = Path(tempfile.gettempdir()) / "lummiss_sync.h264"
    tmp.write_bytes(trimmed)
    print(f"同步点 @ 字节 {sync}：已丢弃其前的残缺 GOP（约 {sync / 1024:.0f} KB）", flush=True)

    capture = cv2.VideoCapture(str(tmp))
    if not capture.isOpened():
        print(f"打不开同步后的码流：{tmp}", file=sys.stderr)
        sys.exit(1)

    out_path = args.out or args.path.with_name(args.path.stem + ".check.mp4")
    target = int(args.fps * args.seconds) if args.seconds > 0 else 0

    ok, first = capture.read()
    if not ok or first is None:
        print("码流没有可解码帧", file=sys.stderr)
        sys.exit(1)

    height, width = first.shape[:2]
    writer = cv2.VideoWriter(str(out_path), cv2.VideoWriter_fourcc(*"mp4v"),
                             args.fps, (width, height))
    if not writer.isOpened():
        print("mp4v 编码器不可用，改用 XVID 输出 .avi", flush=True)
        out_path = out_path.with_suffix(".avi")
        writer = cv2.VideoWriter(str(out_path), cv2.VideoWriter_fourcc(*"XVID"),
                                 args.fps, (width, height))

    written = 0
    while True:
        frame = first if written == 0 else None
        if frame is None:
            ok, frame = capture.read()
            if not ok or frame is None:
                break
        if args.burn:
            cv2.putText(frame, f"{written}", (10, 24),
                        cv2.FONT_HERSHEY_SIMPLEX, 0.8, (0, 255, 0), 2)
        writer.write(frame)
        written += 1
        if target and written >= target:
            break

    capture.release()
    writer.release()
    tmp.unlink(missing_ok=True)
    print(f"完成：写入 {written} 帧 ≈ {written / args.fps:.1f} 秒 → {out_path}", flush=True)
    print("用 Windows 自带播放器 / 浏览器打开即可；有 --burn 水印时可数每秒帧号是否跳 ~"
          f"{args.fps:.0f} 下。", flush=True)


if __name__ == "__main__":
    main()
