# SPDX-License-Identifier: GPL-3.0-or-later
"""Published archive staging, unsafe input rejection and Pages workflow permissions."""
import copy
import hashlib
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import warnings
from unittest.mock import patch
import zipfile

import yaml

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'companion'))
import deploy_pages
import verify_pages
from urllib.error import URLError


class FakeGitHub:
    def __init__(self):
        self.releases = []
        self.assets = {}
        self.commits = {}

    def add(self, tag, draft=False):
        commit = hashlib.sha1(tag.encode()).hexdigest()
        metadata = {
            'identity': tag + '@' + commit,
            'label': tag,
            'commit': commit,
            'source_sha256': 'b' * 64,
        }
        files = {
            tag + '/' + name: name.encode()
            for name in (
                'index.html',
                'style.css',
                'src/app.mjs',
                'render/ht-ui.mjs',
                'render/ht-ui.wasm',
                'bootloader/burner_venus.bin',
            )
        }
        files[tag + '/release.json'] = deploy_pages.canonical(metadata)
        files[tag + '/src/release.mjs'] = (
            'export const release = ' + json.dumps(metadata) + ';\n'
        ).encode()
        files[tag + '/src/site.mjs'] = b'export const site = {"basePath":"/oe3anc-ht/"};\n'
        self.commits[tag] = commit
        release = {
            'id': len(self.releases) + 1,
            'tag_name': tag,
            'target_commitish': commit,
            'draft': draft,
            'body': '',
        }
        self.releases.append(release)
        self.set_files(release, files)
        return release, files

    def set_files(self, release, files):
        buffer = io.BytesIO()
        with zipfile.ZipFile(buffer, 'w') as archive:
            for name, content in files.items():
                archive.writestr(name, content)
        tag = release['tag_name']
        metadata = json.loads(files[tag + '/release.json'])
        manifest = {'schema_version': 1, 'release': metadata, 'packages': []}
        assets = []
        for role, data in (
            ('firmware', b'firmware package fixture'),
            ('companion', buffer.getvalue()),
        ):
            name = role + '-' + tag + '.zip'
            assets.append({'name': name, 'size': len(data), 'data': data, 'state': 'uploaded'})
            manifest['packages'].append(
                {
                    'role': role,
                    'name': name,
                    'size': len(data),
                    'sha256': hashlib.sha256(data).hexdigest(),
                }
            )
        data = deploy_pages.canonical(manifest)
        assets.append(
            {'name': deploy_pages.MANIFEST, 'size': len(data), 'data': data, 'state': 'uploaded'}
        )
        self.assets[release['id']] = assets
        release['body'] = (
            'Pair status: COMPLETE\n<!-- oe3anc-ht-pair:'
            + hashlib.sha256(data).hexdigest()
            + ' -->'
        )

    def listing(self, path):
        if path == '/releases':
            return copy.deepcopy(self.releases)
        return copy.deepcopy(self.assets[int(path.split('/')[2])])

    def download(self, asset):
        return asset['data']

    def json(self, method, path):
        assert method == 'GET', 'Pages assembly must be read-only'
        if path.startswith('/git/ref/tags/'):
            return {'object': {'type': 'commit', 'sha': self.commits[path.rsplit('/', 1)[1]]}}
        return copy.deepcopy(next(r for r in self.releases if r['id'] == int(path.split('/')[2])))


