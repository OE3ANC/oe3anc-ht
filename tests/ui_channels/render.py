#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Run production UI tests and export the channel-list RGB565 renders."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile
from PIL import Image, ImageDraw, ImageFont


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=Path("build/tests/ui_channels"))
    parser.add_argument("--output", type=Path, default=Path("build/screens/channels"))
    args = parser.parse_args()
    names = (
        "channels",
        "channels-page-two",
        "channels-local",
        "banks",
        "banks-last-page",
        "channels-last-page",
        "channels-empty",
        "channels-empty-bank",
        "channel-number",
        "channel-not-in-bank",
        "recall-pending",
        "appearance-after-list",
    )
    captions = (
        "All / first page",
        "All / second page",
        "Local / explicit bank order",
        "Bank selection",
        "Sixteen banks",
        "256 channels",
        "Empty memory",
        "Empty bank",
        "Numeric selection",
        "Number outside chosen bank",
        "Recall pending",
        "Appearance after browsing",
    )
    sheet = Image.new("RGB", (3 * 496 + 16, 4 * 430 + 16), "#10151e")
    draw = ImageDraw.Draw(sheet)
    try:
        font = ImageFont.truetype("DejaVuSans.ttf", 18)
    except OSError:
        font = ImageFont.load_default()
    with tempfile.TemporaryDirectory(prefix="ht-channel-render-") as directory:
        result = subprocess.run(
            [str(args.build_dir.resolve() / "zephyr/zephyr.exe"), "-no-rt"],
            env={**os.environ, "HT_UI_CHANNEL_FRAMES": directory},
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
        x, y = 16 + i % 3 * 496, 16 + i // 3 * 430
        draw.text((x, y), caption, font=font, fill="#eaf1f6")
        sheet.paste(frame.resize((480, 384), Image.Resampling.NEAREST), (x, y + 30))
    sheet.save(args.output / "overview.png")
    print(args.output / "overview.png")


if __name__ == "__main__":
    main()
