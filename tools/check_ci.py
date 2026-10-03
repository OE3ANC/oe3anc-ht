#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Run required release gates using only explicit, bootstrapped dependency paths."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys

import companion_release
from build import ROOT

# Required gates cover serial ownership, RF leases, shared input and settings/NVS.
NATIVE_TESTS = (
    'companion_cps',
    'companion_keys',
    'companion_ptt',
    'c62_companion',
    'c62_radio',
    'radio',
    'codeplug_storage',
    'ui',
    'ui_lock_service',
    'channels',
    'settings',
    'codeplug_profile',
)


def run(*args, **options):
    options.setdefault('cwd', ROOT)
    subprocess.run([str(arg) for arg in args], check=True, **options)


def check(dependencies, tag, base_path):
    pins = json.loads((ROOT / 'config/ci-toolchains.json').read_text())
    node = dependencies / 'emsdk/node' / (pins['node'] + '_64bit/bin/node')
    c62 = dependencies / 'c62'
    lvgl = c62 / 'modules/lib/gui/lvgl'
    common = [
        '--csk-workspace',
        c62,
        '--dsp-modules',
        c62 / 'modules/lib',
        '--emulator-zephyr',
        dependencies / 'emulator/zephyr',
        '--lvgl',
        lvgl,
        '--codec2',
        c62 / 'modules/lib/codec2',
        '--python',
        sys.executable,
        '--sdk',
        dependencies / ('zephyr-sdk-' + pins['zephyr_sdk']),
    ]
    build = ROOT / 'build/ci'
    expected = companion_release.metadata(tag)
    for script in (
        'tests/build/test_build.py',
        'tests/build/test_ci.py',
        'tests/build/test_release_pipeline.py',
        'tests/build/test_pages.py',
        'tests/codeplug/test_codeplug.py',
        'tests/codeplug/test_profiles.py',
        'tests/companion/test_release.py',
    ):
        run(sys.executable, ROOT / script)
    for script in (
        'tests/companion/check.py',
        'tests/companion/check_cps.py',
        'tests/companion/test_firmware_bundle.py',
    ):
        run(sys.executable, ROOT / script, '--node', node)
    for script in ('bootloader.mjs', 'calibration.mjs', 'firmware_tools.mjs'):
        run(node, ROOT / 'tests/companion' / script)
    for target in ('c62', 'emulator'):
        args = ['--release-tag', tag] if tag else []
        run(
            sys.executable,
            ROOT / 'tools/build.py',
            target,
            '--build-dir',
            build / target,
            *common,
            *args,
        )
    args = ['--tag', tag] if tag else []
    run(
        sys.executable,
        ROOT / 'companion/build.py',
        '--emcc',
        dependencies / 'emsdk/upstream/emscripten/emcc',
        '--lvgl',
        lvgl,
        '--renderer-build-dir',
        build / 'web-renderer',
        '--base-path',
        base_path,
        *args,
    )
    for path in (
        build / 'c62/companion-generated/release.json',
        build / 'emulator/companion-generated/release.json',
        ROOT / 'companion/dist/release.json',
    ):
        if json.loads(path.read_text()) != expected:
            raise ValueError(f'Paired build metadata mismatch: {path}')
    run(
        node,
        ROOT / 'tests/companion/firmware_bundle.mjs',
        build / 'c62/zephyr/firmware-bundle.json',
    )
    env = os.environ.copy()
    env['SDL_VIDEODRIVER'] = 'dummy'
    cases = [(name, None) for name in NATIVE_TESTS]
    cases += [(name, ROOT / 'tests' / name / 'nvs.conf') for name in ('codeplug_storage', 'settings')]
    for name, overlay in cases:
        directory = build / 'tests' / (name + ('-nvs' if overlay else ''))
        run(
            sys.executable,
            ROOT / 'tools/build.py',
            'emulator',
            '--app',
            ROOT / 'tests' / name,
            '--build-dir',
            directory,
            *common,
            *(['--extra-conf', overlay] if overlay else []),
        )
        if name == 'codeplug_profile':
            probe_env = dict(env, HT_CODEPLUG_PROBE=str(directory / 'zephyr/zephyr.exe'))
            run(sys.executable, ROOT / 'tests/codeplug/test_profiles.py', env=probe_env)
        else:
            run(directory / 'zephyr/zephyr.exe', '-no-rt', env=env, timeout=120, cwd=directory)
    native = build / 'native-renderer'
    run(
        'cmake',
        '-S',
        ROOT / 'companion/renderer',
        '-B',
        native,
        '-G',
        'Ninja',
        '-DHT_LVGL_DIR=' + str(lvgl),
        '-DCMAKE_BUILD_TYPE=Release',
    )
    run('cmake', '--build', native, '--parallel', '4')
    run(
        node,
        ROOT / 'tests/companion/render.mjs',
        native / 'ht-ui',
        ROOT / 'companion/dist/render/ht-ui.mjs',
    )
    if companion_release.metadata(tag) != expected:
        raise ValueError('Source/release metadata changed while running CI')
    print('PASS: required release gates and paired builds', expected['identity'])


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dependencies', type=Path, default=ROOT / '.deps/ci')
    parser.add_argument('--tag')
    parser.add_argument('--base-path', default='/oe3anc-ht/')
    args = parser.parse_args()
    check(args.dependencies.resolve(), args.tag, args.base_path)
