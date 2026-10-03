#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Fetch public pinned dependencies/toolchains for a clean Linux x86-64 CI build."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import subprocess

import yaml
from build import ROOT, verify_revision, verify_sdk


def run(*args, **options):
    subprocess.run([str(arg) for arg in args], check=True, **options)


def checkout(path, url, revision):
    if not url.startswith('https://') or not re.fullmatch('[0-9a-f]{40}', revision):
        raise ValueError('Dependency requires an HTTPS URL and immutable commit')
    if (path / '.git').is_dir():
        verify_revision(path, revision)
        return
    if path.exists() and any(path.iterdir()):
        raise ValueError(f'Refusing nonempty dependency destination: {path}')
    path.mkdir(parents=True, exist_ok=True)
    run('git', 'init', '-q', path)
    env = os.environ.copy()
    env['GIT_TERMINAL_PROMPT'] = '0'
    run(
        'git',
        '-c',
        'credential.helper=',
        '-c',
        'core.askPass=',
        '-C',
        path,
        'fetch',
        '--quiet',
        '--depth=1',
        url,
        revision,
        env=env,
    )
    run('git', '-C', path, 'checkout', '-q', '--detach', 'FETCH_HEAD')
    verify_revision(path, revision)
    print(f'Pinned dependency: {path.name} {revision}', flush=True)


def download(path, url, expected):
    if path.exists():
        if hashlib.sha256(path.read_bytes()).hexdigest() != expected:
            raise ValueError(f'Cached toolchain checksum mismatch: {path}')
        return
    temporary = path.with_suffix(path.suffix + '.partial')
    run(
        'curl',
        '-fLsS',
        '--retry',
        '3',
        '--connect-timeout',
        '20',
        '--max-time',
        '300',
        url,
        '--output',
        temporary,
    )
    if hashlib.sha256(temporary.read_bytes()).hexdigest() != expected:
        raise ValueError(f'Downloaded toolchain checksum mismatch: {path.name}')
    temporary.replace(path)


def bootstrap(directory):
    if platform.system() != 'Linux' or platform.machine() != 'x86_64':
        raise ValueError('CI bootstrap supports Linux x86-64')
    directory.mkdir(parents=True, exist_ok=True)
    entries = []
    for project in yaml.safe_load((ROOT / 'config/west-c62.yml').read_text())['manifest'][
        'projects'
    ]:
        destination = (directory / 'c62' / project['path']).resolve()
        if not destination.is_relative_to(directory):
            raise ValueError('Dependency destination escapes the CI directory')
        entries.append((destination, project['url'], project['revision']))
    upstream = yaml.safe_load((ROOT / 'west.yml').read_text())['manifest']['projects'][0]
    entries.append((directory / 'emulator/zephyr', upstream['url'], upstream['revision']))
    renderer = json.loads((ROOT / 'companion/renderer/toolchain.json').read_text())
    entries.append(
        (directory / 'emsdk', 'https://github.com/emscripten-core/emsdk', renderer['emsdk_commit'])
    )
    with ThreadPoolExecutor(max_workers=4) as pool:
        list(pool.map(lambda entry: checkout(*entry), entries))

    pins = json.loads((ROOT / 'config/ci-toolchains.json').read_text())
    sdk = directory / ('zephyr-sdk-' + pins['zephyr_sdk'])
    for index, (name, checksum) in enumerate(pins['sdk_archives'].items()):
        archive = directory / name
        download(
            archive,
            'https://github.com/zephyrproject-rtos/sdk-ng/releases/download/v'
            + pins['zephyr_sdk']
            + '/'
            + name,
            checksum,
        )
        marker = sdk / ('sdk_version' if index == 0 else 'arm-zephyr-eabi/bin/arm-zephyr-eabi-gcc')
        if not marker.is_file():
            run('tar', '-xf', archive, '-C', directory if index == 0 else sdk)
    verify_sdk(sdk)
    run(sdk / 'setup.sh', '-h', cwd=sdk)
    emsdk = directory / 'emsdk'
    run(emsdk / 'emsdk', 'install', renderer['emscripten'], cwd=emsdk)
    run(emsdk / 'emsdk', 'activate', renderer['emscripten'], cwd=emsdk)
    verify_revision(emsdk, renderer['emsdk_commit'])
    node = emsdk / 'node' / (pins['node'] + '_64bit/bin/node')
    if subprocess.check_output([str(node), '--version'], text=True).strip() != 'v' + pins['node']:
        raise ValueError('Unexpected bundled Node.js version')
    print(f'CI dependencies ready: {directory}', flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--directory', type=Path, default=ROOT / '.deps/ci')
    args = parser.parse_args()
    bootstrap(args.directory.resolve())
