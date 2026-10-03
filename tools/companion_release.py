#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Write one identity for firmware and web builds from the same source snapshot."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]
SOURCE_DIRS = (
    'app',
    'backends',
    'boards',
    'cmake',
    'companion',
    'config',
    'drivers',
    'dts',
    'include',
    'modules',
    'protocol',
    'schemas',
    'tools',
    'zephyr',
)


def metadata(tag=None):
    commit = subprocess.check_output(
        ['git', '-C', str(ROOT), 'rev-parse', 'HEAD'], text=True
    ).strip()
    digest = hashlib.sha256()
    paths = []
    for directory in SOURCE_DIRS:
        for path in (ROOT / directory).rglob('*'):
            relative = path.relative_to(ROOT)
            if path.is_file() and not any(
                part in ('dist', '__pycache__', 'node_modules', '.cache') for part in relative.parts
            ):
                paths.append(path)
    paths += [ROOT / name for name in ('CMakeLists.txt', 'prj.conf', 'west.yml')]
    for path in sorted(paths):
        digest.update(str(path.relative_to(ROOT)).encode() + b'\0')
        digest.update(hashlib.sha256(path.read_bytes()).digest())
    source_hash = digest.hexdigest()
    if tag:
        if not re.fullmatch(r'v[0-9]+\.[0-9]+\.[0-9]+(?:-rc\.[0-9]+)?', tag):
            raise ValueError('Invalid version tag')
        tagged = subprocess.check_output(
            ['git', '-C', str(ROOT), 'rev-parse', tag + '^{commit}'], text=True
        ).strip()
        if tagged != commit:
            raise ValueError('Release tag does not identify this commit')
        dirty = subprocess.check_output(
            ['git', '-C', str(ROOT), 'status', '--porcelain'], text=True
        )
        if dirty:
            raise ValueError('Release builds require a clean checkout')
        identity = tag + '@' + commit
        label = tag
    else:
        identity = 'dev-' + source_hash[:32]
        label = 'development'
    if len(identity) > 96:
        raise ValueError('Release identity exceeds protocol limit')
    return {'identity': identity, 'label': label, 'commit': commit, 'source_sha256': source_hash}


def write(path, content):
    path.parent.mkdir(parents=True, exist_ok=True)
    if not path.exists() or path.read_text() != content:
        path.write_text(content)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--tag')
    args = parser.parse_args()
    data = metadata(args.tag)
    write(args.output / 'release.json', json.dumps(data, indent=2) + '\n')
    write(
        args.output / 'release.mjs',
        '// SPDX-License-Identifier: GPL-3.0-or-later\nexport const release = '
        + json.dumps(data)
        + ';\n',
    )
    write(
        args.output / 'ht/release.hpp',
        '// SPDX-License-Identifier: GPL-3.0-or-later\n#pragma once\n#define HT_RELEASE_IDENTITY '
        + json.dumps(data['identity'])
        + '\n',
    )


if __name__ == '__main__':
    main()
