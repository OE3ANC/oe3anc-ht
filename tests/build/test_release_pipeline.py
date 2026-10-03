# SPDX-License-Identifier: GPL-3.0-or-later
"""Release trust boundaries, partial-upload retries and frozen pairs; no GitHub writes."""
import copy
import hashlib
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
from urllib.error import HTTPError
import zipfile

import yaml

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
import publish_release
import release_packages

TAG = 'v1.0.0-rc.1'
COMMIT = 'a' * 40
RELEASE = {
    'identity': TAG + '@' + COMMIT,
    'label': TAG,
    'commit': COMMIT,
    'source_sha256': 'b' * 64,
}


class FakeGitHub:
    def __init__(self):
        self.commit = COMMIT
        self.annotated = False
        self.release = None
        self.assets = {}
        self.writes = []
        self.fail_upload = None
        self.fail_status = False
        self.move_after_upload = False
        self.retag_after_upload = False
        self.on_create = None

    def listing(self, path):
        return copy.deepcopy(
            [self.release]
            if path == '/releases' and self.release
            else [] if path == '/releases' else list(self.assets.values())
        )

    def json(self, method, path, value=None):
        if method == 'GET':
            if path.startswith('/git/ref/'):
                return {
                    'object': {
                        'type': 'tag' if self.annotated else 'commit',
                        'sha': 'c' * 40 if self.annotated else self.commit,
                    }
                }
            if path.startswith('/git/tags/'):
                return {'object': {'type': 'commit', 'sha': self.commit}}
            return copy.deepcopy(self.release)
        self.writes.append((method, path))
        if method == 'POST':
            self.release = dict(value, id=7)
            if self.on_create:
                self.on_create()
            return copy.deepcopy(self.release)
        if method == 'PATCH':
            if self.fail_status:
                self.fail_status = False
                raise RuntimeError('lost status update')
            self.release.update(value)
        elif method == 'DELETE':
            name = next(
                name
                for name, asset in self.assets.items()
                if asset['id'] == int(path.rsplit('/', 1)[1])
            )
            del self.assets[name]

    def upload(self, release, name, data):
        self.writes.append(('UPLOAD', name))
        if name in self.assets:
            raise AssertionError('asset clobber')
        asset = {
            'id': len(self.writes),
            'name': name,
            'size': len(data),
            'state': 'uploaded',
            'data': data,
        }
        self.assets[name] = asset
        if self.fail_upload == name:
            self.fail_upload = None
            asset.update(state='starter', size=0, data=b'')
            raise RuntimeError('failed upload')
        if self.move_after_upload:
            self.commit = 'd' * 40
        if self.retag_after_upload:
            self.release['tag_name'] = 'v3.9.9'
        return copy.deepcopy(asset)

    def download(self, asset):
        return self.assets[asset['name']]['data']


