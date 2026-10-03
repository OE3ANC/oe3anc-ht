#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Run production screen movement tests and export their native RGB565 renders."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile
from PIL import Image, ImageDraw, ImageFont


def sheet(frames, names, captions, columns, destination):
    rows = (len(names) + columns - 1) // columns
    result = Image.new("RGB", (columns * 496 + 16, rows * 430 + 16), "#10151e")
    draw = ImageDraw.Draw(result)
    try:
        font = ImageFont.truetype("DejaVuSans.ttf", 18)
    except OSError:
        font = ImageFont.load_default()
    for index, (name, caption) in enumerate(zip(names, captions)):
        x, y = 16 + index % columns * 496, 16 + index // columns * 430
        draw.text((x, y), caption, font=font, fill="#eaf1f6")
        result.paste(frames[name].resize((480, 384), Image.Resampling.NEAREST), (x, y + 30))
    result.save(destination)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=Path("build/tests/ui_motion"))
    parser.add_argument("--output", type=Path, default=Path("build/screens/motion"))
    args = parser.parse_args()
    overview = (
        "menu-start",
        "menu-40ms",
        "menu-80ms",
        "menu-settled",
        "appearance-start",
        "frequency-start",
        "hex-start",
        "motion-off",
        "live-rx",
        "tx-fixed",
        "tx-warning",
        "tx-timeout",
        "fault-fixed",
        "inactive-fixed",
        "error-fixed",
    )
    captions = (
        "Menu / entry at 0 ms",
        "Menu / after 40 ms",
        "Menu / after 80 ms",
        "Menu / settled after 140 ms",
        "Appearance / entry",
        "Frequency / immediate text input",
        "Hex / immediate editor",
        "Motion off / immediate",
        "Live RX / movement cancelled",
        "PTT / movement cancelled",
        "TX countdown / immediate",
        "Timeout / immediate",
        "Fault / immediate",
        "Inactive / immediate",
        "Field error / immediate",
    )
    expected = overview + ("pending-layout-fault",)
    with tempfile.TemporaryDirectory(prefix="ht-motion-render-") as directory:
        result = subprocess.run(
            [str(args.build_dir.resolve() / "zephyr/zephyr.exe"), "-no-rt"],
            env={**os.environ, "HT_UI_MOTION_FRAMES": directory},
            check=False,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
        )
        print(result.stdout, end="")
        if (
            result.returncode
            or "PROJECT EXECUTION SUCCESSFUL" not in result.stdout
            or "PROJECT EXECUTION FAILED" in result.stdout
        ):
            raise RuntimeError("UI checks failed; renders were not published")
        frames = {}
        # Validate every expected native frame before publishing artifacts.
        for name in expected:
            with Image.open(Path(directory) / f"{name}.ppm") as frame:
                if frame.size != (160, 128):
                    raise ValueError("Unexpected radio display geometry")
                frames[name] = frame.copy()
    args.output.mkdir(parents=True, exist_ok=True)
    for name, frame in frames.items():
        frame.save(args.output / f"{name}.png")
    sheet(frames, overview, captions, 4, args.output / "overview.png")
    animated = [frames[name].resize((640, 512), Image.Resampling.NEAREST) for name in overview[:4]]
    animated[0].save(
        args.output / "menu-entry.gif",
        save_all=True,
        append_images=animated[1:],
        duration=[40, 40, 60, 860],
        loop=0,
        disposal=2,
    )
    print(args.output / "overview.png")


if __name__ == "__main__":
    main()
