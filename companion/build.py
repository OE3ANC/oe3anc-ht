#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build the static companion with paired release identity and protocol constants."""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import companion_release
from build_renderer import build as build_renderer


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tag')
    parser.add_argument('--emcc', default=os.environ.get('HT_EMCC', shutil.which('emcc')))
    parser.add_argument(
        '--lvgl',
        type=Path,
        default=Path.home() / 'snap/lisa/current/.listenai/csk-sdk-v2/modules/lib/gui/lvgl',
    )
    parser.add_argument('--base-path', default='/', help='Hosting root, e.g. /oe3anc-ht/')
    parser.add_argument('--renderer-build-dir', type=Path, help='Isolated renderer build directory')
    args = parser.parse_args()
    if not re.fullmatch(r'/(?:[A-Za-z0-9_-]+/)*', args.base_path):
        parser.error('base path must be an absolute directory path with a trailing slash')
    subprocess.run(
        [sys.executable, str(ROOT / 'tools/companion_contract.py'), '--check'], check=True
    )
    if not args.emcc:
        parser.error('Pinned Emscripten is required; pass --emcc /path/to/emcc')
    release = companion_release.metadata(args.tag)
    dist = ROOT / 'companion/dist'
    dist.mkdir(parents=True, exist_ok=True)
    for name in ('index.html', 'style.css'):
        shutil.copyfile(ROOT / 'companion' / name, dist / name)
    # Replace only this build's known source directory, avoiding stale modules.
    if (dist / 'src').exists():
        shutil.rmtree(dist / 'src')
    shutil.copytree(ROOT / 'companion/src', dist / 'src')
    if (dist / 'bootloader').exists():
        shutil.rmtree(dist / 'bootloader')
    shutil.copytree(ROOT / 'companion/bootloader', dist / 'bootloader')
    (dist / 'src/release.mjs').write_text('export const release = ' + json.dumps(release) + ';\n')
    (dist / 'src/site.mjs').write_text(
        'export const site = ' + json.dumps({'basePath': args.base_path}) + ';\n'
    )
    build_renderer(args.emcc, args.lvgl, dist / 'render', args.renderer_build_dir)
    (dist / 'release.json').write_text(json.dumps(release, indent=2) + '\n')
    print(f'Static companion: {dist}\nRelease identity: {release["identity"]}')


if __name__ == '__main__':
    main()
