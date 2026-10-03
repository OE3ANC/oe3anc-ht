# SPDX-License-Identifier: GPL-3.0-or-later
"""Development identity stability and clean tagged build guards."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
import companion_release


class ReleaseChecks(unittest.TestCase):
    def test_pairing_and_tag_guards(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            subprocess.run(['git', 'init', '-q', str(root)], check=True)
            for name in ('CMakeLists.txt', 'prj.conf', 'west.yml'):
                (root / name).write_text('fixture\n')
            (root / '.gitignore').write_text('/build/\n/companion/dist/\n')
            (root / 'modules').mkdir()
            source = root / 'modules/source.cpp'
            source.write_text('first\n')

            def git(*args):
                return subprocess.check_output(['git', '-C', str(root), *args], text=True).strip()

            git('add', '.')
            git(
                '-c',
                'user.name=Test',
                '-c',
                'user.email=test@example.invalid',
                'commit',
                '-qm',
                'fixture',
            )
            git('tag', 'v1.0.0')
            with patch.object(companion_release, 'ROOT', root):
                first = companion_release.metadata()
                self.assertEqual(first, companion_release.metadata())
                tagged = companion_release.metadata('v1.0.0')
                self.assertEqual(tagged['identity'], 'v1.0.0@' + git('rev-parse', 'HEAD'))
                with self.assertRaises(ValueError):
                    companion_release.metadata('not-a-version')
                (root / 'companion/dist').mkdir(parents=True)
                (root / 'companion/dist/release.json').write_text('build output')
                (root / 'build/pages').mkdir(parents=True)
                (root / 'build/pages/index.html').write_text('deployment output')
                self.assertEqual(first, companion_release.metadata())
                self.assertEqual(tagged, companion_release.metadata('v1.0.0'))
                source.write_text('changed\n')
                self.assertNotEqual(first['identity'], companion_release.metadata()['identity'])
                with self.assertRaises(ValueError):
                    companion_release.metadata('v1.0.0')
                git('add', 'modules/source.cpp')
                git(
                    '-c',
                    'user.name=Test',
                    '-c',
                    'user.email=test@example.invalid',
                    'commit',
                    '-qm',
                    'changed',
                )
                with self.assertRaises(ValueError):
                    companion_release.metadata('v1.0.0')


if __name__ == '__main__':
    unittest.main()
