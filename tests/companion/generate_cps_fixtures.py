#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Freeze independent Python CPS v2 binary vectors for both applications."""
import argparse
import copy
import json
from pathlib import Path
import struct
import sys
import zlib

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
import codeplug
import codeplug_wire as wire


def fixtures():
    valid = []
    for path in sorted((ROOT / 'examples/codeplug').glob('*.json')):
        document = codeplug.load(path)
        valid.append(
            {'name': path.stem, 'document': document, 'wire': wire.encode(document, 1).hex()}
        )
    full = copy.deepcopy(valid[1]['document'])
    seed = copy.deepcopy(full['channels'][0])
    full['allocation'] = {'channel_id_high_water': 0xFFFFFFFF, 'bank_id_high_water': 16}
    full['channels'] = [
        dict(copy.deepcopy(seed), id=i, number=i, name='X' * 24) for i in range(1, 257)
    ]
    full['banks'] = [
        {'id': i, 'name': 'B' * 24, 'channel_ids': list(range(256, 0, -1))} for i in range(1, 17)
    ]
    full['selection'] = {'operating': 'memory', 'channel_id': 256, 'bank_id': 16}
    valid.append({'name': 'maximum', 'document': full, 'wire': wire.encode(full, 1).hex()})
    for theme in codeplug.THEMES[4:]:
        document = copy.deepcopy(valid[0]['document'])
        document['global']['ui']['theme'] = theme
        valid.append({'name': theme, 'document': document, 'wire': wire.encode(document, 1).hex()})
    for level in (0, 74, 127):
        document = copy.deepcopy(valid[0]['document'])
        document['global']['fm_ctcss_level'] = level
        valid.append({'name': f'ctcss-level-{level}', 'document': document,
                      'wire': wire.encode(document, 1).hex()})
    invalid = []
    seed = bytes.fromhex(valid[0]['wire'])

    def add(name, data):
        invalid.append({'name': name, 'wire': data.hex()})

    for offset in (0, 12, len(seed) - 1):
        data = bytearray(seed)
        data[offset] ^= 1
        add('corrupt ' + str(offset), data)
    for length in (0, 15, 16, len(seed) - 1):
        add('truncated ' + str(length), seed[:length])
    add('trailing byte', seed + b'\0')
    for name, offset, value in [
        ('unsupported record version', 4, 3),
        ('old manifest rejected on wire', 4, 1),
        ('invalid CTCSS level', len(seed) - 1, 128),
        ('unknown theme', 40, 99),
        ('unknown contrast', 41, 99),
        ('bad boolean', 58, 2),
        ('wrong generation', 8, 2),
        ('unknown mode', 55, 99),
        ('nonzero reserved', 82, 1),
    ]:
        data = bytearray(seed)
        data[offset] = value
        struct.pack_into('<I', data, 12, zlib.crc32(data[:12] + data[16:]))
        add(name, data)
    return {'valid': valid, 'invalid': invalid}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    path = ROOT / 'protocol/companion/cps-fixtures.json'
    contents = json.dumps(fixtures(), separators=(',', ':')) + '\n'
    if args.check:
        if path.read_text() != contents:
            raise SystemExit('Stale CPS fixtures; run tests/companion/generate_cps_fixtures.py')
    else:
        path.write_text(contents)


if __name__ == '__main__':
    main()
