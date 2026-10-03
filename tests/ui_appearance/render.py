#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Run actual firmware UI checks and export the twelve RGB565 Appearance renders."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile
from PIL import Image, ImageDraw, ImageFont


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=Path("build/tests/ui_appearance"))
    parser.add_argument("--output", type=Path, default=Path("build/screens/appearance"))
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    themes = ("Midnight", "Nord", "Solarized Dark", "Darcula")
    contrasts = ("Normal", "High", "Maximum")
    scale, gap, label_height = 3, 16, 30
    width, height = 160 * scale, 128 * scale
    sheet = Image.new(
        "RGB", (4 * (width + gap) + gap, 3 * (height + label_height + gap) + gap), "#10151e"
    )
    draw = ImageDraw.Draw(sheet)
    try:
        font = ImageFont.truetype("DejaVuSans.ttf", 18)
    except OSError:
        font = ImageFont.load_default()
    with tempfile.TemporaryDirectory(prefix="ht-appearance-render-") as directory:
        env = {**os.environ, "HT_UI_APPEARANCE_FRAMES": directory}
        result = subprocess.run(
            [str(args.build_dir.resolve() / "zephyr/zephyr.exe"), "-no-rt"],
            env=env,
            check=False,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
        )
        print(result.stdout, end="")
        # Some native ztest configurations exit zero even after assertions fail.
        if (
            result.returncode
            or "PROJECT EXECUTION SUCCESSFUL" not in result.stdout
            or "PROJECT EXECUTION FAILED" in result.stdout
        ):
            raise RuntimeError("Appearance firmware checks failed; renders were not published")
        for theme, name in enumerate(themes):
            for contrast, level in enumerate(contrasts):
                with Image.open(Path(directory) / f"appearance-{theme}-{contrast}.ppm") as frame:
                    frame.save(args.output / f"appearance-{theme}-{contrast}.png")
                    enlarged = frame.resize((width, height), Image.Resampling.NEAREST)
                x, y = gap + theme * (width + gap), gap + contrast * (height + label_height + gap)
                draw.text((x, y + 4), f"{name} / {level}", font=font, fill="#eaf1f6")
                sheet.paste(enlarged, (x, y + label_height))
    sheet.save(args.output / "overview.png")
    print(args.output / "overview.png")


if __name__ == "__main__":
    main()
