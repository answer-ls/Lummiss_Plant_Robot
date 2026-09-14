from pathlib import Path
import subprocess

src = Path("tools/sd_gif_out")
dst = Path("tools/gif")

dst.mkdir(parents=True, exist_ok=True)

for gif in src.glob("*.gif"):

    out = dst / (gif.stem + ".bin")

    subprocess.run([
        "python",
        "tools/gif_to_rgb565_bin.py",
        str(gif),
        str(out),
        "--width", "320",
        "--height", "240",
        "--byte-order", "be"
    ], check=True)