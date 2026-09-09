#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""把 项目文档/GIF 下的 8 个情绪 GIF 转换为嵌入式 C 资源。

转换思路（与固件端约定一致）：
  - 每个 GIF 逐帧取出（Pillow 实测这些 GIF 每帧都是全画幅，可直接用）。
  - 用 LANCZOS 缩放到 320x180（16:9 等比，正好是横屏 320x240 的显示内容区）。
  - 把该情绪所有帧拼成竖条，整体量化成共享 256 色调色板，避免换帧时调色板漂移导致闪烁。
  - 每帧得到一个 320*180 的 8bit 索引流；再做游程压缩(RLE)：
      反复出现 {run_len(1..255), color_idx}，帧结束标记为 {0, 0}。
  - 调色板直接输出为 RGB565 的“面板字节序”（高位字节在前，与 LVGL
    LV_COLOR_16_SWAP=1 下的 lv_color16_t 内存布局一致），固件端按 2 字节整字拷贝即可。

用法：python tools/gif2c.py [GIF目录] [输出头] [输出C] [--dither]
默认输入目录 ../项目文档/GIF，输出到 main/gif_assets.h 与 main/gif_assets.c。
生成的文件带“自动生成，勿手改”头部，可提交入库；不依赖构建期 Python。
"""

import os
import sys

from PIL import Image

# 轮播顺序与文件名的对应关系（文件名含中文，直接以 Unicode 写在本脚本内）
EMOTIONS = [
    ("眨眼",     "眨眼.gif"),
    ("喜",       "喜.gif"),
    ("怒",       "怒.gif"),
    ("哀",       "哀.gif"),
    ("乐",       "乐.gif"),
    ("思考",     "思考.gif"),
    ("惊讶",     "惊讶.gif"),
    ("疑惑",     "疑惑_.gif"),
]

# 情绪 → ASCII 小写助记，用于 C 标识符（避免非 ASCII 标识符兼容性问题）
VAR_SUFFIX = {"眨眼": "blink", "喜": "xi", "怒": "nu", "哀": "ai", "乐": "le",
              "思考": "think", "惊讶": "surprise", "疑惑": "confused"}

# 显示目标尺寸：16:9 内容区，置于横屏 320x240 中上下各留 30px 黑边
FRAME_W = 320
FRAME_H = 180
PALETTE_SIZE = 256
MIN_FRAME_MS = 30   # 仅用于打印提示，实际时长以素材为准


def rgb565_panel_bytes(r, g, b):
    """经典 RGB565 大端字节序（高位字节在前），返回 (hi, lo) 两个字节。

    即 LVGL LV_COLOR_16_SWAP=1 时 lv_color16_t 的内存布局，可直接写面板。
    """
    val = ((r >> 3) << 11) | ((g >> 2) << 6) | (b >> 3)
    return ((val >> 8) & 0xFF), (val & 0xFF)


def rle_encode(indices):
    """把一整帧的 8bit 索引流编码为游程字节流，并以 (0,0) 结束。

    返回 bytes。运行长超过 255 时拆成多个 token。
    """
    out = bytearray()
    run_val = -1
    run_len = 0
    for v in indices:
        if v == run_val and run_len < 255:
            run_len += 1
        else:
            if run_len > 0:
                out.append(run_len)
                out.append(run_val)
            run_val = v
            run_len = 1
    if run_len > 0:
        out.append(run_len)
        out.append(run_val)
    out.append(0)
    out.append(0)
    return bytes(out)


def load_gif_frames(path):
    """读取一个 GIF 的所有帧（Pillow 帧已按原图给出），转 RGB 返回列表。"""
    im = Image.open(path)
    frames = []
    for i in range(im.n_frames):
        im.seek(i)
        frames.append(im.convert("RGB"))
    return frames


def quantize_shared(frames):
    """用“整组帧拼贴量化 256 色”得到共享调色板，再逐帧量化。

    返回 (palette_rgb[256], indexed_frames[bytes])。
    palette_rgb 固定 256 项，不足部分补 0（避免部分 GIF 用色少于 256）。
    """
    strip = Image.new("RGB", (FRAME_W, FRAME_H * len(frames)))
    for j, f in enumerate(frames):
        strip.paste(f, (0, j * FRAME_H))
    pal_img = strip.quantize(colors=PALETTE_SIZE, dither=Image.Dither.NONE)

    # Pillow 的 P 模式调色板是每 3 字节一色（R,G,B）
    raw = pal_img.getpalette() or []
    palette = []
    for i in range(PALETTE_SIZE):
        base = i * 3
        palette.append(tuple(raw[base:base + 3]) if base + 3 <= len(raw) else (0, 0, 0))

    # 用同一调色板量化每一帧，保证同一颜色在不同帧里索引一致、不闪烁
    indexed = [f.quantize(palette=pal_img, dither=Image.Dither.NONE).tobytes() for f in frames]
    return palette, indexed


def c_bytes_array(name, data, per_line=16):
    """把 bytes 输出成 C 数组文本。"""
    lines = [f"static const uint8_t {name}[] = {{"]
    for i in range(0, len(data), per_line):
        chunk = data[i:i + per_line]
        lines.append("    " + ", ".join(f"0x{b:02X}" for b in chunk) + ",")
    lines.append("};")
    return "\n".join(lines)


def build_one(emo_label, file_name, gif_dir, dither):
    """处理单个情绪，返回生成的 C 文本片段与体积信息。"""
    path = os.path.join(gif_dir, file_name)
    frames = load_gif_frames(path)
    small = [f.resize((FRAME_W, FRAME_H), Image.LANCZOS) for f in frames]

    # 逐个取原始帧延迟（Pillow 单位已是毫秒）
    im = Image.open(path)
    durations = []
    for i in range(im.n_frames):
        im.seek(i)
        d = int(im.info.get("duration", 0) or 0)
        durations.append(max(d, 10))

    palette, indexed = quantize_shared(small)
    rle_frames = [rle_encode(idx) for idx in indexed]

    # 累计偏移表：frame_offset[0..n]，最后一项为总长
    offsets = [0]
    for r in rle_frames:
        offsets.append(offsets[-1] + len(r))
    rle_all = b"".join(rle_frames)

    # 调色板按 RGB565 面板字节序输出（每色 2 字节，共 512 字节）
    pal_bytes = bytearray()
    for (r, g, b) in palette:
        hi, lo = rgb565_panel_bytes(r, g, b)
        pal_bytes.append(hi)
        pal_bytes.append(lo)

    # 标识符一律用 ASCII 助记，避免非 ASCII 标识符兼容性问题
    ident = "gif_" + VAR_SUFFIX[emo_label]

    pal_name = ident + "_palette"
    dur_name = ident + "_duration"
    off_name = ident + "_offset"
    rle_name = ident + "_rle"

    part = []
    part.append(f"/* ===== {emo_label}（{file_name}，{len(frames)} 帧） ===== */")
    part.append(c_bytes_array(pal_name, bytes(pal_bytes)))
    dur_c = ", ".join(f"{d}" for d in durations)
    part.append(f"static const uint16_t {dur_name}[{len(durations)}] = {{ {dur_c} }};")
    off_c = ", ".join(f"0x{o:X}" for o in offsets)
    part.append(f"static const uint32_t {off_name}[{len(offsets)}] = {{ {off_c} }};")
    part.append(c_bytes_array(rle_name, rle_all))
    part.append("")
    txt = "\n".join(part)

    total = len(pal_bytes) + len(dur_c) + 4 * len(offsets) + len(rle_all)
    info = (emo_label, len(frames), len(rle_all), sum(len(r) for r in rle_frames) + len(pal_bytes))
    return txt, info


def main():
    args = [a for a in sys.argv[1:]]
    dither = "--dither" in args
    args = [a for a in args if a != "--dither"]

    gif_dir = args[0] if len(args) > 0 else os.path.normpath(
        os.path.join(os.path.dirname(__file__), "..", "..", "..", "项目文档", "GIF"))
    out_h = args[1] if len(args) > 1 else os.path.normpath(
        os.path.join(os.path.dirname(__file__), "..", "main", "gif_assets.h"))
    out_c = args[2] if len(args) > 2 else os.path.normpath(
        os.path.join(os.path.dirname(__file__), "..", "main", "gif_assets.c"))

    print(f"输入目录: {gif_dir}\ndither={dither}  目标: {FRAME_W}x{FRAME_H}")

    bodies = []
    infos = []
    ids = []
    for idx, (emo_label, file_name) in enumerate(EMOTIONS):
        if not os.path.exists(os.path.join(gif_dir, file_name)):
            raise FileNotFoundError(f"缺少情绪文件: {file_name}")
        body, info = build_one(emo_label, file_name, gif_dir, dither)
        bodies.append(body)
        infos.append(info)
        ids.append((emo_label, "LUMMISS_GIF_" + VAR_SUFFIX[emo_label].upper(), idx))

    # 生成 .h
    guard = "LUMMISS_GIF_ASSETS_H"
    h_lines = [
        "/* 本文件由 tools/gif2c.py 自动生成，请勿手工修改。 */",
        "/* 屏幕 GIF 表情资源：8 个情绪，按本文件顺序轮播。 */",
        "#ifndef " + guard,
        "#define " + guard,
        "",
        '#include "stdint.h"',
        "",
        "/* 动画内容区尺寸：16:9，置于横屏 320x240 时上下各留 30px 黑边 */",
        "#define GIF_FRAME_W 320",
        "#define GIF_FRAME_H 180",
        "",
        "/* 情绪 ID：顺序即屏幕轮播顺序（与 tools/gif2c.py 中 EMOTIONS 一致） */",
        "typedef enum {",
    ]
    for emo_label, ident, idx in ids:
        h_lines.append(f"    {ident} = {idx},    /* {emo_label} */")
    h_lines.append("    LUMMISS_GIF_COUNT")
    h_lines.append("} lummiss_gif_id_t;")
    h_lines += [
        "",
        "/* 单个情绪的资源描述：",
        " *   palette    256 色 x 2B（RGB565 高位字节在前，即 lv_color16_t 布局）",
        " *   duration_ms  每帧停留毫秒数",
        " *   frame_offset 各帧在 rle_data 中的累计偏移，长度 frame_count+1",
        " *   rle_data     逐帧游程数据，每帧以 {0,0} 结束",
        " */",
        "typedef struct {",
        "    const char *name;",
        "    uint8_t     frame_count;",
        "    const uint8_t  *palette;",
        "    const uint16_t *duration_ms;",
        "    const uint32_t *frame_offset;",
        "    const uint8_t  *rle_data;",
        "} lummiss_gif_asset_t;",
        "",
        "/* 资源表：gif_assets[id] 与上方的情绪 ID 一一对应 */",
        "extern const lummiss_gif_asset_t gif_assets[LUMMISS_GIF_COUNT];",
        "",
        "#endif /* " + guard + " */",
        "",
    ]
    with open(out_h, "w", encoding="utf-8") as f:
        f.write("\n".join(h_lines))

    # 生成 .c：先输出全部静态资源数组，再输出资源表
    c_lines = [
        "/* 本文件由 tools/gif2c.py 自动生成，请勿手工修改。 */",
        "",
        '#include "gif_assets.h"',
        "",
        "/* ---- 静态资源数组 ---- */",
    ]
    for body in bodies:
        c_lines.append(body)

    c_lines.append("")
    c_lines.append("/* ---- 情绪资源表（顺序即轮播顺序） ---- */")
    c_lines.append("const lummiss_gif_asset_t gif_assets[LUMMISS_GIF_COUNT] = {")
    for (emo_label, file_name), info in zip(EMOTIONS, infos):
        n = info[1]
        ident = "gif_" + VAR_SUFFIX[emo_label]
        c_lines.append("    {")
        c_lines.append(f'        .name = "{emo_label}",')
        c_lines.append(f"        .frame_count = {n},")
        c_lines.append(f"        .palette     = {ident}_palette,")
        c_lines.append(f"        .duration_ms = {ident}_duration,")
        c_lines.append(f"        .frame_offset= {ident}_offset,")
        c_lines.append(f"        .rle_data    = {ident}_rle,")
        c_lines.append("    },")
    c_lines.append("};")
    c_lines.append("")
    with open(out_c, "w", encoding="utf-8") as f:
        f.write("\n".join(c_lines))

    # 打印统计
    total_bytes = 0
    print(f"{'情绪':<8}{'帧数':>4}{'RLE(B)':>10}{'累计(B)':>10}")
    for (emo_label, n, rle_len, raw) in infos:
        total_bytes += rle_len
        print(f"{emo_label:<8}{n:>4}{rle_len:>10}{raw:>10}")
    print(f"资源总量约: {total_bytes/1024:.1f} KB")
    print(f"生成完成: {out_h}\n          {out_c}")


if __name__ == "__main__":
    main()
