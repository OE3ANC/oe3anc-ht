#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build against pinned installed dependencies without changing their checkouts."""
import argparse
import os
from pathlib import Path
import subprocess
import sys

import yaml

ROOT = Path(__file__).resolve().parents[1]
LISA = Path.home() / 'snap/lisa/current/.listenai'


def verify_revision(path, revision):
    actual = subprocess.check_output(
        ['git', '-C', str(path), 'rev-parse', 'HEAD'], text=True
    ).strip()
    if actual != revision:
        raise RuntimeError(f'{path}: expected {revision}, found {actual}')
    changes = subprocess.check_output(
        ['git', '-C', str(path), 'status', '--porcelain', '--untracked-files=no'], text=True
    )
    if changes:
        raise RuntimeError(f'{path}: tracked dependency files have local changes')


def verify_sdk(path):
    version_file = path / "sdk_version"
    if not version_file.is_file():
        raise RuntimeError(f"Zephyr SDK not found: {path}; pass --sdk")
    version = version_file.read_text().strip()
    if version != "0.16.1":
        raise RuntimeError(f"{path}: expected SDK 0.16.1, found {version}")


def verify_c62_boot_stack(build):
    config = dict(
        line.split('=', 1)
        for line in (build / 'zephyr/.config').read_text().splitlines()
        if line.startswith('CONFIG_') and '=' in line
    )
    if config.get('CONFIG_HT_SETTINGS') != 'y':
        return

    # Check the hardware-reproduced overflow path with the actual ARM frames.
    # Reserve 512 bytes for decoder helpers, kernel entry and exception context;
    # this focused regression guard is not a whole-program stack bound.
    def frame(path, signature):
        for line in (build / path).read_text().splitlines():
            location, size, kind = line.split('\t')
            if location.endswith(signature):
                if kind != 'static':
                    raise RuntimeError(f'Unbounded boot stack frame: {signature}')
                return int(size)
        raise RuntimeError(f'Missing boot stack frame: {signature}')

    settings = 'modules/ht/settings/CMakeFiles/ht_settings.dir/'
    records = 'modules/ht/channels/CMakeFiles/ht_channels.dir/records.cpp.su'
    required = (
        frame('CMakeFiles/app.dir/app/main.cpp.su', 'int main()')
        + frame(settings + 'service.cpp.su', 'int ht::settings_start(RadioConfig&, Selection&)')
        + frame(settings + 'codeplug_store.cpp.su', 'int ht::codeplug_load(Codeplug&, uint32_t&)')
        + max(
            frame(records, 'int ht::decode_manifest(const uint8_t*, size_t, uint32_t, '
                  'CodeplugManifest&)'),
            frame(records, 'int ht::decode_channel(const uint8_t*, size_t, uint32_t, Channel&)'),
            frame(records, 'int ht::decode_bank(const uint8_t*, size_t, uint32_t, Bank&)'),
        )
        + 512
    )
    available = int(config['CONFIG_MAIN_STACK_SIZE'])
    if required > available:
        raise RuntimeError(f'C62 boot stack requires {required} bytes; configured {available}')
    print(f'C62 boot stack: {required} bytes including reserve / {available} configured')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('target', choices=('c62', 'emulator'))
    parser.add_argument(
        '--csk-workspace',
        type=Path,
        default=Path(os.environ.get('HT_CSK_WORKSPACE', LISA / 'csk-sdk-v2')),
    )
    parser.add_argument(
        '--emulator-zephyr',
        type=Path,
        default=Path(os.environ.get('HT_EMULATOR_ZEPHYR', Path.home() / 'openrtx-build/zephyr')),
    )
    parser.add_argument('--python', type=Path, default=LISA / 'lisa-zephyr/venv/bin/python3')
    parser.add_argument(
        '--sdk',
        type=Path,
        default=LISA / 'lisa-zephyr/packages/node_modules/' '@binary/zephyr-sdk-0.16.1/binary',
    )
    parser.add_argument('--lvgl', type=Path, default=LISA / 'csk-sdk-v2/modules/lib/gui/lvgl')
    parser.add_argument(
        '--codec2',
        type=Path,
        default=os.environ.get('HT_CODEC2_DIR'),
        help='pinned Codec2-mod checkout (default: C62 workspace/modules/lib/codec2-mod)',
    )
    parser.add_argument(
        '--dsp-modules',
        type=Path,
        help='directory containing pinned af, lsf, urpc and freertos_shims',
    )
    parser.add_argument('--release-tag', help='clean tagged companion/firmware release identity')
    parser.add_argument('--app', type=Path, default=ROOT)
    parser.add_argument('--build-dir', type=Path)
    parser.add_argument(
        '--extra-conf',
        type=Path,
        action='append',
        default=[],
        help='additional Kconfig overlay (may be repeated)',
    )
    args = parser.parse_args()
    if args.app.resolve() != ROOT and args.build_dir is None:
        parser.error('--app requires its own --build-dir')
    for overlay in args.extra_conf:
        if not overlay.is_file() or ';' in str(overlay.resolve()):
            parser.error(f'invalid Kconfig overlay: {overlay}')
    manifest = ROOT / ('config/west-c62.yml' if args.target == 'c62' else 'west.yml')
    projects = yaml.safe_load(manifest.read_text())['manifest']['projects']
    workspace = args.csk_workspace.resolve()
    zephyr = (workspace / 'zephyr' if args.target == 'c62' else args.emulator_zephyr).resolve()
    verify_revision(zephyr, projects[0]['revision'])
    lvgl = args.lvgl.resolve()
    lvgl_pin = next(p['revision'] for p in projects if p['name'] == 'lvgl')
    verify_revision(lvgl, lvgl_pin)
    codec2 = (args.codec2 or workspace / 'modules/lib/codec2-mod').resolve()
    codec2_pin = next(p['revision'] for p in projects if p['name'] == 'codec2')
    verify_revision(codec2, codec2_pin)
    # Register only the project integration module, never vendor LVGL glue.
    # Zephyr's two baselines expect different LVGL releases; our UI module
    # builds the same verified sources with the same configuration on both.
    build = (args.build_dir or ROOT / 'build' / args.target).resolve()
    modules = [str(ROOT)]
    if args.target == 'c62':
        dsp_modules = (
            args.dsp_modules
            or (
                workspace / 'modules/lib'
                if (workspace / 'modules/lib/af').is_dir()
                else Path.home() / 'openrtx-build/modules/lib'
            )
        ).resolve()
        for project in projects:
            if project['name'] in ('csk', 'csk6_cm33', 'cmsis'):
                module = workspace / project['path']
                verify_revision(module, project['revision'])
                modules.append(str(module))
            elif project['name'] in ('af', 'lsf', 'urpc', 'freertos_shims'):
                module = dsp_modules / project['name']
                verify_revision(module, project['revision'])
                modules.append(str(module))
    if not args.python.is_file():
        raise RuntimeError(f'Zephyr Python not found: {args.python}; pass --python')
    env = os.environ.copy()
    env.update(ZEPHYR_BASE=str(zephyr), CCACHE_DISABLE='1')
    env['PATH'] = str(args.python.parent) + os.pathsep + env['PATH']
    env.pop('ZEPHYR_EXTRA_MODULES', None)
    if args.target == 'c62':
        verify_sdk(args.sdk)
        env.update(
            ZEPHYR_TOOLCHAIN_VARIANT='zephyr',
            ZEPHYR_SDK_INSTALL_DIR=str(args.sdk.resolve()),
            CSK_BASE=str(workspace / 'csk'),
        )
    else:
        env['ZEPHYR_TOOLCHAIN_VARIANT'] = 'host'
    command = [
        'cmake',
        '-S',
        str(args.app.resolve()),
        '-B',
        str(build),
        '-G',
        'Ninja',
        '-DBOARD=' + ('c62' if args.target == 'c62' else 'native_sim_64'),
        '-DZEPHYR_BASE=' + str(zephyr),
        '-DPYTHON_EXECUTABLE=' + str(args.python.absolute()),
        '-DPython3_EXECUTABLE=' + str(args.python.absolute()),
        '-DZEPHYR_MODULES=' + ';'.join(modules),
        '-DZEPHYR_EXTRA_MODULES=',
        '-DHT_LVGL_DIR=' + str(lvgl),
        '-DHT_CODEC2_DIR=' + str(codec2),
        '-DHT_RELEASE_TAG=' + (args.release_tag or ''),
        '-DUSER_CACHE_DIR=' + str(ROOT / '.cache/zephyr'),
        '-DEXTRA_CONF_FILE='
        + ';'.join(
            [str(ROOT / 'config' / (args.target + '.conf'))]
            + [str(overlay.resolve()) for overlay in args.extra_conf]
        ),
    ]
    subprocess.run(command, env=env, check=True)
    subprocess.run(['cmake', '--build', str(build)], env=env, check=True)
    if args.target == 'c62' and args.app.resolve() == ROOT:
        verify_c62_boot_stack(build)


if __name__ == '__main__':
    try:
        main()
    except (RuntimeError, subprocess.CalledProcessError) as error:
        print(error, file=sys.stderr)
        sys.exit(1)
