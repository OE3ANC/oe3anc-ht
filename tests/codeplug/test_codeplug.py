# SPDX-License-Identifier: GPL-3.0-or-later
import copy
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

try:
    import jsonschema
except ImportError:
    jsonschema = None  # Optional developer check; CLI/runtime use only stdlib.

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('codeplug', ROOT / 'tools/codeplug.py')
codeplug = importlib.util.module_from_spec(spec)
spec.loader.exec_module(codeplug)


class CodeplugTests(unittest.TestCase):
    def example(self, name='fm-repeater'):
        return codeplug.load(ROOT / 'examples/codeplug' / (name + '.json'))

    def invalid(self, document, path):
        with self.assertRaisesRegex(codeplug.InvalidCodeplug, path):
            codeplug.validate(document)

    def test_examples_and_deterministic_round_trip(self):
        for path in sorted((ROOT / 'examples/codeplug').glob('*.json')):
            original = codeplug.load(path)
            before = copy.deepcopy(original)
            encoded = codeplug.canonical(original)
            self.assertEqual(original, before)
            self.assertEqual(codeplug.canonical(json.loads(encoded)), encoded)
            self.assertEqual(json.loads(encoded), original)
        schema = json.loads((ROOT / 'schemas/codeplug-v1.schema.json').read_text())
        self.assertEqual(schema['properties']['schema_version']['const'], codeplug.SCHEMA_VERSION)

    @unittest.skipIf(jsonschema is None, 'optional JSON Schema developer dependency unavailable')
    def test_published_schema_and_lexical_integer_boundary(self):
        schema = json.loads((ROOT / 'schemas/codeplug-v1.schema.json').read_text())
        jsonschema.Draft202012Validator.check_schema(schema)
        validator = jsonschema.Draft202012Validator(schema)
        for file in (ROOT / 'examples/codeplug').glob('*.json'):
            document = codeplug.load(file)
            validator.validate(document)
            document['schema_version'] = True
            self.assertTrue(list(validator.iter_errors(document)))
            document['schema_version'] = 1.0
            # JSON Schema counts integral numeric values as integers; our wire
            # contract additionally requires actual integer JSON tokens.
            validator.validate(document)
            self.invalid(document, 'schema_version')
            document['schema_version'] = 1
            document['channels'] = [
                {'id': 1, 'number': 1, 'name': 'ABC\n', 'configuration': document['vfo']}
            ]
            self.assertTrue(list(validator.iter_errors(document)))

    def test_current_schema_and_step_validation(self):
        original = self.example()
        for step in codeplug.VFO_STEPS_HZ:
            original['global']['vfo_step_hz'] = step
            codeplug.validate(original)
        for step in (0, 1, 8333, 12500.0, True, '12500'):
            original['global']['vfo_step_hz'] = step
            self.invalid(original, 'vfo_step_hz')
        del original['global']['vfo_step_hz']
        self.invalid(original, 'missing fields')
        original = self.example()
        for version in (0, 2, 3):
            original['schema_version'] = version
            self.invalid(original, 'schema_version')

    def test_full_store_and_membership_order(self):
        full = self.example()
        sample = full['channels'][0]
        full['allocation'] = {'channel_id_high_water': codeplug.MAX_ID, 'bank_id_high_water': 16}
        full['channels'] = [
            dict(copy.deepcopy(sample), id=i, number=i, name='X' * 24) for i in range(1, 257)
        ]
        full['banks'] = [
            {'id': i, 'name': 'B' * 24, 'channel_ids': list(range(256, 0, -1))}
            for i in range(1, 17)
        ]
        full['selection'] = {'operating': 'memory', 'channel_id': 1, 'bank_id': 1}
        canonical = codeplug.canonical(full)
        self.assertLess(len(canonical), codeplug.MAX_BYTES)
        self.assertLess(len(json.dumps(full, indent=2).encode()), codeplug.MAX_BYTES)
        self.assertEqual(json.loads(canonical)['banks'][0]['channel_ids'], list(range(256, 0, -1)))
        full['channels'].reverse()
        full['banks'].reverse()
        self.assertEqual(codeplug.canonical(full), canonical)
        full['channels'].append(copy.deepcopy(sample))
        self.invalid(full, r'\$\.channels')

    def test_capacity_identity_and_reference_failures(self):
        changes = [
            (lambda d: d['channels'].append(copy.deepcopy(d['channels'][0])), 'duplicate'),
            (lambda d: d['allocation'].update(channel_id_high_water=16), r'channels\[0\]\.id'),
            (lambda d: d['allocation'].update(bank_id_high_water=1), r'banks\[0\]\.id'),
            (lambda d: d['banks'][0]['channel_ids'].append(17), 'duplicate channel reference'),
            (lambda d: d['banks'][0]['channel_ids'].append(99), 'missing or duplicate'),
            (lambda d: d['banks'].extend([copy.deepcopy(d['banks'][0])] * 16), r'\$\.banks'),
            (lambda d: d['selection'].update(channel_id=None), 'requires a selected'),
            (lambda d: d['selection'].update(bank_id=99), 'missing object reference'),
            (lambda d: d['banks'][0].update(channel_ids=[]), 'outside the selected bank'),
            (lambda d: d['selection'].update(channel_id=True), 'expected integer'),
        ]
        for change, reason in changes:
            with self.subTest(reason=reason):
                document = self.example()
                change(document)
                self.invalid(document, reason)

    def test_strict_fields_types_units_and_modes(self):
        changes = [
            (lambda d: d.update(schema_version=3), 'schema_version'),
            (lambda d: d.update(schema_version=True), 'schema_version'),
            (lambda d: d['global'].update(registers={}), 'unknown fields'),
            (lambda d: d['global'].update(local_callsign='ALL'), 'local_callsign'),
            (lambda d: d['global'].update(local_callsign='OE3ANC\n'), 'local_callsign'),
            (lambda d: d['global'].update(gain=16), 'gain'),
            (lambda d: d['global'].update(transmit_limit_s=1), 'transmit_limit_s'),
            (lambda d: d['global']['ui']['backlight'].update(idle_s=15.0), 'idle_s'),
            (lambda d: d['vfo'].update(rx_frequency_hz=300000000), 'supported bands'),
            (lambda d: d['vfo'].update(tx_frequency_hz=0, tx_inhibit=True), 'tx_frequency_hz'),
            (lambda d: d['vfo'].update(power_mw=True), 'power_mw'),
            (lambda d: d['vfo'].update(power_mw=5001), 'power_mw'),
            (lambda d: d['vfo'].update(mode='m17'), 'missing fields'),
            (lambda d: d['channels'][0].update(name='X' * 25), 'printable ASCII'),
            (lambda d: d['channels'][0].update(name='ABC\n'), 'printable ASCII'),
            (lambda d: d['channels'][0].update(name='\u00f6'), 'printable ASCII'),
            (
                lambda d: d['channels'][0]['configuration']['fm']['rx_tone'].update(tenths_hz=100),
                'tenths_hz',
            ),
            (
                lambda d: d['channels'][0]['configuration']['fm']['tx_tone'].update(code=23),
                'three octal',
            ),
            (
                lambda d: d['channels'][0]['configuration']['fm']['tx_tone'].update(code='089'),
                'three octal',
            ),
            (
                lambda d: d['channels'][0]['configuration']['fm']['tx_tone'].update(
                    polarity='reverse'
                ),
                'polarity',
            ),
        ]
        for change, reason in changes:
            with self.subTest(reason=reason):
                document = self.example()
                change(document)
                self.invalid(document, reason)

    def test_m17_explicit_destination_and_can(self):
        document = self.example('m17-directed')
        settings = document['channels'][0]['configuration']['m17']
        for can in (0, 15):
            settings['can'] = can
            codeplug.validate(document)
        settings['can'] = 16
        self.invalid(document, 'can')
        settings['can'] = 0
        settings['destination'] = {'kind': 'broadcast'}
        codeplug.validate(document)
        settings['destination']['callsign'] = 'ALL'
        self.invalid(document, 'unknown fields')
        settings['destination'] = {'kind': 'station', 'callsign': ''}
        self.invalid(document, 'callsign')

    def test_untrusted_json_is_bounded_and_no_duplicates_or_nonfinite_numbers(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'input.json'
            for value in (
                b'{"x":1,"x":2}',
                b'{"x":NaN}',
                b'{"x":Infinity}',
                b'\xff',
                b'[' * 1200 + b']' * 1200,
                b' ' * codeplug.MAX_BYTES + b'{}',
            ):
                path.write_bytes(value)
                with self.subTest(value=value[:20]), self.assertRaises(codeplug.InvalidCodeplug):
                    codeplug.load(path)


if __name__ == '__main__':
    unittest.main()
