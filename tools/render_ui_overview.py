#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compose the current native LVGL galleries after running tests/*/render.py."""
from pathlib import Path
import html
from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[1]
IMAGES = ROOT / "build/screens"
# One representative for every production screen, plus key operating states.
SCREENS = (
    ("home/vfo-exact", "VFO / direct tuning"),
    ("home/memory-rx", "Memory / channel and bank"),
    ("channels/channels", "Channel list"),
    ("channels/banks", "Bank selection"),
    ("channels/channel-number", "Direct channel number"),
    ("text-entry/frequency", "Frequency entry"),
    ("text-entry/callsign", "Local callsign"),
    ("menu-status/menu-workflows", "Menu"),
    ("appearance/appearance-0-1", "Appearance / theme and contrast"),
    ("quick/quick-vfo", "Quick controls"),
    ("backlight/backlight-preview", "Backlight / idle dimming"),
    ("limit/limit-180", "Transmit limit"),
    ("vfo/vfo-step", "VFO tuning step"),
    ("menu-status/status-fm", "Status / radio"),
    ("channel-editor/new-channel", "Channel editor"),
    ("channel-editor/channel-name", "Channel field / name"),
    ("channel-editor/rx-tone", "FM tone chooser"),
    ("channel-editor/channel-bank", "Channel bank membership"),
    ("channel-editor/channel-review", "Channel review"),
    ("channel-editor/channel-replace", "Replacement confirmation"),
    ("channel-editor/channel-saved", "Durable channel save"),
    ("banks/bank-new", "Bank editor"),
    ("banks/bank-name", "Bank name"),
    ("banks/bank-members", "Ordered members"),
    ("banks/bank-add", "Add bank member"),
    ("banks/bank-actions-top", "Member actions"),
    ("banks/bank-delete", "Delete bank confirmation"),
    ("banks/bank-saved", "Durable bank save"),
    ("diagnostic/diagnostic", "Exclusive register diagnostics"),
    ("diagnostic/hex-value", "Hex register field"),
    ("system-states/fault-tx", "Latched fault / reboot required"),
    ("system-states/inactive-cradle", "Cradle / switch off"),
    ("home/rssi-full", "Relative RSSI bars"),
    ("home/m17-rx", "M17 / received callsign"),
    ("home/tx-warning", "TX countdown"),
    ("home/tx-timeout", "TX timeout / release"),
    ("home/battery-unknown", "Battery unknown"),
    ("home/charger", "Charger input present"),
    ("home/battery-stale", "Retained stale voltage"),
    ("channel-editor/channel-unsaved", "Save error / not durable"),
)


def main():
    frames = []
    for name, caption in SCREENS:
        with Image.open(IMAGES / (name + ".png")) as frame:
            if frame.size != (160, 128):
                raise ValueError(f"Non-native frame: {name}")
            frames.append(frame.copy())
    sheet = Image.new("RGB", (4 * 344 + 16, ((len(frames) + 3) // 4) * 292 + 72), "#10151e")
    draw = ImageDraw.Draw(sheet)
    try:
        font = ImageFont.truetype("DejaVuSans.ttf", 15)
    except OSError:
        font = ImageFont.load_default()
    draw.text((16, 16), "OE3ANC HT / actual production LVGL screens", font=font, fill="#eaf1f6")
    draw.text(
        (16, 40),
        "160 x 128 RGB565 / integer 2x / software renders; hardware acceptance pending",
        font=font,
        fill="#8eabbc",
    )
    for i, (frame, (_, caption)) in enumerate(zip(frames, SCREENS)):
        x, y = 16 + i % 4 * 344, 72 + i // 4 * 292
        draw.text((x, y), caption, font=font, fill="#eaf1f6")
        sheet.paste(frame.resize((320, 256), Image.Resampling.NEAREST), (x, y + 24))
    sheet.save(IMAGES / "overview.png")
    sections = []
    count = 0
    for directory in sorted(IMAGES.iterdir()):
        if not directory.is_dir():
            continue
        cards = []
        for path in sorted(directory.glob("*.png")):
            with Image.open(path) as frame:
                if frame.size != (160, 128):
                    continue
            url = html.escape(str(path.relative_to(IMAGES)), quote=True)
            title = html.escape(path.stem.replace("-", " "))
            cards.append(
                f'<figure><a href="{url}"><img src="{url}" alt="{title}" loading="lazy"></a><figcaption>{title}</figcaption></figure>'
            )
            count += 1
        if cards:
            sections.append(
                f'<h2>{html.escape(directory.name)}</h2><section>{"".join(cards)}</section>'
            )
    (IMAGES / "gallery.html").write_text(
        '<!doctype html><html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width">'
        '<title>OE3ANC HT production gallery</title><style>'
        'body{background:#10151e;color:#eaf1f6;font:16px system-ui;margin:24px}section{display:flex;flex-wrap:wrap;gap:24px}'
        'figure{margin:0;width:320px}img{width:320px;height:256px;image-rendering:pixelated}figcaption{padding:8px 0;color:#8eabbc}'
        '</style><h1>Production LVGL gallery</h1><p>Native RGB565 frames at integer 2x. Click a frame for native size. '
        'Static fixtures show software behavior; physical C62 acceptance is pending.</p>'
        + ''.join(sections)
        + '</html>\n'
    )
    print(f"Overview: {len(frames)} states; native gallery: {count} frames")


if __name__ == "__main__":
    main()
