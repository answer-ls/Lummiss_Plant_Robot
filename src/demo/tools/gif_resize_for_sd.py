#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""把源 GIF 缩放到屏幕尺寸，生成可直接拷进 TF 卡的 GIF 素材。

为什么必须在 PC 上离线缩放，而不是让固件运行时缩放：

  1. lv_gif 根本不做缩放。lv_gif_set_src() 把 imgdsc.header.w/h 设成 GIF 的
     原始尺寸，lv_img_set_src() 随即把对象尺寸也刷成同样大小。素材比屏幕大时，
     对象被 lv_obj_center 居中，屏幕只显示中间那一块。

  2. gifdec 按原始尺寸分配画布。LV_COLOR_DEPTH == 16 时：
         lv_mem_alloc(sizeof(gd_GIF) + 4 * width * height)
     canvas 占 3 字节/像素，frame 占 1 字节/像素。1280x720 实测约 3.7MB，
     每帧 gd_render_frame 要写 2.76MB，一帧约 490ms —— 只有 2fps。
     缩到 320x180 后像素数变成 1/16，解码开销同比例下降。

  运行时用 lv_img_set_zoom 只能让画面「放得下」，救不了帧率：
  canvas 仍按原始尺寸解码，反而多一层绘制期缩放开销。

用法：
    python tools/gif_resize_for_sd.py [源目录] [输出目录] [--size 320x180]

默认从 项目文档/GIF 读取，输出到 tools/sd_gif_out/，文件名为 exp_01.gif
这类 8.3 短名（避免依赖 FATFS 长文件名，目录名 expressions 仍需长文件名支持）。
"""

import os
import sys

from PIL import Image

# 与 tools/gif2c.py 的 FRAME_W/FRAME_H 保持一致：16:9 内容区，
# 放进横屏 320x240 时上下各留 30px 黑边。
DEFAULT_SIZE = (320, 180)

# GIF 延迟单位是 10ms。写 0 会被解码器当成「未定义」（有的按 100ms 处理），
# 所以给个下限。20ms = 50fps 上限，不会成为瓶颈。
MIN_FRAME_MS = 20


def fit_to_box(frame, box_w, box_h):
    """等比缩放并居中贴到 box_w x box_h 的透明画布上，保证不变形。"""
    scale = min(box_w / frame.width, box_h / frame.height)
    new_w = max(1, round(frame.width * scale))
    new_h = max(1, round(frame.height * scale))

    small = frame.resize((new_w, new_h), Image.LANCZOS)
    canvas = Image.new("RGBA", (box_w, box_h), (0, 0, 0, 0))
    canvas.paste(small, ((box_w - new_w) // 2, (box_h - new_h) // 2), small)
    return canvas


def convert(src_path, dst_path, box_w, box_h):
    """读一个 GIF，逐帧缩放后按原帧延迟重新写出。返回 (帧数, 源尺寸)。"""
    im = Image.open(src_path)
    src_size = im.size

    frames = []
    durations = []
    for i in range(im.n_frames):
        im.seek(i)
        frames.append(fit_to_box(im.convert("RGBA"), box_w, box_h))
        d = int(im.info.get("duration", 0) or 0)
        durations.append(max(d, MIN_FRAME_MS))

    frames[0].save(
        dst_path,
        save_all=True,
        append_images=frames[1:],
        duration=durations,
        loop=0,
        disposal=2,      # 每帧前恢复背景：这批素材每帧都是完整画面
        optimize=False,  # 不做帧差分，避免编码器产出意外结果
    )
    return len(frames), src_size


def parse_size(text):
    try:
        w, h = text.lower().split("x")
        return int(w), int(h)
    except ValueError:
        raise SystemExit(f"--size 需要 WxH 格式（例如 320x180），收到：{text}")


def main():
    args = sys.argv[1:]
    box = DEFAULT_SIZE
    if "--size" in args:
        idx = args.index("--size")
        if idx + 1 >= len(args):
            raise SystemExit("--size 后面要跟 WxH，例如 --size 320x180")
        box = parse_size(args[idx + 1])
        del args[idx:idx + 2]

    here = os.path.dirname(os.path.abspath(__file__))
    src_dir = args[0] if len(args) > 0 else os.path.normpath(
        os.path.join(here, "..", "..", "..", "项目文档", "GIF"))
    out_dir = args[1] if len(args) > 1 else os.path.join(here, "sd_gif_out")

    if not os.path.isdir(src_dir):
        raise SystemExit(f"源目录不存在：{src_dir}")

    os.makedirs(out_dir, exist_ok=True)

    sources = sorted(f for f in os.listdir(src_dir) if f.lower().endswith(".gif"))
    if not sources:
        raise SystemExit(f"{src_dir} 下没有 .gif 文件")

    print(f"源目录  : {src_dir}")
    print(f"输出目录: {out_dir}")
    print(f"目标尺寸: {box[0]}x{box[1]}（等比缩放后居中，不变形）")
    print()
    print(f"{'源文件':<18}{'源尺寸':>11}{'帧数':>6}{'输出':>13}{'大小':>11}")

    total = 0
    for i, name in enumerate(sources, start=1):
        src_path = os.path.join(src_dir, name)
        out_name = f"exp_{i:02d}.gif"   # 8.3 短名，不依赖 FATFS 长文件名
        out_path = os.path.join(out_dir, out_name)

        count, src_size = convert(src_path, out_path, box[0], box[1])
        size = os.path.getsize(out_path)
        total += size

        size_text = f"{src_size[0]}x{src_size[1]}"
        print(f"{name:<18}{size_text:>11}{count:>6}{out_name:>13}{size / 1024:>9.1f}KB")

    print()
    print(f"共 {len(sources)} 个文件，合计 {total / 1024:.1f} KB")
    print(f"把 {out_dir} 里的 exp_*.gif 拷到 TF 卡的 /expressions/ 目录下即可。")


if __name__ == "__main__":
    main()
