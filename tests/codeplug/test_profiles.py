# SPDX-License-Identifier: GPL-3.0-or-later
import copy
import fcntl
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
import zlib

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
import codeplug
import codeplug_profile as profiles
import codeplug_wire as wire


class ProfileTests(unittest.TestCase):
    def example(self, name='fm-repeater'):
        return codeplug.load(ROOT / 'examples/codeplug' / (name + '.json'))

    def full(self):
        result = self.example('m17-directed')
        sample = result['channels'][0]
        result['channels'] = [
            dict(copy.deepcopy(sample), id=i, number=i, name='X' * 24) for i in range(1, 257)
        ]
        for i, channel in enumerate(result['channels']):
            channel['configuration']['m17']['can'] = i % 16
        result['allocation'] = dict(channel_id_high_water=1000, bank_id_high_water=20)
        result['banks'] = [
            dict(id=i, name='B' * 24, channel_ids=list(range(256, 0, -1))) for i in range(1, 17)
        ]
        result['selection'] = dict(operating='memory', channel_id=256, bank_id=16)
        return codeplug.validate(result)

    def test_wire_round_trips_examples_full_capacity_and_generation(self):
        for document in [
            self.example('empty'),
            self.example(),
            self.example('m17-directed'),
            self.full(),
        ]:
            for generation in (1, 12345, codeplug.MAX_ID):
                data = wire.encode(document, generation)
                self.assertLessEqual(len(data), wire.MAX_PROFILE)
                decoded, found = wire.decode(data)
                self.assertEqual(found, generation)
                self.assertEqual(codeplug.canonical(decoded), codeplug.canonical(document))
        with self.assertRaises(codeplug.InvalidCodeplug):
            wire.encode(self.example(), 0)

    def test_crc_versions_canonical_records_and_trailing_data_are_rejected(self):
        original = wire.encode(self.example(), 1)
        for data in (
            b'',
            original[:-1],
            original + b'X',
            b'X' + original[1:],
            original + b' ' * wire.MAX_PROFILE,
        ):
            with self.subTest(length=len(data)), self.assertRaises(codeplug.InvalidCodeplug):
                wire.decode(data)
        # Recompute the CRC to reach deeper validation rather than only its checksum guard.
        for position, value in ((4, 4), (5, 2), (8, 0), (16 + 26, 2), (16 + 25, 99), (16 + 30, 2)):
            data = bytearray(original)
            (length,) = struct.unpack_from('<H', data, 6)
            data[position] = value
            struct.pack_into('<I', data, 12, zlib.crc32(data[:12] + data[16:length]))
            if position == 16 + 25:  # Unknown contrast IDs have the firmware's documented fallback.
                self.assertEqual(wire.decode(data)[0]['global']['ui']['contrast'], 'normal')
            else:
                with self.subTest(position=position), self.assertRaises(codeplug.InvalidCodeplug):
                    wire.decode(data)

    def test_retired_binary_version_rejection_and_step_validation(self):
        original = wire.encode(self.example(), 1)
        length = struct.unpack_from('<H', original, 6)[0]
        header = bytearray(original[:12])
        header[4] = 2
        struct.pack_into('<H', header, 6, length - 4)
        payload = original[16 : length - 4]
        old = (
            bytes(header)
            + struct.pack('<I', zlib.crc32(header + payload))
            + payload
            + original[length:]
        )
        with self.assertRaisesRegex(codeplug.InvalidCodeplug, 'unsupported binary version'):
            wire.decode(old)
        decoded, generation = wire.decode(original)
        self.assertEqual(generation, 1)
        for step in (0, 1, 0xFFFFFFFF):
            damaged = bytearray(original)
            struct.pack_into('<I', damaged, length - 4, step)
            struct.pack_into('<I', damaged, 12, zlib.crc32(damaged[:12] + damaged[16:length]))
            with self.assertRaisesRegex(codeplug.InvalidCodeplug, 'vfo_step_hz'):
                wire.decode(damaged)
        decoded['global']['vfo_step_hz'] = 6250
        self.assertEqual(wire.decode(wire.encode(decoded, 2))[0]['global']['vfo_step_hz'], 6250)

    def test_binary_binding_string_reserved_field_and_membership_damage(self):
        original = wire.encode(self.example(), 1)
        channel_start = struct.unpack_from('<H', original, 6)[0]
        bank_start = channel_start + struct.unpack_from('<H', original, channel_start + 6)[0]
        config_start = channel_start + 16 + 31
        name_end = channel_start + 16 + 6 + len(self.example()['channels'][0]['name'])
        mutations = [
            (channel_start, channel_start + 5, b'\x03'),
            (channel_start, channel_start + 8, struct.pack('<I', 2)),
            (channel_start, channel_start + 16, struct.pack('<I', 99)),
            (channel_start, channel_start + 20, b'\0\0'),
            (channel_start, name_end + 1, b'X'),
            (channel_start, config_start + 16, b'\x03'),
            (channel_start, config_start + 17, b'\x02'),
            (channel_start, config_start + 22, struct.pack('<H', 0o1000)),
            (channel_start, config_start + 25, b'\x01'),
            (channel_start, config_start + 38, b'\x01'),
            (bank_start, bank_start + 16 + 29, struct.pack('<H', 257)),
            (bank_start, bank_start + 16 + 31, struct.pack('<I', 99)),
        ]
        for record, offset, value in mutations:
            data = bytearray(original)
            data[offset : offset + len(value)] = value
            length = struct.unpack_from('<H', data, record + 6)[0]
            crc = zlib.crc32(data[record : record + 12] + data[record + 16 : record + length])
            struct.pack_into('<I', data, record + 12, crc)
            with self.subTest(offset=offset), self.assertRaises(codeplug.InvalidCodeplug):
                wire.decode(data)

    def test_old_export_preserves_high_water_and_dry_run_has_no_profile_write(self):
        old = self.example()
        with tempfile.TemporaryDirectory() as directory, profiles.Profile(
            directory, 'test'
        ) as profile:
            current = copy.deepcopy(old)
            current['allocation'] = dict(channel_id_high_water=4000, bank_id_high_water=3000)
            profiles.replace(profile, current)
            original = profile.path.read_bytes()
            restored, generation = profiles.replace(profile, old, True)
            self.assertEqual(profile.path.read_bytes(), original)
            self.assertEqual(generation, 2)
            self.assertEqual(restored['allocation'], current['allocation'])
            profiles.replace(profile, old)
            restored, generation = profile.load()
            self.assertEqual(generation, 2)
            self.assertEqual(restored['allocation'], current['allocation'])
            self.assertEqual(old, self.example())  # No mutation of caller data.
            profile.path.write_bytes(wire.encode(current, codeplug.MAX_ID))
            with self.assertRaises(codeplug.InvalidCodeplug):
                profiles.replace(profile, old)

    def test_profile_lock_is_the_firmware_lock_and_other_profiles_stay_independent(self):
        with tempfile.TemporaryDirectory() as directory, profiles.Profile(
            directory, 'test'
        ) as profile:
            self.assertEqual(profile.lock_path.name, 'test.bin.lock')
            with self.assertRaises(BlockingIOError), profiles.Profile(directory, 'test'):
                pass
            with profiles.Profile(directory, 'other') as other:
                profiles.replace(other, self.example())
            self.assertFalse(profile.path.exists())
            fd = os.open(profile.lock_path, os.O_RDWR)
            try:
                with self.assertRaises(BlockingIOError):
                    fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
            finally:
                os.close(fd)

    def test_backend_validation_paths_output_aliases_and_unlocked_writes(self):
        for name in ('', '../test', 'X' * 33, 'test.bin', 'ä'):
            with self.assertRaises(codeplug.InvalidCodeplug):
                profiles.Profile('/tmp', name)
        bad = self.example()
        bad['global']['gain'] = 1
        with tempfile.TemporaryDirectory() as directory, profiles.Profile(
            directory, 'test'
        ) as profile:
            with self.assertRaisesRegex(codeplug.InvalidCodeplug, 'global.gain'):
                profiles.replace(profile, bad)
            self.assertFalse(profile.path.exists())
            profiles.replace(profile, self.example())
            alias = Path(directory) / 'alias'
            alias.symlink_to(profile.path)
            hardlink = Path(directory) / 'hardlink'
            os.link(profile.path, hardlink)
            with profiles.Profile(directory, 'other') as other:
                profiles.replace(other, self.example())
                other_alias = Path(directory) / 'other-alias'
                os.link(other.path, other_alias)
            for output in (
                profile.path,
                profile.lock_path,
                alias,
                hardlink,
                other.path,
                other_alias,
                Path(directory) / 'future.bin',
            ):
                with self.assertRaises(codeplug.InvalidCodeplug):
                    profile.protect_output(output)
        with self.assertRaises(codeplug.InvalidCodeplug):
            profiles.replace(profiles.Profile('/tmp', 'test'), self.example())

    def test_atomic_failure_preserves_old_file_and_late_failure_reports_visible_publication(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'profile'
            path.write_bytes(b'old')
            for operation in ('fsync', 'replace'):
                with patch.object(profiles.os, operation, side_effect=OSError('injected')):
                    with self.assertRaises(OSError):
                        profiles.atomic_write(path, b'new')
                self.assertEqual(path.read_bytes(), b'old')
                self.assertEqual(list(Path(directory).glob('*.tmp.*')), [])
            with patch.object(profiles.os, 'fsync', side_effect=[None, OSError('directory')]):
                with self.assertRaisesRegex(OSError, 'replacement is visible'):
                    profiles.atomic_write(path, b'new')
            self.assertEqual(path.read_bytes(), b'new')

    def test_cli_requires_explicit_replacement_and_exports_deterministically(self):
        with tempfile.TemporaryDirectory() as directory:
            profile_dir = Path(directory) / 'profiles'
            output = Path(directory) / 'export.json'
            base = [sys.executable, str(ROOT / 'tools/codeplug_profile.py')]
            source = str(ROOT / 'examples/codeplug/fm-repeater.json')
            common = ['--profile', 'test', '--directory', str(profile_dir)]
            command = base + ['import', source] + common
            self.assertEqual(subprocess.run(command, capture_output=True).returncode, 2)
            self.assertFalse(profile_dir.exists())
            self.assertEqual(
                subprocess.run(command + ['--dry-run'], capture_output=True).returncode, 0
            )
            self.assertFalse((profile_dir / 'test.bin').exists())
            self.assertEqual(
                subprocess.run(command + ['--replace'], capture_output=True).returncode, 0
            )
            export = base + ['export'] + common + ['--output', str(output)]
            self.assertEqual(subprocess.run(export, capture_output=True).returncode, 0)
            self.assertEqual(output.read_bytes(), codeplug.canonical(self.example()))
            with profiles.Profile(str(profile_dir), 'test'):
                self.assertEqual(subprocess.run(export, capture_output=True).returncode, 1)

    @unittest.skipUnless(
        os.getenv('HT_CODEPLUG_PROBE'), 'build/set HT_CODEPLUG_PROBE for firmware interoperability'
    )
    def test_full_capacity_profile_read_write_by_actual_firmware(self):
        with tempfile.TemporaryDirectory() as directory:
            document = self.full()
            with profiles.Profile(directory, 'test') as profile:
                profiles.replace(profile, document)
            env = dict(
                os.environ,
                HT_PROFILE_PROBE='1',
                HT_PROFILE_PROBE_WRITE='1',
                HT_SETTINGS_DIR=directory,
                HT_PROFILE='test',
            )
            run = subprocess.run(
                [os.environ['HT_CODEPLUG_PROBE'], '-no-rt'],
                env=env,
                capture_output=True,
                text=True,
                timeout=30,
            )
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertIn('PROJECT EXECUTION SUCCESSFUL', run.stdout)
            self.assertIn('channels=256 banks=16', run.stdout)
            self.assertIn('mode=1 can=15', run.stdout)
            self.assertIn('PROFILE: published=2', run.stdout)
            with profiles.Profile(directory, 'test') as profile:
                restored, generation = profile.load()
            self.assertEqual(generation, 2)
            document['global']['ui']['theme'] = 'nord'
            document['global']['ui']['contrast'] = 'maximum'
            document['global']['vfo_step_hz'] = 6250
            document['vfo']['rx_frequency_hz'] = 145555000
            document['vfo']['tx_frequency_hz'] = 145555010
            self.assertEqual(codeplug.canonical(restored), codeplug.canonical(document))


if __name__ == '__main__':
    unittest.main()
