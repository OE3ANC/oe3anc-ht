# SPDX-License-Identifier: GPL-3.0-or-later
"""Static map assets remain local under domain-root and project-subpath hosting."""
import importlib.util
import json
from pathlib import Path
import shutil
import sys
import tempfile
import unittest
from unittest.mock import patch
from urllib.parse import urljoin

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'companion'))
spec = importlib.util.spec_from_file_location('companion_build', ROOT / 'companion/build.py')
builder = importlib.util.module_from_spec(spec)
spec.loader.exec_module(builder)


class CompanionAssets(unittest.TestCase):
    def test_local_assets_at_root_and_subpath(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            shutil.copytree(ROOT / 'companion', root / 'companion',
                            ignore=shutil.ignore_patterns('dist', '__pycache__'))
            for base in ('/', '/oe3anc-ht/'):
                with self.subTest(base=base), patch.object(builder, 'ROOT', root), \
                        patch.object(builder.companion_release, 'metadata',
                                     return_value={'identity': 'fixture'}), \
                        patch.object(builder.subprocess, 'run'), \
                        patch.object(builder, 'build_renderer'), \
                        patch.object(sys, 'argv', ['build.py', '--emcc', '/fixture/emcc',
                                                   '--base-path', base]):
                    builder.main()
                dist = root / 'companion/dist'
                html = (dist / 'index.html').read_text()
                for name in ('leaflet.js', 'leaflet.css'):
                    relative = './vendor/leaflet/' + name
                    self.assertIn(relative, html)
                    self.assertEqual(urljoin('https://example.invalid' + base, relative),
                                     'https://example.invalid' + base + 'vendor/leaflet/' + name)
                for path in (ROOT / 'companion/vendor').rglob('*'):
                    if path.is_file():
                        self.assertEqual((dist / 'vendor' / path.relative_to(
                            ROOT / 'companion/vendor')).read_bytes(), path.read_bytes())
                site = (dist / 'src/site.mjs').read_text()
                self.assertEqual(json.loads(site.removeprefix('export const site = ')
                                            .removesuffix(';\n'))['basePath'], base)
                self.assertTrue((dist / 'src/received-map.mjs').is_file())


if __name__ == '__main__':
    unittest.main()
