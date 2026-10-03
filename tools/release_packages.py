#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build deterministic paired archives and validate their publication manifest."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import zipfile

import companion_release
import firmware_bundle

ROOT = companion_release.ROOT

MANIFEST = 'release-pair.json'
MAX_PACKAGE = 16 * 1024 * 1024


def canonical(value):
    return (json.dumps(value, sort_keys=True, separators=(',', ':')) + '\n').encode()


def validate_manifest(data, tag, commit):
    if not re.fullmatch(r'v[0-9]+\.[0-9]+\.[0-9]+(?:-rc\.[0-9]+)?', tag):
        raise ValueError('Invalid version tag')
    if not re.fullmatch('[0-9a-f]{40}', commit):
        raise ValueError('Invalid release commit')
    if (
        not isinstance(data, dict)
        or set(data) != {'schema_version', 'release', 'packages'}
        or type(data['schema_version']) is not int
        or data['schema_version'] != 1
    ):
        raise ValueError('Unsupported release pair manifest')
    release = data['release']
    if (
        not isinstance(release, dict)
        or set(release) != {'identity', 'label', 'commit', 'source_sha256'}
        or release['identity'] != tag + '@' + commit
        or release['label'] != tag
        or release['commit'] != commit
        or not isinstance(release['source_sha256'], str)
        or not re.fullmatch('[0-9a-f]{64}', release['source_sha256'])
    ):
        raise ValueError('Release pair tag/commit/source mismatch')
    if not isinstance(data['packages'], list) or len(data['packages']) != 2:
        raise ValueError('Release pair requires two packages')
    for package, role in zip(data['packages'], ('firmware', 'companion')):
        if (
            not isinstance(package, dict)
            or set(package) != {'role', 'name', 'size', 'sha256'}
            or package['role'] != role
            or package['name'] != role + '-' + tag + '.zip'
            or type(package['size']) is not int
            or not 0 < package['size'] <= MAX_PACKAGE
            or not isinstance(package['sha256'], str)
            or not re.fullmatch('[0-9a-f]{64}', package['sha256'])
        ):
            raise ValueError('Invalid release package descriptor')
    return data


def validate_files(directory, manifest):
    contents = {}
    for package in manifest['packages']:
        path = directory / package['name']
        if path.is_symlink() or path.stat().st_size != package['size']:
            raise ValueError('Release package size/type mismatch: ' + package['name'])
        with path.open('rb') as source:
            data = source.read(MAX_PACKAGE + 1)
        if len(data) != package['size'] or hashlib.sha256(data).hexdigest() != package['sha256']:
            raise ValueError('Release package checksum mismatch: ' + package['name'])
        contents[package['name']] = data
    return contents


def archive(path, entries):
    # Stored ZIP entries avoid compressor-version differences on same-tag retries.
    with zipfile.ZipFile(path, 'w', compression=zipfile.ZIP_STORED) as output:
        for name, content in sorted(entries.items()):
            info = zipfile.ZipInfo(name, date_time=(1980, 1, 1, 0, 0, 0))
            info.create_system = 3
            info.external_attr = 0o100644 << 16
            output.writestr(info, content)
    if path.stat().st_size > MAX_PACKAGE:
        raise ValueError('Release package exceeds size limit')


def package(build, website, output, tag, dependencies):
    output.mkdir(parents=True, exist_ok=True)
    # A failed attempt must never leave a previous manifest ready for publication.
    (output / MANIFEST).unlink(missing_ok=True)
    release = companion_release.metadata(tag)
    if json.loads((build / 'emulator/companion-generated/release.json').read_text()) != release:
        raise ValueError('Emulator release metadata mismatch')
    if json.loads((website / 'release.json').read_text()) != release:
        raise ValueError('Companion release metadata mismatch')
    bundle_path = build / 'c62/zephyr/firmware-bundle.json'
    bundle = firmware_bundle.build_bundle(build / 'c62', tag)
    if json.loads(bundle_path.read_text()) != bundle:
        raise ValueError('Firmware bundle differs from the verified build')
    licenses = {
        str(path.relative_to(ROOT)): path.read_bytes()
        for path in sorted((ROOT / 'LICENSES').glob('*.txt'))
    }
    modules = dependencies / 'c62/modules/lib'
    licenses['LICENSES/LVGL.txt'] = (modules / 'gui/lvgl/LICENCE.txt').read_bytes()
    entries = dict(licenses)
    entries['LICENSES/Codec2.txt'] = (modules / 'codec2/COPYING').read_bytes()
    # Preserve KISS FFT's original attribution/license alongside its firmware binary.
    entries['LICENSES/kiss_fft.c'] = (modules / 'codec2/src/kiss_fft.c').read_bytes()
    entries.update(
        {'release.json': canonical(release), 'firmware-bundle.json': bundle_path.read_bytes()}
    )
    archive(output / ('firmware-' + tag + '.zip'), entries)
    # Include only files produced by this builder, never stray dist fixtures/files.
    paths = [
        Path(name)
        for name in (
            'index.html',
            'style.css',
            'release.json',
            'src/release.mjs',
            'src/site.mjs',
            'render/ht-ui.mjs',
            'render/ht-ui.wasm',
        )
    ]
    for folder in ('src', 'bootloader'):
        paths.extend(
            path.relative_to(ROOT / 'companion')
            for path in sorted((ROOT / 'companion' / folder).glob('*'))
            if path.is_file()
        )
    entries = {tag + '/' + name: content for name, content in licenses.items()}
    for relative in paths:
        path = website / relative
        if path.is_symlink():
            raise ValueError('Companion package cannot contain symlinks')
        entries[tag + '/' + relative.as_posix()] = path.read_bytes()
    archive(output / ('companion-' + tag + '.zip'), entries)
    manifest = {'schema_version': 1, 'release': release, 'packages': []}
    for role in ('firmware', 'companion'):
        path = output / (role + '-' + tag + '.zip')
        manifest['packages'].append(
            {
                'role': role,
                'name': path.name,
                'size': path.stat().st_size,
                'sha256': hashlib.sha256(path.read_bytes()).hexdigest(),
            }
        )
    validate_manifest(manifest, tag, release['commit'])
    if companion_release.metadata(tag) != release:
        raise ValueError('Release source changed during packaging')
    (output / MANIFEST).write_bytes(canonical(manifest))
    return manifest


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tag', required=True)
    parser.add_argument('--build-dir', type=Path, default=ROOT / 'build/ci')
    parser.add_argument('--website', type=Path, default=ROOT / 'companion/dist')
    parser.add_argument('--output', type=Path, default=ROOT / 'build/release')
    parser.add_argument('--dependencies', type=Path, default=ROOT / '.deps/ci')
    args = parser.parse_args()
    manifest = package(args.build_dir, args.website, args.output, args.tag, args.dependencies)
    print('Paired release archives:', manifest['release']['identity'])
