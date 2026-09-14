#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import argparse
import os
import struct
import sys
from PIL import Image


# ============================================================
# Lummiss Animation BIN 格式
#
# Header: 16 bytes
# ------------------------------------------------------------
# magic        4 bytes   b"LUM1"
# version      uint16    当前 = 1
# width        uint16
# height       uint16
# frame_count  uint16
# pixel_format uint8     1 = RGB565
# flags        uint8     当前保留，写 0
# loop_count   uint16    0 = 无限循环
#
# 本项目约定帧像素采用 RGB565-BE；Header 和 Frame Table 固定为小端。
#
# Frame Table: 每帧 12 bytes
# ------------------------------------------------------------
# offset       uint32    当前帧数据在文件中的绝对偏移
# size         uint32    当前帧字节数
# delay_ms     uint16    当前帧显示时间
# reserved     uint16
#
# 后面连续存放所有 RGB565 帧数据
# ============================================================

MAGIC = b"LUM1"
VERSION = 1

PIXEL_FORMAT_RGB565 = 1

HEADER_FMT = "<4sHHHHBBH"
FRAME_INFO_FMT = "<IIHH"

HEADER_SIZE = struct.calcsize(HEADER_FMT)
FRAME_INFO_SIZE = struct.calcsize(FRAME_INFO_FMT)
UINT16_MAX = 0xFFFF


def rgb888_to_rgb565_bytes(img: Image.Image, byte_order="be"):
    """
    RGB888 -> RGB565

    byte_order:
        be: 高字节在前，匹配本项目 LV_COLOR_16_SWAP=y
        le: 低字节在前，供其他显示链路选用
    """
    img = img.convert("RGB")

    pixels = img.load()
    width, height = img.size

    out = bytearray(width * height * 2)

    pos = 0

    for y in range(height):
        for x in range(width):
            r, g, b = pixels[x, y]

            r5 = r >> 3
            g6 = g >> 2
            b5 = b >> 3

            rgb565 = (r5 << 11) | (g6 << 5) | b5

            if byte_order == "le":
                out[pos] = rgb565 & 0xFF
                out[pos + 1] = (rgb565 >> 8) & 0xFF
            else:
                out[pos] = (rgb565 >> 8) & 0xFF
                out[pos + 1] = rgb565 & 0xFF

            pos += 2

    return bytes(out)


def resize_frame(frame: Image.Image,
                 target_width: int,
                 target_height: int,
                 mode="contain",
                 bg_color=(0, 0, 0)):
    """
    mode:
        stretch : 强制拉伸
        contain : 等比例缩放，剩余区域填背景
        cover   : 等比例铺满，多余部分裁剪
    """

    frame = frame.convert("RGBA")

    if mode == "stretch":
        frame = frame.resize(
            (target_width, target_height),
            Image.Resampling.LANCZOS
        )

        bg = Image.new(
            "RGBA",
            (target_width, target_height),
            (*bg_color, 255)
        )

        bg.alpha_composite(frame)
        return bg.convert("RGB")

    src_w, src_h = frame.size

    if mode == "contain":
        scale = min(
            target_width / src_w,
            target_height / src_h
        )

    elif mode == "cover":
        scale = max(
            target_width / src_w,
            target_height / src_h
        )

    else:
        raise ValueError(f"Unknown resize mode: {mode}")

    new_w = max(1, round(src_w * scale))
    new_h = max(1, round(src_h * scale))

    resized = frame.resize(
        (new_w, new_h),
        Image.Resampling.LANCZOS
    )

    canvas = Image.new(
        "RGBA",
        (target_width, target_height),
        (*bg_color, 255)
    )

    x = (target_width - new_w) // 2
    y = (target_height - new_h) // 2

    canvas.alpha_composite(resized, (x, y))

    if mode == "cover":
        canvas = canvas.crop(
            (
                0,
                0,
                target_width,
                target_height
            )
        )

    return canvas.convert("RGB")


def read_gif_frames(gif_path,
                    width,
                    height,
                    resize_mode,
                    bg_color,
                    min_delay_ms):
    """
    顺序 seek GIF。
    Pillow 在顺序 seek 时会处理 GIF 的帧叠加/disposal，
    convert("RGBA") 得到当前逻辑帧。
    """

    gif = Image.open(gif_path)

    original_size = gif.size
    frame_count = getattr(gif, "n_frames", 1)
    if frame_count <= 0 or frame_count > UINT16_MAX:
        raise ValueError(f"GIF 帧数超出 LUM1 范围: {frame_count}")

    loop_count = gif.info.get("loop", 0)

    frames = []

    print(f"GIF: {gif_path}")
    print(f"原始尺寸: {original_size[0]}x{original_size[1]}")
    print(f"GIF帧数: {frame_count}")
    print(f"Loop: {loop_count}")

    for i in range(frame_count):
        gif.seek(i)

        # 当前 GIF 帧延迟，单位 ms
        delay = gif.info.get("duration", 100)

        delay = max(delay, min_delay_ms)
        if delay > UINT16_MAX:
            raise ValueError(f"第 {i} 帧 delay 超出 uint16: {delay} ms")

        # seek + convert 得到这一时刻的完整显示画面
        frame = gif.convert("RGBA").copy()

        frame = resize_frame(
            frame,
            width,
            height,
            resize_mode,
            bg_color
        )

        frames.append(
            {
                "image": frame,
                "delay": delay
            }
        )

        print(
            f"\r处理 GIF 帧: {i + 1}/{frame_count}",
            end="",
            flush=True
        )

    print()

    return frames, loop_count, original_size