class ReleaseChecks(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        self.api = FakeGitHub()
        self.manifest = {'schema_version': 1, 'release': dict(RELEASE), 'packages': []}
        for role in ('firmware', 'companion'):
            name = role + '-' + TAG + '.zip'
            data = (role + ' fixture').encode()
            (self.directory / name).write_bytes(data)
            self.manifest['packages'].append(
                {
                    'role': role,
                    'name': name,
                    'size': len(data),
                    'sha256': hashlib.sha256(data).hexdigest(),
                }
            )
        self.save_manifest()

    def save_manifest(self):
        (self.directory / release_packages.MANIFEST).write_bytes(
            release_packages.canonical(self.manifest)
        )

    def publish(self):
        publish_release.publish(self.api, self.directory, TAG, COMMIT, RELEASE)

    def test_annotated_tag_complete_pair_and_published_rerun_are_read_only(self):
        self.api.annotated = True
        self.publish()
        self.assertTrue(self.api.release['draft'])
        self.assertTrue(self.api.release['prerelease'])
        self.assertIn('Pair status: COMPLETE', self.api.release['body'])
        original = copy.deepcopy(self.api.assets)
        self.api.writes.clear()
        self.publish()
        self.assertEqual(self.api.writes, [])
        self.api.release['draft'] = False
        self.publish()
        self.assertEqual(self.api.writes, [])
        self.assertEqual(self.api.assets, original)

    def test_failed_upload_retries_only_missing_asset_and_empty_starter(self):
        failed = self.manifest['packages'][1]['name']
        self.api.fail_upload = failed
        with self.assertRaisesRegex(RuntimeError, 'failed upload'):
            self.publish()
        first = copy.deepcopy(self.api.assets[self.manifest['packages'][0]['name']])
        self.assertIn('INCOMPLETE', self.api.release['body'])
        self.api.writes.clear()
        self.publish()
        self.assertEqual(self.api.assets[first['name']], first)
        self.assertEqual([name for method, name in self.api.writes if method == 'UPLOAD'], [failed])
        self.assertEqual(sum(method == 'DELETE' for method, _ in self.api.writes), 1)

    def test_pair_freezes_even_when_complete_status_update_fails(self):
        self.api.fail_status = True
        with self.assertRaisesRegex(RuntimeError, 'lost status'):
            self.publish()
        original = copy.deepcopy(self.api.assets)
        descriptor = self.manifest['packages'][0]
        changed = b'changed package'
        (self.directory / descriptor['name']).write_bytes(changed)
        descriptor.update(size=len(changed), sha256=hashlib.sha256(changed).hexdigest())
        self.save_manifest()
        self.api.writes.clear()
        with self.assertRaisesRegex(ValueError, 'different contents'):
            self.publish()
        self.assertEqual(self.api.writes, [])
        self.assertEqual(self.api.assets, original)

    def test_moved_tag_rejects_before_creation_and_between_uploads(self):
        self.api.commit = 'd' * 40
        with self.assertRaisesRegex(ValueError, 'Remote tag moved'):
            self.publish()
        self.assertEqual(self.api.writes, [])
        self.api.commit = COMMIT
        self.api.move_after_upload = True
        with self.assertRaisesRegex(ValueError, 'Remote tag moved'):
            self.publish()
        self.assertEqual(set(self.api.assets), {release_packages.MANIFEST})

    def test_verified_bytes_are_immutable_during_publication(self):
        name = self.manifest['packages'][0]['name']
        path = self.directory / name
        verified = path.read_bytes()
        self.api.on_create = lambda: path.write_bytes(b'changed during API call')
        self.publish()
        self.assertEqual(self.api.assets[name]['data'], verified)
        self.assertEqual(self.api.assets[name]['size'], self.manifest['packages'][0]['size'])

    def test_retagged_draft_rejects_between_uploads_and_before_final_status(self):
        self.api.retag_after_upload = True
        with self.assertRaisesRegex(ValueError, 'Release changed'):
            self.publish()
        self.assertEqual(set(self.api.assets), {release_packages.MANIFEST})
        self.api = FakeGitHub()
        upload = self.api.upload

        def retag_after_last(release, name, data):
            result = upload(release, name, data)
            if name == self.manifest['packages'][1]['name']:
                self.api.release['tag_name'] = 'v3.9.9'
            return result

        self.api.upload = retag_after_last
        with self.assertRaisesRegex(ValueError, 'Release identity changed'):
            self.publish()
        self.assertIn('Pair status: INCOMPLETE', self.api.release['body'])

    def test_published_partial_pair_and_corrupt_uploaded_assets_cannot_be_replaced(self):
        self.api.fail_upload = self.manifest['packages'][1]['name']
        with self.assertRaises(RuntimeError):
            self.publish()
        self.api.release['draft'] = False
        self.api.writes.clear()
        with self.assertRaisesRegex(ValueError, 'published'):
            self.publish()
        self.assertEqual(self.api.writes, [])
        self.api.release['draft'] = True
        first = self.api.assets[self.manifest['packages'][0]['name']]
        first['data'] = b'x' * first['size']
        with self.assertRaisesRegex(ValueError, 'cannot be overwritten'):
            self.publish()
        self.assertEqual(self.api.writes, [])

    def test_incoming_hash_bad_commit_and_existing_manual_release_are_rejected(self):
        path = self.directory / self.manifest['packages'][0]['name']
        original = path.read_bytes()
        path.write_bytes(b'x' * len(original))
        with self.assertRaisesRegex(ValueError, 'checksum mismatch'):
            self.publish()
        self.assertEqual(self.api.writes, [])
        path.write_bytes(original)
        self.manifest['release']['commit'] = 'd' * 40
        self.save_manifest()
        with self.assertRaises(ValueError):
            self.publish()
        self.assertEqual(self.api.writes, [])
        self.manifest['release'] = dict(RELEASE)
        self.save_manifest()
        self.api.release = {
            'id': 7,
            'tag_name': TAG,
            'body': 'manual',
            'target_commitish': COMMIT,
            'draft': True,
        }
        with self.assertRaisesRegex(ValueError, 'different contents'):
            self.publish()
        self.assertEqual(self.api.writes, [])

    def test_zip_bytes_are_stable_and_manifest_destinations_are_strict(self):
        path = self.directory / 'archive.zip'
        release_packages.archive(path, {'b': b'two', 'a': b'one'})
        first = path.read_bytes()
        release_packages.archive(path, {'a': b'one', 'b': b'two'})
        self.assertEqual(path.read_bytes(), first)
        with zipfile.ZipFile(path) as archive:
            self.assertEqual(archive.namelist(), ['a', 'b'])
            self.assertEqual(archive.getinfo('a').date_time, (1980, 1, 1, 0, 0, 0))
        for key, value in (('name', '../other.zip'), ('size', True), ('sha256', 'bad')):
            bad = copy.deepcopy(self.manifest)
            bad['packages'][0][key] = value
            with self.assertRaises(ValueError):
                release_packages.validate_manifest(bad, TAG, COMMIT)

    def test_package_versioned_site_and_abort_remove_stale_manifest(self):
        root = self.directory / 'source'
        build = self.directory / 'build'
        website = self.directory / 'website'
        output = self.directory / 'output'
        dependencies = self.directory / 'dependencies'
        files = {
            root / 'LICENSES/project.txt': b'project license',
            root / 'companion/src/app.mjs': b'app',
            root / 'companion/bootloader/LICENSE': b'helper license',
            build
            / 'emulator/companion-generated/release.json': release_packages.canonical(RELEASE),
            build / 'c62/zephyr/firmware-bundle.json': b'{"release":{}}',
            dependencies / 'c62/modules/lib/gui/lvgl/LICENCE.txt': b'LVGL license',
            dependencies / 'c62/modules/lib/codec2/COPYING': b'Codec2 license',
            dependencies / 'c62/modules/lib/codec2/src/kiss_fft.c': b'KISS license/source',
        }
        for name in (
            'index.html',
            'style.css',
            'render/ht-ui.mjs',
            'render/ht-ui.wasm',
            'src/app.mjs',
            'src/release.mjs',
            'src/site.mjs',
            'bootloader/LICENSE',
            'stray-test-fixture.mjs',
        ):
            files[website / name] = name.encode()
        files[website / 'release.json'] = release_packages.canonical(RELEASE)
        for path, content in files.items():
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(content)
        with patch.object(release_packages, 'ROOT', root), patch.object(
            release_packages.companion_release, 'metadata', return_value=RELEASE
        ), patch.object(
            release_packages.firmware_bundle, 'build_bundle', return_value={'release': {}}
        ):
            manifest = release_packages.package(build, website, output, TAG, dependencies)
            release_packages.validate_files(output, manifest)
            with zipfile.ZipFile(output / ('companion-' + TAG + '.zip')) as archive:
                self.assertIn(TAG + '/index.html', archive.namelist())
                self.assertIn(TAG + '/LICENSES/LVGL.txt', archive.namelist())
                self.assertIn(TAG + '/LICENSES/project.txt', archive.namelist())
                self.assertIn(TAG + '/bootloader/LICENSE', archive.namelist())
                self.assertNotIn(TAG + '/provenance.md', archive.namelist())
                for name in ('src/release.mjs', 'src/site.mjs'):
                    self.assertEqual(archive.read(TAG + '/' + name), files[website / name])
                self.assertNotIn(TAG + '/stray-test-fixture.mjs', archive.namelist())
            with zipfile.ZipFile(output / ('firmware-' + TAG + '.zip')) as archive:
                self.assertEqual(json.loads(archive.read('release.json')), RELEASE)
                self.assertIn('firmware-bundle.json', archive.namelist())
                for name in ('project.txt', 'LVGL.txt', 'Codec2.txt', 'kiss_fft.c'):
                    self.assertIn('LICENSES/' + name, archive.namelist())
                self.assertNotIn('provenance.md', archive.namelist())
            repeated = release_packages.package(build, website, output, TAG, dependencies)
            self.assertEqual(manifest, repeated)
            (website / 'release.json').write_text('{}')
            with self.assertRaisesRegex(ValueError, 'Companion release metadata mismatch'):
                release_packages.package(build, website, output, TAG, dependencies)
            self.assertFalse((output / release_packages.MANIFEST).exists())

    def test_asset_redirect_does_not_forward_token_and_download_is_bounded(self):
        api = publish_release.GitHub('owner/repo', 'private-token')
        redirect = HTTPError(
            'https://api.github.com/asset',
            302,
            'redirect',
            {'Location': 'https://release-assets.githubusercontent.com/signed'},
            None,
        )
        with patch.object(
            api.opener, 'open', side_effect=[redirect, io.BytesIO(b'asset')]
        ) as opened:
            self.assertEqual(api.download({'id': 5}), b'asset')
            first, second = [call.args[0] for call in opened.call_args_list]
            self.assertEqual(first.get_header('Authorization'), 'Bearer private-token')
            self.assertEqual(second.full_url, redirect.headers['Location'])
            self.assertIsNone(second.get_header('Authorization'))
        with patch.object(publish_release, 'MAX_PACKAGE', 4), patch.object(
            api.opener, 'open', return_value=io.BytesIO(b'oversized')
        ):
            with self.assertRaisesRegex(ValueError, 'exceeds'):
                api.download({'id': 5})


class WorkflowChecks(unittest.TestCase):
    def test_workflow_events_permissions_artifact_origin_and_shell_syntax(self):
        def read(name):
            return yaml.load((ROOT / '.github' / name).read_text(), Loader=yaml.BaseLoader)

        ci = read('workflows/ci.yml')
        release = read('workflows/release.yml')
        action = read('actions/build/action.yml')
        self.assertEqual(set(ci['on']), {'pull_request', 'push'})
        self.assertEqual(ci['on']['push']['branches'], ['main'])
        self.assertEqual(set(release['on']), {'push'})
        self.assertEqual(release['on']['push']['tags'], ['v*'])
        self.assertEqual(release['concurrency']['cancel-in-progress'], 'false')
        for workflow in (ci, release):
            self.assertEqual(workflow['permissions'], {'contents': 'read'})
            for name, job in workflow['jobs'].items():
                self.assertEqual(job['runs-on'], 'ubuntu-22.04')
                self.assertNotIn('environment', job)
                self.assertNotIn('if', job)
                if name != 'draft':
                    self.assertNotIn('permissions', job)
                for step in job['steps']:
                    self.assertNotIn('if', step)
                    if 'uses' in step and not step['uses'].startswith('./'):
                        self.assertRegex(step['uses'], r'^actions/[a-z-]+@[0-9a-f]{40}$')
                    if step.get('uses', '').startswith('actions/checkout@'):
                        self.assertEqual(step['with']['persist-credentials'], 'false')
                    self.assertNotIn('secrets.', json.dumps(step))
        self.assertEqual(release['jobs']['draft']['needs'], 'build')
        self.assertEqual(release['jobs']['draft']['permissions'], {'contents': 'write'})
        steps = release['jobs']['draft']['steps']
        self.assertEqual(steps[0]['with']['ref'], '${{ github.sha }}')
        self.assertEqual(steps[1]['with']['artifact-ids'], '${{ needs.build.outputs.artifact-id }}')
        self.assertNotIn('github-token', steps[1]['with'])
        self.assertNotIn('run-id', steps[1]['with'])
        self.assertEqual(steps[2]['env']['GH_TOKEN'], '${{ github.token }}')
        scripts = [
            step['run']
            for job in release['jobs'].values()
            for step in job['steps']
            if 'run' in step
        ]
        scripts.extend(step['run'] for step in action['runs']['steps'] if 'run' in step)
        for script in scripts:
            self.assertNotIn('${{', script)
            subprocess.run(['bash', '-n'], input=script, text=True, check=True)
        self.assertIn('tools/check_ci.py', scripts[-1])


if __name__ == '__main__':
    unittest.main()
