# SPDX-License-Identifier: GPL-3.0-or-later
"""Producer rejection checks and browser acceptance of the very same package."""
import argparse
import base64
import copy
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
import firmware_bundle
import patch_dsp_uart

NODE = shutil.which('node')
RELEASE = {
    'identity': 'dev-' + 'a' * 32,
    'label': 'development',
    'commit': 'b' * 40,
    'source_sha256': 'a' * 64,
}
CONFIG = (
    'CONFIG_BOARD_C62=y\nCONFIG_HT_COMPANION=y\nCONFIG_FLASH_SIZE=4096\n'
    'CONFIG_FLASH_BASE_ADDRESS=0x18000000\n'
)
DTS = '''flash0: flash@18000000 {
    reg = <0x18000000 0x400000>; erase-block-size = <0x1000>;
    partitions {
        slot0_partition: partition@0 { reg = <0 0x100000>; };
        dsp_firmware: partition@100000 { reg = <0x100000 0x100000>; };
        storage_partition: partition@300000 { reg = <0x300000 0x100000>; };
    };
};
'''


class BundleChecks(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.build = Path(self.directory.name)
        (self.build / 'zephyr').mkdir()
        (self.build / 'companion-generated').mkdir()
        (self.build / 'companion-generated/release.json').write_text(json.dumps(RELEASE))
        self.files = self.build / 'zephyr'
        (self.files / '.config').write_text(CONFIG)
        (self.files / 'zephyr.dts').write_text(DTS)
        self.app = (
            b'fixture application\0' + RELEASE['identity'].encode() + b'\0' + bytes(range(256))
        )
        (self.files / 'zephyr.bin').write_bytes(self.app)
        self.dsp = patch_dsp_uart.patch_image(
            (ROOT / 'backends/c62/resources/dsp_firmware.bin').read_bytes()
        )
        (self.files / 'dsp_firmware.bin').write_bytes(self.dsp)
        mock = patch.object(firmware_bundle.companion_release, 'metadata', return_value=RELEASE)
        self.metadata = mock.start()
        self.addCleanup(mock.stop)

    def test_pair_bytes_offsets_and_incremental_regeneration(self):
        output = firmware_bundle.write_bundle(self.build)
        before = output.read_bytes()
        bundle = json.loads(before)
        self.assertEqual(bundle['flash_size'], 0x400000)
        self.assertEqual(bundle['erase_size'], 4096)
        self.assertEqual(bundle['release'], RELEASE)
        for image, role, offset, data in zip(
            bundle['images'], ('application', 'dsp'), (0, 0x100000), (self.app, self.dsp)
        ):
            self.assertEqual(image['role'], role)
            self.assertEqual(image['offset'], offset)
            self.assertEqual(image['size'], len(data))
            self.assertEqual(base64.b64decode(image['data_base64'], validate=True), data)
        firmware_bundle.write_bundle(self.build)
        self.assertEqual(output.read_bytes(), before)
        output.unlink()
        firmware_bundle.write_bundle(self.build)
        self.assertEqual(output.read_bytes(), before)

    def test_stale_attempt_removes_previous_uploadable_package(self):
        output = firmware_bundle.write_bundle(self.build)
        changed = copy.deepcopy(RELEASE)
        changed['commit'] = 'c' * 40
        (self.build / 'companion-generated/release.json').write_text(json.dumps(changed))
        with self.assertRaisesRegex(ValueError, 'Stale'):
            firmware_bundle.write_bundle(self.build)
        self.assertFalse(output.exists())

    def test_configuration_and_effective_layout_guards(self):
        for line in CONFIG.splitlines():
            (self.files / '.config').write_text(CONFIG.replace(line, ''))
            with self.assertRaisesRegex(ValueError, 'C62'):
                firmware_bundle.build_bundle(self.build)
        (self.files / '.config').write_text(CONFIG)
        for old, new in (
            ('0x400000', '0x800000'),
            ('<0 0x100000>', '<0x1000 0x100000>'),
            ('<0x100000 0x100000>', '<0x200000 0x100000>'),
            ('0x300000 0x100000', '0x300000 0x80000'),
            ('0x1000>', '0x2000>'),
        ):
            (self.files / 'zephyr.dts').write_text(DTS.replace(old, new))
            with self.assertRaisesRegex(ValueError, 'layout|erase size'):
                firmware_bundle.build_bundle(self.build)

    def test_empty_oversized_unpaired_and_unpatched_images(self):
        for invalid in (b'', bytes(0x100001), b'old application\0'):
            (self.files / 'zephyr.bin').write_bytes(invalid)
            with self.assertRaises(ValueError):
                firmware_bundle.build_bundle(self.build)
        (self.files / 'zephyr.bin').write_bytes(self.app)
        for invalid in (
            self.dsp[:-1],
            b'wrong DSP',
            (ROOT / 'backends/c62/resources/dsp_firmware.bin').read_bytes(),
        ):
            (self.files / 'dsp_firmware.bin').write_bytes(invalid)
            with self.assertRaisesRegex(ValueError, 'DSP'):
                firmware_bundle.build_bundle(self.build)

    def test_tag_forwarding_and_browser_pairing(self):
        tagged = dict(RELEASE, label='v1.0.0-rc.1', identity='v1.0.0-rc.1@' + RELEASE['commit'])
        self.metadata.return_value = tagged
        (self.build / 'companion-generated/release.json').write_text(json.dumps(tagged))
        (self.files / 'zephyr.bin').write_bytes(tagged['identity'].encode() + b'\0' + bytes(256))
        output = firmware_bundle.write_bundle(self.build, 'v1.0.0-rc.1')
        self.metadata.assert_called_with('v1.0.0-rc.1')
        subprocess.run(
            [NODE, str(ROOT / 'tests/companion/firmware_bundle.mjs'), str(output)], check=True
        )


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--node', default=shutil.which('node'))
    args, unittest_args = parser.parse_known_args()
    NODE = args.node
    if not NODE:
        parser.error('Node.js required; pass --node /path/to/node')
    unittest.main(argv=[sys.argv[0]] + unittest_args)
