#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Run both real codecs against the same frozen wire fixtures and session checks."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--node', default=shutil.which('node'), help='Node.js executable')
    args = parser.parse_args()
    if not args.node:
        parser.error('Node.js is required; pass --node /path/to/node')
    subprocess.run(
        [sys.executable, str(ROOT / 'tools/companion_contract.py'), '--check'], check=True
    )
    subprocess.run(
        [sys.executable, str(ROOT / 'tests/companion/generate_cps_fixtures.py'), '--check'],
        check=True,
    )
    subprocess.run(
        [sys.executable, str(ROOT / 'tests/companion/generate_ui_fixtures.py'), '--check'],
        check=True,
    )
    fixtures = json.loads((ROOT / 'protocol/companion/fixtures.json').read_text())
    with tempfile.TemporaryDirectory() as directory:
        directory = Path(directory)
        lines = [
            '// Generated temporarily from shared frozen fixtures.',
            'static void fixtures() {',
        ]
        for fixture in fixtures:
            frame = fixture['frame']
            wire = bytes.fromhex(fixture['wire'])
            payload = bytes.fromhex(frame['payload'])
            lines.append('{ Frame f;')
            for field in ('major', 'minor', 'type', 'flags', 'request'):
                lines.append(f'f.{field} = {frame[field]};')
            lines.append(f'f.session = UINT64_C({frame["session"]});')
            lines.append(f'f.size = {len(payload)};')
            for i, byte in enumerate(payload):
                lines.append(f'f.payload[{i}] = {byte};')
            lines += [
                'const uint8_t expected[] = {' + ','.join(map(str, wire)) + '};',
                'check_wire(f, expected, sizeof(expected)); }',
            ]
        lines.append('}')
        (directory / 'fixtures.hpp').write_text('\n'.join(lines) + '\n')
        executable = directory / 'protocol-test'
        subprocess.run(
            [
                'g++',
                '-std=c++14',
                '-Wall',
                '-Wextra',
                '-Werror',
                '-fno-exceptions',
                '-fno-rtti',
                '-I' + str(ROOT / 'include'),
                '-I' + str(directory),
                str(ROOT / 'tests/companion/protocol.cpp'),
                str(ROOT / 'modules/companion/protocol.cpp'),
                '-o',
                str(executable),
            ],
            check=True,
        )
        subprocess.run([str(executable)], check=True)
        executable = directory / 'ui-transfer-test'
        subprocess.run(
            [
                'g++',
                '-std=c++14',
                '-Wall',
                '-Wextra',
                '-Werror',
                '-fno-exceptions',
                '-fno-rtti',
                '-I' + str(ROOT / 'include'),
                '-I' + str(ROOT / 'tests/companion/fakes'),
                str(ROOT / 'tests/companion/ui-transfer.cpp'),
                str(ROOT / 'modules/companion/ui.cpp'),
                str(ROOT / 'modules/companion/protocol.cpp'),
                '-o',
                str(executable),
            ],
            check=True,
        )
        subprocess.run([str(executable)], check=True)
        keys = json.loads((ROOT / 'protocol/companion/key-fixtures.json').read_text())
        lines = ['static void fixtures() {']
        for item in keys:
            data = bytes.fromhex(item['payload'])
            lines.append('{ const uint8_t bytes[] = {' + (','.join(map(str, data)) or '0') + '};')
            lines.append(f'fixture({item["type"]}, bytes, {len(data)}, {item["status"]}); }}')
        lines.append('}')
        (directory / 'key-fixtures.hpp').write_text('\n'.join(lines) + '\n')
        executable = directory / 'keys-test'
        subprocess.run(
            [
                'g++',
                '-std=c++14',
                '-Wall',
                '-Wextra',
                '-Werror',
                '-fno-exceptions',
                '-fno-rtti',
                '-I' + str(ROOT / 'include'),
                '-I' + str(ROOT / 'tests/companion/fakes'),
                '-I' + str(directory),
                str(ROOT / 'tests/companion/keys.cpp'),
                str(ROOT / 'modules/companion/keys.cpp'),
                '-o',
                str(executable),
            ],
            check=True,
        )
        subprocess.run([str(executable)], check=True)
        ptt = json.loads((ROOT / 'protocol/companion/ptt-fixtures.json').read_text())
        lines = ['static void fixtures() {']
        for item in ptt:
            data = bytes.fromhex(item['payload'])
            lines.append('{ const uint8_t bytes[] = {' + (','.join(map(str, data)) or '0') + '};')
            lines.append(f'fixture({item["type"]}, bytes, {len(data)}, {item["status"]}); }}')
        lines.append('}')
        (directory / 'ptt-fixtures.hpp').write_text('\n'.join(lines) + '\n')
        executable = directory / 'ptt-test'
        subprocess.run(
            [
                'g++',
                '-std=c++14',
                '-Wall',
                '-Wextra',
                '-Werror',
                '-fno-exceptions',
                '-fno-rtti',
                '-I' + str(ROOT / 'include'),
                '-I' + str(ROOT / 'tests/companion/fakes'),
                '-I' + str(directory),
                str(ROOT / 'tests/companion/ptt.cpp'),
                str(ROOT / 'modules/companion/ptt.cpp'),
                '-o',
                str(executable),
            ],
            check=True,
        )
        subprocess.run([str(executable)], check=True)
        cps = json.loads((ROOT / 'protocol/companion/cps-fixtures.json').read_text())
        lines = ['static void fixtures() {']
        for kind in ('valid', 'invalid'):
            for item in cps[kind]:
                values = ','.join(str(byte) for byte in bytes.fromhex(item['wire'])) or '0'
                lines.append('{ static const uint8_t bytes[] = {' + values + '};')
                lines.append(f'{kind}(bytes, {len(bytes.fromhex(item["wire"]))}); }}')
        lines.append('}')
        (directory / 'cps-fixtures.hpp').write_text('\n'.join(lines) + '\n')
        executable = directory / 'cps-codec-test'
        subprocess.run(
            [
                'g++',
                '-std=c++14',
                '-Wall',
                '-Wextra',
                '-Werror',
                '-fno-exceptions',
                '-fno-rtti',
                '-I' + str(ROOT / 'include'),
                '-I' + str(ROOT / 'tests/companion/fakes'),
                '-I' + str(directory),
                str(ROOT / 'tests/companion/codeplug-wire.cpp'),
                str(ROOT / 'modules/companion/codeplug.cpp'),
                str(ROOT / 'modules/channels/model.cpp'),
                str(ROOT / 'modules/channels/records.cpp'),
                '-o',
                str(executable),
            ],
            check=True,
        )
        subprocess.run([str(executable)], check=True)
        ui = json.loads((ROOT / 'protocol/companion/ui-fixtures.json').read_text())
        lines = ['static void fixtures() {']
        for kind in ('valid', 'invalid'):
            for item in ui[kind]:
                data = bytes.fromhex(item['wire'])
                lines.append('{ static const uint8_t bytes[] = {' + ','.join(map(str, data)) + '};')
                lines.append(f'{kind}(bytes, {len(data)}); }}')
        lines.append('}')
        (directory / 'ui-fixtures.hpp').write_text('\n'.join(lines) + '\n')
        executable = directory / 'ui-codec-test'
        subprocess.run(
            [
                'g++',
                '-std=c++14',
                '-Wall',
                '-Wextra',
                '-Werror',
                '-fno-exceptions',
                '-fno-rtti',
                '-I' + str(ROOT / 'include'),
                '-I' + str(directory),
                str(ROOT / 'tests/companion/presentation-wire.cpp'),
                str(ROOT / 'modules/ui/presentation_wire.cpp'),
                '-o',
                str(executable),
            ],
            check=True,
        )
        subprocess.run([str(executable)], check=True)
    subprocess.run([args.node, str(ROOT / 'tests/companion/protocol.mjs')], check=True)
    subprocess.run([args.node, str(ROOT / 'tests/companion/serial.mjs')], check=True)
    subprocess.run([args.node, str(ROOT / 'tests/companion/codeplug-wire.mjs')], check=True)
    subprocess.run([args.node, str(ROOT / 'tests/companion/cps-transfer.mjs')], check=True)
    subprocess.run([args.node, str(ROOT / 'tests/companion/presentation-wire.mjs')], check=True)
    subprocess.run([args.node, str(ROOT / 'tests/companion/ui-transfer.mjs')], check=True)
    subprocess.run([args.node, str(ROOT / 'tests/companion/keys.mjs')], check=True)
    subprocess.run([args.node, str(ROOT / 'tests/companion/ptt.mjs')], check=True)


if __name__ == '__main__':
    main()
