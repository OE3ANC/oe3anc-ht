#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Run production fault/inactive tests and export their native RGB565 renders."""
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
    parser.add_argument("--build-dir", type=Path, default=Path("build/tests/ui_system"))
    parser.add_argument("--output", type=Path, default=Path("build/screens/system-states"))
    args = parser.parse_args()
    overview = (
        "fault-tx",
        "inactive-fault",
        "inactive-unknown",
        "inactive-unknown-error",
        "inactive-cradle",
        "inactive-read-error",
        "inactive-stale",
        "inactive-confirming",
        "fault-preview-discarded",
        "fault-resume",
        "home-restored",
        "menu-restored",
    )
    captions = (
        "TX fault / immediate stop",
        "Inactive / retained fault",
        "No valid battery sample",
        "Unknown / read error",
        "Cradle / switch off",
        "Read error / retained voltage",
        "Stale / unknown switch",
        "Switch-on confirmation",
        "Preview discarded on fault",
        "Switch-on preserves fault",
        "Restart / Home restored",
        "Menu geometry restored",
    )
    extra = ()
    themes = (
        "Midnight", "Nord", "Solarized Dark", "Darcula",
        "Terminal Green", "Terminal Amber", "Terminal Ice",
    )
    contrasts = ("Normal", "High", "Maximum")
    galleries = {}
    for view in ("fault", "inactive"):
        names = [f"{view}-theme-{theme}-{contrast}" for contrast in range(3) for theme in range(len(themes))]
        labels = [f"{theme} / {contrast}" for contrast in contrasts for theme in themes]
        galleries[view] = names, labels
    expected = list(overview + extra) + [name for names, _ in galleries.values() for name in names]
    with tempfile.TemporaryDirectory(prefix="ht-system-render-") as directory:
        result = subprocess.run(
            [str(args.build_dir.resolve() / "zephyr/zephyr.exe"), "-no-rt"],
            env={**os.environ, "HT_UI_SYSTEM_FRAMES": directory},
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
    for view, (names, labels) in galleries.items():
        sheet(frames, names, labels, len(themes), args.output / f"{view}-themes.png")
    print(args.output / "overview.png")


if __name__ == "__main__":
    main()
