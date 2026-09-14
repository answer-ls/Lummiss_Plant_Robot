#!/usr/bin/env python3
"""校验 LUM1 动画 Header、Frame Table、偏移、大小和 RGB565 测试颜色。"""

from __future__ import annotations

import argparse
from pathlib import Path
import struct
import sys


HEADER = struct.Struct("<4sHHHHBBH")
FRAME = struct.Struct("<IIHH")
MAGIC = b"LUM1"
VERSION = 1
PIXEL_FORMAT_RGB565 = 1


def validate(path: Path, expected_byte_order: str) -> dict[str, int]:
    int_byte_order = "big" if expected_byte_order == "be" else "little"
    file_size = path.stat().st_size
    if file_size < HEADER.size:
        raise ValueError("文件小于 LUM1 Header")

    with path.open("rb") as stream:
        raw_header = stream.read(HEADER.size)
        magic, version, width, height, frame_count, pixel_format, flags, loop_count = HEADER.unpack(raw_header)

        if magic != MAGIC:
            raise ValueError(f"magic 错误: {magic!r}")
        if version != VERSION:
            raise ValueError(f"version 错误: {version}")
        if width == 0 or height == 0 or width > 320 or height > 240:
            raise ValueError(f"尺寸非法: {width}x{height}")
        if frame_count == 0 or frame_count > 1024:
            raise ValueError(f"frame_count 非法: {frame_count}")
        if pixel_format != PIXEL_FORMAT_RGB565:
            raise ValueError(f"pixel_format 错误: {pixel_format}")
        if flags != 0:
            raise ValueError(f"flags 错误: {flags}")

        table_end = HEADER.size + frame_count * FRAME.size
        if table_end > file_size:
            raise ValueError("Frame Table 超出文件")

        expected_frame_size = width * height * 2
        previous_end = table_end
        total_duration_ms = 0
        first_pixels: list[int] = []

        for index in range(frame_count):
            raw_frame = stream.read(FRAME.size)
            if len(raw_frame) != FRAME.size:
                raise ValueError(f"第 {index} 帧描述读取不足")
            offset, size, delay_ms, reserved = FRAME.unpack(raw_frame)
            if reserved != 0:
                raise ValueError(f"第 {index} 帧 reserved 非 0")
            if size != expected_frame_size:
                raise ValueError(
                    f"第 {index} 帧大小错误: {size}, 期望 {expected_frame_size}"
                )
            if offset < table_end or offset != previous_end:
                raise ValueError(f"第 {index} 帧 offset 不连续或指向表内: {offset}")
            if offset + size > file_size:
                raise ValueError(f"第 {index} 帧数据越界")
            previous_end = offset + size
            total_duration_ms += delay_ms

            current = stream.tell()
            stream.seek(offset)
            pixel_bytes = stream.read(2)
            if len(pixel_bytes) != 2:
                raise ValueError(f"第 {index} 帧首像素读取不足")
            first_pixels.append(int.from_bytes(pixel_bytes, int_byte_order))
            stream.seek(current)

        if previous_end != file_size:
            raise ValueError(f"末帧结束位置 {previous_end} 与文件大小 {file_size} 不一致")

    return {
        "width": width,
        "height": height,
        "frame_count": frame_count,
        "loop_count": loop_count,
        "file_size": file_size,
        "frame_size": expected_frame_size,
        "total_duration_ms": total_duration_ms,
        "flags": flags,
        "first_pixels": first_pixels,
    }


def main() -> None:
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
        sys.stderr.reconfigure(encoding="utf-8", errors="replace")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path)
    parser.add_argument("--byte-order", choices=("le", "be"), default="be")
    parser.add_argument(
        "--expect-colors",
        help="可选，逗号分隔的 RGB565 首像素，例如 F800,07E0,001F,FFFF,0000",
    )
    args = parser.parse_args()

    try:
        result = validate(args.input, args.byte_order)
        if args.expect_colors:
            expected = [int(value, 16) for value in args.expect_colors.split(",")]
            actual = result["first_pixels"]
            if actual != expected:
                raise ValueError(
                    "测试颜色错误: "
                    f"actual={[f'{value:04X}' for value in actual]}, "
                    f"expected={[f'{value:04X}' for value in expected]}"
                )
    except (OSError, ValueError) as exc:
        print(f"校验失败: {exc}", file=sys.stderr)
        raise SystemExit(1) from exc

    print("LUM1 校验通过")
    print(f"尺寸: {result['width']}x{result['height']}")
    print(f"帧数: {result['frame_count']}")
    print(f"帧大小: {result['frame_size']} bytes")
    print(f"文件大小: {result['file_size']} bytes")
    print(f"动画总时长: {result['total_duration_ms']} ms")
    print(f"Loop: {result['loop_count']}")
    print(f"帧数据解释字节序: RGB565-{args.byte_order.upper()}")


if __name__ == "__main__":
    main()
