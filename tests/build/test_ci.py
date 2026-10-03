# SPDX-License-Identifier: GPL-3.0-or-later
"""Offline checks for CI bootstrap pin and cache validation."""
import hashlib
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
import bootstrap_ci


class BootstrapChecks(unittest.TestCase):
    def test_checkout_rejects_unpinned_sources_and_preserves_existing_files(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'dependency'
            for url, revision in (
                ('http://example.invalid/repo', 'a' * 40),
                ('https://example.invalid/repo', 'main'),
            ):
                with self.assertRaises(ValueError):
                    bootstrap_ci.checkout(path, url, revision)
                self.assertFalse(path.exists())
            path.mkdir()
            source = path / 'source'
            source.write_text('keep')
            with self.assertRaises(ValueError):
                bootstrap_ci.checkout(path, 'https://example.invalid/repo', 'a' * 40)
            self.assertEqual(source.read_text(), 'keep')
            subprocess.run(['git', 'init', '-q', str(path)], check=True)
            subprocess.run(['git', '-C', str(path), 'add', 'source'], check=True)
            subprocess.run(
                [
                    'git',
                    '-C',
                    str(path),
                    '-c',
                    'user.name=Test',
                    '-c',
                    'user.email=test@example.invalid',
                    'commit',
                    '-qm',
                    'fixture',
                ],
                check=True,
            )
            revision = subprocess.check_output(
                ['git', '-C', str(path), 'rev-parse', 'HEAD'], text=True
            ).strip()
            bootstrap_ci.checkout(path, 'https://example.invalid/repo', revision)
            source.write_text('modified')
            with self.assertRaises(RuntimeError):
                bootstrap_ci.checkout(path, 'https://example.invalid/repo', revision)
            self.assertEqual(source.read_text(), 'modified')

    def test_cached_archive_requires_exact_hash(self):
        with tempfile.TemporaryDirectory() as directory:
            archive = Path(directory) / 'sdk.tar.xz'
            archive.write_bytes(b'cached SDK')
            digest = hashlib.sha256(archive.read_bytes()).hexdigest()
            bootstrap_ci.download(archive, 'https://example.invalid/sdk', digest)
            with self.assertRaises(ValueError):
                bootstrap_ci.download(archive, 'https://example.invalid/sdk', '0' * 64)
            self.assertEqual(archive.read_bytes(), b'cached SDK')


if __name__ == '__main__':
    unittest.main()
