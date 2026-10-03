#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Run production UI tests and export the quick-controls RGB565 renders."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile
from PIL import Image, ImageDraw, ImageFont


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=Path("build/tests/ui_quick"))
    parser.add_argument("--output", type=Path, default=Path("build/screens/quick"))
    args = parser.parse_args()
    names = (
        "quick-vfo",
        "quick-preview",
        "quick-pending",
        "quick-memory",
        "quick-unavailable",
        "quick-busy",
        "quick-gain-fixture",
        "quick-theme-fixture",
    )
    captions = (
        "VFO / applied squelch",
        "Local squelch preview",
        "Controller Apply pending",
        "Memory / temporary squelch",
        "M17 / unavailable controls",
        "Full command queue / retry",
        "Gain capability test fixture",
        "Darcula Maximum / motion off fixture",
    )
    sheet = Image.new("RGB", (2 * 496 + 16, 4 * 430 + 16), "#10151e")
    draw = ImageDraw.Draw(sheet)
    try:
        font = ImageFont.truetype("DejaVuSans.ttf", 18)
    except OSError:
        font = ImageFont.load_default()
    with tempfile.TemporaryDirectory(prefix="ht-quick-render-") as directory:
        result = subprocess.run(
            [str(args.build_dir.resolve() / "zephyr/zephyr.exe"), "-no-rt"],
            env={**os.environ, "HT_UI_QUICK_FRAMES": directory},
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
        # Load every expected frame before publishing any artifacts.
        frames = []
        for name in names:
            with Image.open(Path(directory) / f"{name}.ppm") as frame:
                if frame.size != (160, 128):
                    raise ValueError("Unexpected radio display geometry")
                frames.append(frame.copy())
    args.output.mkdir(parents=True, exist_ok=True)
    for i, (name, caption, frame) in enumerate(zip(names, captions, frames)):
        frame.save(args.output / f"{name}.png")
        x, y = 16 + i % 2 * 496, 16 + i // 2 * 430
        draw.text((x, y), caption, font=font, fill="#eaf1f6")
        sheet.paste(frame.resize((480, 384), Image.Resampling.NEAREST), (x, y + 30))
    sheet.save(args.output / "overview.png")
    print(args.output / "overview.png")


if __name__ == "__main__":
    main()