def convert_gif_to_bin(args):

    if not 1 <= args.width <= UINT16_MAX or not 1 <= args.height <= UINT16_MAX:
        raise ValueError("width/height 必须在 1..65535")

    frames, loop_count, original_size = read_gif_frames(
        args.input,
        args.width,
        args.height,
        args.resize,
        args.background,
        args.min_delay
    )

    encoded_frames = []

    print("转换 RGB565...")

    for i, frame_info in enumerate(frames):

        raw = rgb888_to_rgb565_bytes(
            frame_info["image"],
            args.byte_order
        )

        encoded_frames.append(
            {
                "data": raw,
                "delay": frame_info["delay"]
            }
        )

        print(
            f"\rRGB565: {i + 1}/{len(frames)}",
            end="",
            flush=True
        )

    print()

    frame_count = len(encoded_frames)
    if not 0 <= loop_count <= UINT16_MAX:
        raise ValueError(f"GIF loop 超出 uint16: {loop_count}")

    # Header 后面紧跟 Frame Table
    data_start_offset = (
        HEADER_SIZE +
        frame_count * FRAME_INFO_SIZE
    )

    frame_table = []

    current_offset = data_start_offset

    for frame in encoded_frames:

        frame_size = len(frame["data"])

        frame_table.append(
            (
                current_offset,
                frame_size,
                frame["delay"],
                0
            )
        )

        current_offset += frame_size

    os.makedirs(
        os.path.dirname(os.path.abspath(args.output)),
        exist_ok=True
    )

    with open(args.output, "wb") as f:

        # ---------- Header ----------
        header = struct.pack(
            HEADER_FMT,

            MAGIC,
            VERSION,

            args.width,
            args.height,

            frame_count,

            PIXEL_FORMAT_RGB565,

            0,  # flags

            loop_count
        )

        f.write(header)

        # ---------- Frame Table ----------
        for info in frame_table:
            f.write(
                struct.pack(
                    FRAME_INFO_FMT,
                    *info
                )
            )

        # ---------- Frame Data ----------
        for frame in encoded_frames:
            f.write(frame["data"])

    file_size = os.path.getsize(args.output)

    print()
    print("转换完成")
    print("--------------------------------")
    print(f"输出文件 : {args.output}")
    total_duration_ms = sum(frame["delay"] for frame in encoded_frames)
    average_frame_size = sum(len(frame["data"]) for frame in encoded_frames) / frame_count
    print(f"原始尺寸 : {original_size[0]}x{original_size[1]}")
    print(f"目标尺寸 : {args.width}x{args.height}")
    print(f"帧数     : {frame_count}")
    print(f"像素格式 : RGB565-{args.byte_order.upper()}")
    print(f"Loop     : {loop_count}")
    print(f"BIN大小  : {file_size} bytes")
    print(f"平均帧大小: {average_frame_size:.0f} bytes")
    print(f"动画总时长: {total_duration_ms} ms")
    print("--------------------------------")


def parse_color(text):
    """
    支持：
        000000
        #000000
        FF0000
    """

    text = text.strip().lstrip("#")

    if len(text) != 6:
        raise argparse.ArgumentTypeError(
            "背景色必须是 RRGGBB，例如 000000"
        )

    try:
        value = int(text, 16)
    except ValueError:
        raise argparse.ArgumentTypeError(
            "背景色格式错误"
        )

    r = (value >> 16) & 0xFF
    g = (value >> 8) & 0xFF
    b = value & 0xFF

    return r, g, b


def main():

    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
        sys.stderr.reconfigure(encoding="utf-8", errors="replace")

    parser = argparse.ArgumentParser(
        description="GIF -> Lummiss RGB565 Animation BIN"
    )

    parser.add_argument(
        "input",
        help="输入 GIF"
    )

    parser.add_argument(
        "output",
        help="输出 BIN"
    )

    parser.add_argument(
        "--width",
        type=int,
        default=320,
        help="输出宽度，默认 320"
    )

    parser.add_argument(
        "--height",
        type=int,
        default=240,
        help="输出高度，默认 240"
    )

    parser.add_argument(
        "--resize",
        choices=[
            "stretch",
            "contain",
            "cover"
        ],
        default="contain",
        help="缩放方式"
    )

    parser.add_argument(
        "--background",
        type=parse_color,
        default=(0, 0, 0),
        help="透明区域背景色，如 000000"
    )

    parser.add_argument(
        "--byte-order",
        choices=["le", "be"],
        default="be",
        help="RGB565帧数据字节序；本项目 LV_COLOR_16_SWAP=y，默认 be"
    )

    parser.add_argument(
        "--min-delay",
        type=int,
        default=0,
        help="可选的最小帧间隔 ms，默认 0 表示保留 GIF 原始 delay"
    )

    args = parser.parse_args()

    try:
        convert_gif_to_bin(args)
    except (OSError, ValueError, Image.UnidentifiedImageError) as exc:
        print(f"转换失败: {exc}", file=sys.stderr)
        raise SystemExit(1) from exc


if __name__ == "__main__":
    main()
