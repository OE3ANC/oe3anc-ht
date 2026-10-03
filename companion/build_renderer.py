#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build the same pinned LVGL/presentation renderer for the static companion."""
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from build import verify_revision
import yaml


def build(emcc, lvgl, output, directory=None):
    emcc = Path(emcc).resolve()
    expected = json.loads((ROOT / 'companion/renderer/toolchain.json').read_text())['emscripten']
    version = subprocess.check_output([str(emcc), '--version'], text=True)
    if not re.search(r'^emcc .* ' + re.escape(expected) + r'(?: |\n)', version, re.M):
        raise RuntimeError(f'Web renderer requires Emscripten {expected}')
    manifest = yaml.safe_load((ROOT / 'config/west-c62.yml').read_text())
    pin = next(p['revision'] for p in manifest['manifest']['projects'] if p['name'] == 'lvgl')
    verify_revision(lvgl, pin)
    directory = directory or ROOT / 'build/companion-renderer'
    subprocess.run(
        [
            str(emcc.parent / 'emcmake'),
            'cmake',
            '-S',
            str(ROOT / 'companion/renderer'),
            '-B',
            str(directory),
            '-G',
            'Ninja',
            '-DHT_LVGL_DIR=' + str(lvgl.resolve()),
            '-DCMAKE_BUILD_TYPE=Release',
        ],
        check=True,
    )
    subprocess.run(['cmake', '--build', str(directory), '--parallel', '4'], check=True)
    output.mkdir(parents=True, exist_ok=True)
    for name in ('ht-ui.mjs', 'ht-ui.wasm'):
        shutil.copyfile(directory / name, output / name)
