#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Run production UI tests and export the channel-editor RGB565 renders."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile
from PIL import Image, ImageDraw, ImageFont


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=Path("build/tests/ui_channel_editor"))
    parser.add_argument("--output", type=Path, default=Path("build/screens/channel-editor"))
    args = parser.parse_args()
    names = (
        "new-channel",
        "channel-name",
        "channel-frequency",
        "channel-radio",
        "fm-profile",
        "rx-tone",
        "invalid-dcs",
        "tx-dcs",
        "channel-tx-frequency",
        "channel-bank",
        "channel-review",
        "channel-replace",
        "channel-delete",
        "channel-pending",
        "channel-saved",
        "channel-unsaved",
        "m17-profile",
        "m17-destination",
        "channel-actions",
        "editor-appearance",
    )
    captions = (
        "New channel",
        "Physical keypad name",
        "Exact RX frequency",
        "RF settings",
        "FM profile",
        "Independent RX CTCSS",
        "Invalid DCS retained",
        "TX DCS / inverted",
        "Explicit TX frequency",
        "Bank addition",
        "Explicit review",
        "Confirm named replacement",
        "Confirm deletion",
        "Owner operation pending",
        "Durably saved",
        "Storage failure / unsaved",
        "M17 profile",
        "Station destination",
        "Duplicate / delete",
        "Appearance restored",
    )
    sheet = Image.new("RGB", (4 * 496 + 16, 5 * 430 + 16), "#10151e")
    draw = ImageDraw.Draw(sheet)
    try:
        font = ImageFont.truetype("DejaVuSans.ttf", 18)
    except OSError:
        font = ImageFont.load_default()
    with tempfile.TemporaryDirectory(prefix="ht-channel-render-") as directory:
        result = subprocess.run(
            [str(args.build_dir.resolve() / "zephyr/zephyr.exe"), "-no-rt"],
            env={**os.environ, "HT_UI_EDITOR_FRAMES": directory},
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
        x, y = 16 + i % 4 * 496, 16 + i // 4 * 430
        draw.text((x, y), caption, font=font, fill="#eaf1f6")
        sheet.paste(frame.resize((480, 384), Image.Resampling.NEAREST), (x, y + 30))
    sheet.save(args.output / "overview.png")
    print(args.output / "overview.png")


if __name__ == "__main__":
    main()