class PagesChecks(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.output = Path(self.temporary.name) / 'site'
        self.api = FakeGitHub()

    def assemble(self, required=None):
        deploy_pages.assemble(self.api, self.output, '/oe3anc-ht/', required)

    def test_retains_exact_archives_and_selects_highest_version_independent_of_event_order(self):
        _, expected = self.api.add('v1.1.0')
        older, old_files = self.api.add('v1.0.0')
        self.api.add('v4.0.0', draft=True)
        self.api.add('v1.1.0-rc.9')
        self.assemble(older['id'])
        for name, data in dict(expected, **old_files).items():
            self.assertEqual((self.output / name).read_bytes(), data)
        self.assertIn('./v1.1.0/', (self.output / 'index.html').read_text())
        self.assertFalse((self.output / 'v4.0.0').exists())
        self.assertTrue((self.output / '.nojekyll').exists())

    def test_drafts_and_unrelated_release_events_cannot_deploy(self):
        release, _ = self.api.add('v1.0.0', draft=True)
        with self.assertRaisesRegex(ValueError, 'No published'):
            self.assemble(release['id'])
        release['draft'] = False
        with self.assertRaisesRegex(ValueError, 'No published'):
            self.assemble(99)

    def test_invalid_published_tag_reports_exact_value_without_writing_site(self):
        release, _ = self.api.add('v1.0.0-rc.1')
        release['tag_name'] = 'v1.0.0-rc.1 '
        with self.assertRaisesRegex(ValueError, "Invalid version tag 'v1.0.0-rc.1 '"):
            self.assemble()
        self.assertFalse(self.output.exists())

    def test_custom_domain_preserves_archive_bytes_and_compiled_mismatch_links(self):
        release, files = self.api.add('v1.0.0-rc.1')
        deploy_pages.assemble(self.api, self.output, '/oe3anc-ht/', release['id'], '/')
        for name, data in files.items():
            self.assertEqual((self.output / 'oe3anc-ht' / name).read_bytes(), data)
        self.assertIn('./oe3anc-ht/v1.0.0-rc.1/', (self.output / 'index.html').read_text())
        self.assertFalse((self.output / 'v1.0.0-rc.1').exists())
        # The absolute base embedded in site.mjs resolves to the retained mounted version.
        self.assertTrue((self.output / 'oe3anc-ht/v1.0.0-rc.1/index.html').is_file())

    def test_domain_root_archive_and_incompatible_hosting_root(self):
        release, files = self.api.add('v1.0.0')
        files['v1.0.0/src/site.mjs'] = b'export const site = {"basePath":"/"};\n'
        self.api.set_files(release, files)
        with self.assertRaisesRegex(ValueError, 'outside the Pages hosting root'):
            deploy_pages.assemble(self.api, self.output, '/', site_base_path='/oe3anc-ht/')
        deploy_pages.assemble(self.api, self.output, '/', site_base_path='/')
        self.assertIn('./v1.0.0/', (self.output / 'index.html').read_text())
        for name, data in files.items():
            self.assertEqual((self.output / name).read_bytes(), data)

    def test_cli_detects_repository_cname_without_changing_packages(self):
        _, files = self.api.add('v1.0.0-rc.1')
        for custom_domain in (False, True):
            source = Path(self.temporary.name) / ('custom' if custom_domain else 'project')
            source.mkdir()
            if custom_domain:
                (source / 'CNAME').write_text('companion.oe3anc.at\n')
            output = source / 'build/pages'
            argv = ['deploy_pages.py', '--repository', 'owner/repo', '--output', str(output)]
            with patch.object(
                deploy_pages, '__file__', str(source / 'companion/deploy_pages.py')
            ), patch.object(deploy_pages, 'GitHub', return_value=self.api), patch.object(
                sys, 'argv', argv
            ), patch.dict(
                os.environ, {'GH_TOKEN': 'fixture', 'PAGES_RELEASE_ID': ''}
            ):
                deploy_pages.main()
            mount = 'oe3anc-ht/' if custom_domain else ''
            self.assertIn('./' + mount + 'v1.0.0-rc.1/', (output / 'index.html').read_text())
            for name, data in files.items():
                self.assertEqual((output / mount / name).read_bytes(), data)

    def test_moved_tag_corrupt_packages_and_incomplete_pair_fail(self):
        release, _ = self.api.add('v1.0.0')
        self.api.commits['v1.0.0'] = 'c' * 40
        with self.assertRaisesRegex(ValueError, 'Remote tag moved'):
            self.assemble()
        self.api.commits['v1.0.0'] = release['target_commitish']
        asset = self.api.assets[release['id']][0]
        original = asset['data']
        asset['data'] = b'x' * asset['size']
        with self.assertRaisesRegex(ValueError, 'checksum mismatch'):
            self.assemble()
        asset['data'] = original
        release['body'] = release['body'].replace('COMPLETE', 'INCOMPLETE')
        with self.assertRaisesRegex(ValueError, 'complete, bound pair'):
            self.assemble()
        self.assertFalse((self.output / 'index.html').exists())

    def test_unsafe_archive_base_mismatch_missing_modules_and_identity_fail(self):
        release, files = self.api.add('v1.0.0')
        for name in (
            'v1.0.0/../../escape',
            'v1.0.0/./hidden',
            'v1.0.0\\escape',
            '/absolute',
            'other-version/index.html',
        ):
            with self.subTest(name=name):
                self.api.set_files(release, dict(files, **{name: b'bad'}))
                with self.assertRaisesRegex(ValueError, 'Unsafe'):
                    self.assemble()
        wrong_base = dict(files)
        wrong_base['v1.0.0/src/site.mjs'] = b'export const site = {"basePath":"/"};\n'
        self.api.set_files(release, wrong_base)
        with self.assertRaisesRegex(ValueError, 'hosting base mismatch'):
            self.assemble()
        missing = dict(files)
        del missing['v1.0.0/src/release.mjs']
        self.api.set_files(release, missing)
        with self.assertRaisesRegex(ValueError, 'Incomplete companion'):
            self.assemble()
        mismatch = dict(files)
        mismatch['v1.0.0/release.json'] = b'{}'
        descriptor = json.loads(files['v1.0.0/release.json'])
        data = io.BytesIO()
        with zipfile.ZipFile(data, 'w') as archive:
            for name, content in mismatch.items():
                archive.writestr(name, content)
        with self.assertRaisesRegex(ValueError, 'identity mismatch'):
            deploy_pages.archive_files(data.getvalue(), 'v1.0.0', descriptor, '/oe3anc-ht/')

    def test_duplicate_symlink_and_expansion_limit_are_rejected(self):
        release, files = self.api.add('v1.0.0')
        metadata = json.loads(files['v1.0.0/release.json'])
        for mode in ('duplicate', 'symlink', 'expanded'):
            data = io.BytesIO()
            with warnings.catch_warnings(), zipfile.ZipFile(
                data, 'w', compression=zipfile.ZIP_DEFLATED
            ) as archive:
                warnings.simplefilter('ignore', UserWarning)
                if mode == 'duplicate':
                    archive.writestr('v1.0.0/file', b'one')
                    archive.writestr('v1.0.0/file', b'two')
                elif mode == 'symlink':
                    entry = zipfile.ZipInfo('v1.0.0/link')
                    entry.external_attr = 0o120777 << 16
                    archive.writestr(entry, b'outside')
                else:
                    archive.writestr('v1.0.0/file', b'x' * 1025)
            with patch.object(deploy_pages, 'MAX_PACKAGE', 1024), self.assertRaisesRegex(
                ValueError, 'Unsafe'
            ):
                deploy_pages.archive_files(data.getvalue(), 'v1.0.0', metadata, '/oe3anc-ht/')

    def test_missing_assets_changed_release_and_existing_site_abort(self):
        release, _ = self.api.add('v1.0.0')
        assets = self.api.assets[release['id']]
        removed = assets.pop(0)
        with self.assertRaisesRegex(ValueError, 'Missing or invalid'):
            self.assemble()
        assets.insert(0, removed)
        original = self.api.json

        def change_release(method, path):
            result = original(method, path)
            if path.startswith('/releases/'):
                result['draft'] = True
            return result

        with patch.object(self.api, 'json', side_effect=change_release), self.assertRaisesRegex(
            ValueError, 'Release changed'
        ):
            self.assemble()
        self.assertFalse((self.output / 'index.html').exists())
        self.output.mkdir()
        (self.output / 'index.html').write_text('existing site')
        with self.assertRaisesRegex(ValueError, 'must be empty'):
            self.assemble()
        self.assertEqual((self.output / 'index.html').read_text(), 'existing site')

    def make_nightly(self):
        api = FakeGitHub()
        release, tagged_files = api.add('v1.0.0')
        metadata = json.loads(tagged_files['v1.0.0/release.json'])
        metadata['label'] = 'development'
        metadata['identity'] = 'dev-' + metadata['source_sha256'][:32]
        files = {name.replace('v1.0.0/', 'nightly/', 1): data
                 for name, data in tagged_files.items()}
        files['nightly/release.json'] = deploy_pages.canonical(metadata)
        files['nightly/src/release.mjs'] = (
            'export const release = ' + json.dumps(metadata) + ';\n'
        ).encode()
        directory = Path(self.temporary.name) / 'packages'
        directory.mkdir()
        buffer = io.BytesIO()
        with zipfile.ZipFile(buffer, 'w') as archive:
            for name, data in files.items():
                archive.writestr(name, data)
        manifest = {'schema_version': 1, 'release': metadata, 'packages': []}
        for role, data in [('firmware', b'nightly firmware'), ('companion', buffer.getvalue())]:
            name = role + '-nightly.zip'
            (directory / name).write_bytes(data)
            manifest['packages'].append({
                'role': role, 'name': name, 'size': len(data),
                'sha256': hashlib.sha256(data).hexdigest(),
            })
        (directory / deploy_pages.MANIFEST).write_bytes(deploy_pages.canonical(manifest))
        return directory, metadata

    def test_nightly_date_firmware_pair_and_empty_release_history(self):
        directory, metadata = self.make_nightly()
        files = deploy_pages.nightly_files(
            directory, metadata['commit'], '/oe3anc-ht/', '2026-10-15T14:23:45Z'
        )
        self.assertEqual(files['nightly/firmware-nightly.zip'], b'nightly firmware')
        self.assertIn(b'2026-10-15 14:23 UTC', files['nightly/src/site.mjs'])
        self.assertEqual(json.loads(files['nightly/release.json']), metadata)
        deploy_pages.assemble(self.api, self.output, '/oe3anc-ht/', nightly=files)
        self.assertIn('./nightly/', (self.output / 'index.html').read_text())
        self.assertEqual((self.output / 'nightly/firmware-nightly.zip').read_bytes(),
                         b'nightly firmware')

    def test_nightly_coexists_with_unchanged_release_archives_on_custom_domain(self):
        directory, metadata = self.make_nightly()
        release, archived = self.api.add('v1.0.0')
        files = deploy_pages.nightly_files(
            directory, metadata['commit'], '/oe3anc-ht/', '2026-10-15T14:23:45Z'
        )
        deploy_pages.assemble(self.api, self.output, '/oe3anc-ht/', release['id'], '/', files)
        for name, content in archived.items():
            self.assertEqual((self.output / 'oe3anc-ht' / name).read_bytes(), content)
        self.assertTrue((self.output / 'nightly/index.html').is_file())
        self.assertIn('./oe3anc-ht/v1.0.0/', (self.output / 'index.html').read_text())

    def test_nightly_rejects_wrong_commit_corrupt_packages_and_invalid_date(self):
        directory, metadata = self.make_nightly()
        for commit, date in [('0' * 40, '2026-10-15T14:23:45Z'),
                             (metadata['commit'], 'not a date')]:
            with self.assertRaises(ValueError):
                deploy_pages.nightly_files(directory, commit, '/oe3anc-ht/', date)
        (directory / 'firmware-nightly.zip').write_bytes(b'corrupt')
        with self.assertRaises(ValueError):
            deploy_pages.nightly_files(
                directory, metadata['commit'], '/oe3anc-ht/', '2026-10-15T14:23:45Z'
            )

    def test_pages_workflow_triggers_trusted_checkout_and_least_privilege(self):
        workflow = yaml.load(
            (ROOT / '.github/workflows/pages.yml').read_text(), Loader=yaml.BaseLoader
        )
        self.assertEqual(set(workflow['on']), {'push', 'release', 'workflow_dispatch'})
        self.assertEqual(workflow['on']['release']['types'], ['published'])
        self.assertEqual(workflow['on']['push']['branches'], ['main'])
        self.assertEqual(workflow['permissions'], {'contents': 'read'})
        self.assertEqual(workflow['concurrency']['cancel-in-progress'], 'false')
        assemble, deploy = workflow['jobs']['assemble'], workflow['jobs']['deploy']
        self.assertNotIn('permissions', assemble)
        dispatch = workflow['jobs']['dispatch-release']
        self.assertEqual(dispatch['if'], "github.event_name == 'release'")
        self.assertEqual(dispatch['permissions'], {'actions': 'write'})
        step = dispatch['steps'][0]
        self.assertEqual(step['env']['RELEASE_ID'], '${{ github.event.release.id }}')
        self.assertIn('--ref "$DEFAULT_BRANCH" -f release_id="$RELEASE_ID"', step['run'])
        self.assertIn("github.event_name != 'release' &&", assemble['if'])
        stage = next(step for step in assemble['steps'] if 'Stage nightly' in step.get('name', ''))
        self.assertEqual(stage['env']['PAGES_RELEASE_ID'], '${{ inputs.release_id }}')
        self.assertEqual(workflow['on']['workflow_dispatch']['inputs']['release_id']['type'], 'string')
        self.assertEqual(deploy['steps'][0]['with']['ref'], '${{ needs.assemble.outputs.source }}')
        self.assertEqual(deploy['steps'][-1]['env']['PAGES_EXPECTED'],
                         '${{ needs.assemble.outputs.checks }}')
        self.assertIn('verify_pages.py --url', deploy['steps'][-1]['run'])
        self.assertIn('github.event.repository.default_branch', assemble['if'])
        checkout = assemble['steps'][0]
        self.assertEqual(checkout['with']['ref'], '${{ github.event.repository.default_branch }}')
        self.assertEqual(checkout['with']['persist-credentials'], 'false')
        self.assertEqual(deploy['needs'], 'assemble')
        self.assertEqual(deploy['permissions'],
                         {'contents': 'read', 'pages': 'write', 'id-token': 'write'})
        self.assertEqual(deploy['environment']['name'], 'github-pages')
        self.assertEqual(assemble['steps'][-1]['with']['path'], 'build/pages')
        for job in workflow['jobs'].values():
            for step in job['steps']:
                self.assertNotIn('secrets.', json.dumps(step))
                if 'uses' in step and not step['uses'].startswith('./'):
                    self.assertRegex(step['uses'].split(' #')[0], r'^actions/[a-z-]+@[0-9a-f]{40}$')
                if 'run' in step:
                    self.assertNotIn('${{', step['run'])
                    subprocess.run(['bash', '-n'], input=step['run'], text=True, check=True)


class LivePagesChecks(unittest.TestCase):
    def test_staged_checks_cover_landing_archived_release_and_nightly(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'index.html').write_text('latest release')
            with self.assertRaisesRegex(ValueError, 'No staged'):
                verify_pages.expected_content(root)
            expected = {'index.html': b'latest release'}
            for version in ('oe3anc-ht/v1.3.9', 'nightly'):
                for name in ('release.json', 'index.html', 'src/site.mjs'):
                    path = root / version / name
                    path.parent.mkdir(parents=True, exist_ok=True)
                    path.write_text(version + name)
                    expected[version + '/' + name] = (version + name).encode()
            self.assertEqual(verify_pages.expected_content(root), {
                path: hashlib.sha256(content).hexdigest() for path, content in expected.items()
            })

    def test_old_content_and_404_retry_until_correct_content_is_served(self):
        digest = hashlib.sha256(b'new release').hexdigest()
        with patch.object(verify_pages, 'urlopen', side_effect=[
            io.BytesIO(b'old release'), URLError('404'), io.BytesIO(b'new release')
        ]) as fetch, patch.object(verify_pages.time, 'sleep') as sleep:
            verify_pages.verify('https://example.invalid/', {'v1.3.9/release.json': digest})
        self.assertEqual(fetch.call_count, 3)
        self.assertEqual(sleep.call_count, 2)
        self.assertIn('/v1.3.9/release.json?verify=', fetch.call_args.args[0].full_url)

    def test_success_report_cannot_hide_stale_site_or_missing_expectations(self):
        digest = hashlib.sha256(b'new').hexdigest()
        with patch.object(verify_pages, 'urlopen', return_value=io.BytesIO(b'old')), \
             patch.object(verify_pages.time, 'monotonic', side_effect=[0, 0, 2]):
            with self.assertRaisesRegex(RuntimeError, 'release.json'):
                verify_pages.verify('https://example.invalid/', {'release.json': digest}, timeout=1)
        with self.assertRaises(ValueError):
            verify_pages.verify('https://example.invalid/', {})


if __name__ == '__main__':
    unittest.main()
