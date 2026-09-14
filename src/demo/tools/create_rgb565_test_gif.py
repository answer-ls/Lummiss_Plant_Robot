#!/usr/bin/env python3
"""生成红、绿、蓝、白、黑五帧的 320×240 GIF，用于验证 RGB565。"""

from pathlib import Path
import sys

from PIL import Image, ImageDraw


COLORS = (
    ("RED", (255, 0, 0)),
    ("GREEN", (0, 255, 0)),
    ("BLUE", (0, 0, 255)),
    ("WHITE", (255, 255, 255)),
    ("BLACK", (0, 0, 0)),
)


def main() -> None:
    output = Path(sys.argv[1]) if len(sys.argv) > 1 else Path("tools/anim_bin_out/rgb565_test.gif")
    output.parent.mkdir(parents=True, exist_ok=True)

    frames: list[Image.Image] = []
    for label, color in COLORS:
        frame = Image.new("RGB", (320, 240), color)
        draw = ImageDraw.Draw(frame)
        # 中央反色块让纯色画面也能检查方向和完整刷新。
        inverse = tuple(255 - channel for channel in color)
        draw.rectangle((30, 80, 289, 159), fill=inverse)
        draw.text((135, 110), label, fill=color)
        frames.append(frame)

    frames[0].save(
        output,
        save_all=True,
        append_images=frames[1:],
        duration=[1000] * len(frames),
        loop=0,
        disposal=2,
    )
    print(output.resolve())


if __name__ == "__main__":
    main()
