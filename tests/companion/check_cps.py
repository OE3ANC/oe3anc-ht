#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Run browser JSON acceptance/canonicalization against the existing Python contract."""
import argparse
import copy
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
import codeplug


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--node', default=shutil.which('node'))
    args = parser.parse_args()
    if not args.node:
        parser.error('Node.js required; pass --node /path/to/node')
    cases = []
    with tempfile.TemporaryDirectory() as directory:
        directory = Path(directory)
        input_path = directory / 'input.json'

        def add(name, text):
            input_path.write_text(text)
            try:
                expected = codeplug.canonical(codeplug.load(input_path)).decode()
            except codeplug.InvalidCodeplug:
                expected = None
            cases.append({'name': name, 'text': text, 'expected': expected})

        for file in sorted((ROOT / 'examples/codeplug').glob('*.json')):
            add(file.name, file.read_text())
        seed = codeplug.load(ROOT / 'examples/codeplug/fm-repeater.json')
        for version in (1, 2):
            document = copy.deepcopy(seed)
            document['schema_version'] = version
            add('schema ' + str(version), json.dumps(document))
        mutations = [
            ('duplicate ID', lambda d: d['channels'].append(copy.deepcopy(d['channels'][0]))),
            (
                'duplicate number',
                lambda d: d['channels'].append(dict(copy.deepcopy(d['channels'][0]), id=16)),
            ),
            ('low water', lambda d: d['allocation'].update(channel_id_high_water=0)),
            (
                'duplicate member',
                lambda d: d['banks'][0]['channel_ids'].append(d['channels'][0]['id']),
            ),
            ('missing member', lambda d: d['banks'][0]['channel_ids'].append(99)),
            ('missing selection', lambda d: d['selection'].update(channel_id=None)),
            ('selected outside bank', lambda d: d['banks'][0].update(channel_ids=[])),
            ('unknown global', lambda d: d['global'].update(registers={})),
            ('bad local call', lambda d: d['global'].update(local_callsign='ALL')),
            ('newline local call', lambda d: d['global'].update(local_callsign='OE3ANC\n')),
            ('unicode name', lambda d: d['channels'][0].update(name='\u00f6')),
            ('newline name', lambda d: d['channels'][0].update(name='ABC\n')),
            ('long name', lambda d: d['channels'][0].update(name='X' * 25)),
            (
                'integer DCS',
                lambda d: d['channels'][0]['configuration']['fm']['tx_tone'].update(code=23),
            ),
            (
                'newline DCS',
                lambda d: d['channels'][0]['configuration']['fm']['tx_tone'].update(code='023\n'),
            ),
            (
                'bad DCS',
                lambda d: d['channels'][0]['configuration']['fm']['tx_tone'].update(code='089'),
            ),
            (
                'bad CTCSS',
                lambda d: d['channels'][0]['configuration']['fm']['rx_tone'].update(tenths_hz=100),
            ),
            ('bool integer', lambda d: d['global'].update(gain=True)),
            ('unsupported gain offline valid', lambda d: d['global'].update(gain=15)),
            ('bad step', lambda d: d['global'].update(vfo_step_hz=8333)),
            ('bool step', lambda d: d['global'].update(vfo_step_hz=True)),
            ('future version', lambda d: d.update(schema_version=3)),
            ('frequency gap', lambda d: d['vfo'].update(rx_frequency_hz=300000000)),
            ('inhibit invalid TX', lambda d: d['vfo'].update(tx_frequency_hz=0, tx_inhibit=True)),
            ('mode without fields', lambda d: d['vfo'].update(mode='m17')),
        ]
        for name, mutate in mutations:
            document = copy.deepcopy(seed)
            mutate(document)
            add(name, json.dumps(document))
        base = codeplug.canonical(seed).decode()
        for name, text in [
            ('duplicate key', base.replace('"gain":0', '"gain":0,"gain":0')),
            ('escaped duplicate', base.replace('"gain":0', '"gain":0,"g\\u0061in":0')),
            ('float token', base.replace('"gain":0', '"gain":0.0')),
            ('exponent token', base.replace('"gain":0', '"gain":0e0')),
            ('nonfinite', base.replace('"gain":0', '"gain":NaN')),
            ('trailing data', base + 'null'),
            ('trailing comma', '{"x":1,}'),
            ('leading zero', '{"x":01}'),
            ('deep nesting', '[' * 100 + '0' + ']' * 100),
            ('too large', ' ' * codeplug.MAX_BYTES + base),
            ('UTF-8 BOM', '\ufeff' + base),
            ('negative zero', base.replace('"gain":0', '"gain":-0')),
        ]:
            add(name, text)
        full = copy.deepcopy(seed)
        full['allocation'] = {'channel_id_high_water': codeplug.MAX_ID, 'bank_id_high_water': 16}
        full['channels'] = [
            dict(copy.deepcopy(seed['channels'][0]), id=i, number=i, name='X' * 24)
            for i in range(1, 257)
        ]
        full['banks'] = [
            {'id': i, 'name': 'B' * 24, 'channel_ids': list(range(256, 0, -1))}
            for i in range(1, 17)
        ]
        full['selection'] = {'operating': 'memory', 'channel_id': 1, 'bank_id': 1}
        add('maximum capacity', json.dumps(full, indent=2))
        full['channels'].reverse()
        full['banks'].reverse()
        add('record order', json.dumps(full))
        full['channels'].append(copy.deepcopy(seed['channels'][0]))
        add('channel overcapacity', json.dumps(full))
        cases_path = directory / 'cases.json'
        cases_path.write_text(json.dumps(cases))
        subprocess.run(
            [args.node, str(ROOT / 'tests/companion/codeplug.mjs'), str(cases_path)], check=True
        )
        subprocess.run([args.node, str(ROOT / 'tests/companion/cps_ui.mjs')], check=True)


if __name__ == '__main__':
    main()
