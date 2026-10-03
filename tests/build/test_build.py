# SPDX-License-Identifier: GPL-3.0-or-later
"""Dependency and DSP pairing checks, runnable without flashing hardware."""
import hashlib
import importlib.util
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('build', ROOT / 'tools/build.py')
build = importlib.util.module_from_spec(spec)
spec.loader.exec_module(build)


class BuildChecks(unittest.TestCase):
    def test_dependency_revision_and_dirty_files_are_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            repo = Path(directory)
            subprocess.run(['git', 'init', '-q', str(repo)], check=True)
            (repo / 'source.c').write_text('original')
            subprocess.run(['git', '-C', str(repo), 'add', '.'], check=True)
            subprocess.run(
                [
                    'git',
                    '-C',
                    str(repo),
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
                ['git', '-C', str(repo), 'rev-parse', 'HEAD'], text=True
            ).strip()
            build.verify_revision(repo, revision)
            with self.assertRaises(RuntimeError):
                build.verify_revision(repo, '0' * 40)
            (repo / 'source.c').write_text('modified')
            with self.assertRaises(RuntimeError):
                build.verify_revision(repo, revision)

    def test_sdk_version_is_enforced(self):
        with tempfile.TemporaryDirectory() as directory:
            sdk = Path(directory)
            with self.assertRaises(RuntimeError):
                build.verify_sdk(sdk)
            (sdk / 'sdk_version').write_text('0.16.1\n')
            build.verify_sdk(sdk)
            (sdk / 'sdk_version').write_text('1.0.1\n')
            with self.assertRaises(RuntimeError):
                build.verify_sdk(sdk)

    def test_dsp_pairing_accepts_pinned_image_and_rejects_another(self):
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            output = directory / 'build/zephyr/dsp_firmware.bin'
            original = ROOT / 'backends/c62/resources/dsp_firmware.bin'
            original_bytes = original.read_bytes()
            (directory / 'CMakeLists.txt').write_text(
                'cmake_minimum_required(VERSION 3.20)\n'
                'project(dsp_package NONE)\n'
                f'include("{ROOT / "cmake/dsp-image.cmake"}")\n'
            )
            wrong = directory / 'wrong.bin'
            wrong.write_bytes(b'wrong firmware')
            command = [
                'cmake',
                '-S',
                str(directory),
                '-B',
                str(directory / 'build'),
                '-G',
                'Ninja',
                '-DPython3_EXECUTABLE=' + sys.executable,
            ]
            subprocess.run(command, check=True)
            build_command = ['cmake', '--build', str(directory / 'build')]
            subprocess.run(build_command, check=True)
            patched = output.read_bytes()
            self.assertEqual(
                hashlib.sha256(patched).hexdigest(),
                'fc0e2d7a8a2a991c831daf798a965743a178a582c6cbab9f9ef6a05c34628bdb',
            )
            # Incremental builds retain the image; missing outputs regenerate it.
            subprocess.run(build_command, check=True)
            self.assertEqual(output.read_bytes(), patched)
            output.unlink()
            subprocess.run(build_command, check=True)
            self.assertEqual(output.read_bytes(), patched)
            self.assertEqual(original.read_bytes(), original_bytes)
            rejected = subprocess.run(
                command + ['-DHT_DSP_IMAGE=' + str(wrong)], capture_output=True, text=True
            )
            self.assertNotEqual(rejected.returncode, 0)
            self.assertIn('does not match', rejected.stderr)

    def test_dsp_patch_never_overwrites_its_original(self):
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            original = directory / 'original.bin'
            original_bytes = (ROOT / 'backends/c62/resources/dsp_firmware.bin').read_bytes()
            original.write_bytes(original_bytes)
            symlink = directory / 'symlink.bin'
            symlink.symlink_to(original)
            hardlink = directory / 'hardlink.bin'
            os.link(original, hardlink)
            for output in (original, symlink, hardlink):
                rejected = subprocess.run(
                    [
                        sys.executable,
                        str(ROOT / 'tools/patch_dsp_uart.py'),
                        '--input',
                        str(original),
                        '--output',
                        str(output),
                        '--replace-output',
                    ],
                    capture_output=True,
                    text=True,
                )
                self.assertNotEqual(rejected.returncode, 0)
                self.assertIn('must not replace', rejected.stderr)
                self.assertEqual(original.read_bytes(), original_bytes)


if __name__ == '__main__':
    unittest.main()
