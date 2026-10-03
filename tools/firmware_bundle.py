#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Package the current C62 application and UART-silent DSP as one static bundle."""
import argparse
import base64
import hashlib
import json
from pathlib import Path
import re

import companion_release

FORMAT = 'oe3anc-ht-firmware'
FLASH_SIZE = 0x400000
PARTITION_SIZE = 0x100000
DSP_SHA256 = 'fc0e2d7a8a2a991c831daf798a965743a178a582c6cbab9f9ef6a05c34628bdb'
MAX_BYTES = 3 * 1024 * 1024


def build_bundle(build_dir, tag=None):
    build_dir = Path(build_dir)
    expected = companion_release.metadata(tag)
    release = json.loads((build_dir / 'companion-generated/release.json').read_text())
    if release != expected:
        raise ValueError('Stale release metadata: rebuild C62 from the current source snapshot')
    config = (build_dir / 'zephyr/.config').read_text().splitlines()
    if not all(
        line in config
        for line in (
            'CONFIG_BOARD_C62=y',
            'CONFIG_HT_COMPANION=y',
            'CONFIG_FLASH_SIZE=4096',
            'CONFIG_FLASH_BASE_ADDRESS=0x18000000',
        )
    ):
        raise ValueError('Bundle requires a companion-enabled C62 build with 4 MiB flash')
    # Check the effective build layout, including overlays, rather than relying
    # on the board source alone. This accepts only schema 1's known layout.
    dts = (build_dir / 'zephyr/zephyr.dts').read_text()
    for label, values in (
        ('flash0', (0x18000000, FLASH_SIZE)),
        ('slot0_partition', (0, PARTITION_SIZE)),
        ('dsp_firmware', (PARTITION_SIZE, PARTITION_SIZE)),
        ('storage_partition', (0x300000, PARTITION_SIZE)),
    ):
        found = re.search(r'\b' + label + r':[^{}]+\{[^{}]*?\breg\s*=\s*<([^>]+)>', dts)
        if not found or tuple(int(cell, 0) for cell in found[1].split()) != values:
            raise ValueError('Unsupported effective flash layout: ' + label)
    flash = re.search(r'\bflash0:[^{}]+\{([^{}]*)', dts)
    erase = re.search(r'\berase-block-size\s*=\s*<\s*(0x[0-9a-fA-F]+|[0-9]+)\s*>', flash[1])
    if not erase or int(erase[1], 0) != 4096:
        raise ValueError('Unsupported effective flash erase size')
    images = []
    for role, filename, offset in (
        ('application', 'zephyr.bin', 0),
        ('dsp', 'dsp_firmware.bin', PARTITION_SIZE),
    ):
        data = (build_dir / 'zephyr' / filename).read_bytes()
        if not 0 < len(data) <= PARTITION_SIZE:
            raise ValueError(role + ' image is empty or exceeds its 1 MiB partition')
        digest = hashlib.sha256(data).hexdigest()
        if role == 'dsp' and digest != DSP_SHA256:
            raise ValueError('DSP does not match the pinned UART-silent host pairing')
        if role == 'application' and release['identity'].encode('ascii') + b'\0' not in data:
            raise ValueError('Application does not contain the current paired release identity')
        images.append(
            {
                'role': role,
                'offset': offset,
                'size': len(data),
                'sha256': digest,
                'data_base64': base64.b64encode(data).decode('ascii'),
            }
        )
    return {
        'format': FORMAT,
        'schema_version': 1,
        'target': 'c62',
        'flash_size': FLASH_SIZE,
        'erase_size': 4096,
        'release': release,
        'images': images,
    }


def write_bundle(build_dir, tag=None):
    output = Path(build_dir) / 'zephyr/firmware-bundle.json'
    # Remove an older package before validation so a failed attempt cannot leave
    # an apparently current package available for upload.
    output.unlink(missing_ok=True)
    data = (json.dumps(build_bundle(build_dir, tag), separators=(',', ':')) + '\n').encode()
    if len(data) > MAX_BYTES:
        raise ValueError('Firmware bundle exceeds schema 1 size limit')
    temporary = output.with_suffix('.json.tmp')
    try:
        temporary.write_bytes(data)
        temporary.replace(output)
    finally:
        temporary.unlink(missing_ok=True)
    return output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, required=True)
    parser.add_argument('--tag')
    args = parser.parse_args()
    print('Firmware bundle: ' + str(write_bundle(args.build_dir, args.tag)))


if __name__ == '__main__':
    main()
