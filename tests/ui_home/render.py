#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Run production UI tests and export the Home RGB565 renders."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile
from PIL import Image, ImageDraw, ImageFont


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=Path("build/tests/ui_home"))
    parser.add_argument("--output", type=Path, default=Path("build/screens/home"))
    args = parser.parse_args()
    names = (
        'vfo-exact',
        'memory-rx',
        'memory-tx',
        'm17-rx',
        'rssi-full',
        'monitor',
        'receive-only',
        'locked',
        'tx-warning',
        'tx-timeout',
        'storage-pending',
        'storage-error',
        'battery-unknown',
        'charger',
        'battery-stale',
        'theme-1-1',
        'docs-vfo',
        'docs-memory',
        'docs-m17',
    )
    captions = (
        'VFO / exact integer Hz',
        'Memory / bank context',
        'Memory / split TX frequency',
        'M17 / received callsign',
        'Relative RSSI bars',
        'Held FM monitor',
        'Receive-only channel',
        'Locked front keypad',
        'Final ten-second warning',
        'Timeout / release PTT',
        'Accepted / storage pending',
        'Storage failure / unsaved',
        'Battery unknown',
        'Charger input present',
        'Retained stale voltage',
        'Nord / High contrast',
        'VFO / documentation',
        'Memory / documentation',
        'M17 / documentation',
    )
    sheet = Image.new("RGB", (4 * 496 + 16, 5 * 430 + 16), "#10151e")
    draw = ImageDraw.Draw(sheet)
    try:
        font = ImageFont.truetype("DejaVuSans.ttf", 18)
    except OSError:
        font = ImageFont.load_default()
    with tempfile.TemporaryDirectory(prefix="ht-home-render-") as directory:
        result = subprocess.run(
            [str(args.build_dir.resolve() / "zephyr/zephyr.exe"), "-no-rt"],
            env={**os.environ, "HT_UI_HOME_FRAMES": directory},
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
        themes = []
        for theme in range(4):
            for contrast in range(3):
                with Image.open(Path(directory) / f"theme-{theme}-{contrast}.ppm") as frame:
                    themes.append(frame.copy())
    args.output.mkdir(parents=True, exist_ok=True)
    for i, (name, caption, frame) in enumerate(zip(names, captions, frames)):
        frame.save(args.output / f"{name}.png")
        x, y = 16 + i % 4 * 496, 16 + i // 4 * 430
        draw.text((x, y), caption, font=font, fill="#eaf1f6")
        sheet.paste(frame.resize((480, 384), Image.Resampling.NEAREST), (x, y + 30))
    sheet.save(args.output / "overview.png")
    theme_sheet = Image.new("RGB", (4 * 496 + 16, 3 * 430 + 16), "#10151e")
    theme_draw = ImageDraw.Draw(theme_sheet)
    for theme, title in enumerate(("Midnight", "Nord", "Solarized Dark", "Darcula")):
        for contrast, level in enumerate(("Normal", "High", "Maximum")):
            frame = themes[theme * 3 + contrast]
            frame.save(args.output / f"theme-{theme}-{contrast}.png")
            x, y = 16 + theme * 496, 16 + contrast * 430
            theme_draw.text((x, y), f"{title} / {level}", font=font, fill="#eaf1f6")
            theme_sheet.paste(frame.resize((480, 384), Image.Resampling.NEAREST), (x, y + 30))
    theme_sheet.save(args.output / "themes.png")
    print(args.output / "overview.png")


if __name__ == "__main__":
    main()
