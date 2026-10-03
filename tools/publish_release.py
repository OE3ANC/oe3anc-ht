#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Attach a validated pair to a draft; never publish or replace completed assets."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
from urllib.error import HTTPError
from urllib.parse import quote, urlparse
from urllib.request import HTTPRedirectHandler, Request, build_opener

import companion_release
from release_packages import MANIFEST, MAX_PACKAGE, canonical, validate_files, validate_manifest


class NoRedirect(HTTPRedirectHandler):
    def redirect_request(self, request, response, code, message, headers, url):
        return None


class GitHub:
    def __init__(self, repository, token):
        if not re.fullmatch(r'[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+', repository) or not token:
            raise ValueError('A GitHub repository and token are required')
        self.prefix = '/repos/' + repository
        self.token = token
        self.opener = build_opener(NoRedirect())

    def request(self, method, path, body=None, binary=False, upload=False):
        host = 'uploads.github.com' if upload else 'api.github.com'
        headers = {
            'Authorization': 'Bearer ' + self.token,
            'Accept': 'application/octet-stream' if binary else 'application/vnd.github+json',
            'X-GitHub-Api-Version': '2026-03-10',
            'User-Agent': 'oe3anc-ht-release',
        }
        if body is not None:
            headers['Content-Type'] = 'application/octet-stream' if upload else 'application/json'
        request = Request(
            'https://' + host + self.prefix + path, data=body, headers=headers, method=method
        )
        try:
            response = self.opener.open(request, timeout=60)
        except HTTPError as error:
            # Private asset downloads redirect to a signed URL. Never forward the token.
            location = error.headers.get('Location', '')
            if (
                method != 'GET'
                or not binary
                or error.code != 302
                or urlparse(location).scheme != 'https'
            ):
                raise
            error.close()
            response = self.opener.open(Request(location), timeout=60)
        with response:
            limit = MAX_PACKAGE if binary else 1024 * 1024
            data = response.read(limit + 1)
        if len(data) > limit:
            raise ValueError('GitHub response exceeds the release limit')
        return data if binary else json.loads(data) if data else None

    def json(self, method, path, value=None):
        return self.request(method, path, canonical(value) if value is not None else None)

    def listing(self, path):
        entries = []
        for page in range(1, 101):
            batch = self.json('GET', path + f'?per_page=100&page={page}')
            entries.extend(batch)
            if len(batch) < 100:
                return entries
        raise ValueError('GitHub listing exceeds the release limit')

    def download(self, asset):
        return self.request('GET', '/releases/assets/' + str(asset['id']), binary=True)

    def upload(self, release, name, data):
        return self.request(
            'POST',
            '/releases/' + str(release['id']) + '/assets?name=' + quote(name),
            body=data,
            upload=True,
        )


def check_tag(api, tag, commit):
    value = api.json('GET', '/git/ref/tags/' + quote(tag))['object']
    for _ in range(8):
        if value['type'] == 'commit' and value['sha'] == commit:
            return
        if value['type'] != 'tag' or not re.fullmatch('[0-9a-f]{40}', value['sha']):
            break
        value = api.json('GET', '/git/tags/' + value['sha'])['object']
    raise ValueError('Remote tag moved or does not identify the build commit')


def publish(api, directory, tag, commit, expected):
    path = directory / MANIFEST
    if path.is_symlink() or path.stat().st_size > 65536:
        raise ValueError('Invalid release manifest file')
    data = path.read_bytes()
    manifest = validate_manifest(json.loads(data), tag, commit)
    if data != canonical(manifest) or manifest['release'] != expected:
        raise ValueError('Manifest differs from the exact tagged source')
    packages = validate_files(directory, manifest)
    binding = '<!-- oe3anc-ht-pair:' + hashlib.sha256(data).hexdigest() + ' -->'
    check_tag(api, tag, commit)
    # Listing includes authenticated drafts; the by-tag endpoint is for published releases.
    matches = [release for release in api.listing('/releases') if release['tag_name'] == tag]
    if len(matches) > 1:
        raise ValueError('Multiple releases use this tag')
    if matches:
        release = matches[0]
        if binding not in (release['body'] or '') or release['target_commitish'] != commit:
            raise ValueError(
                'Existing release is bound to different contents/commit; use a new tag'
            )
    else:
        body = (
            f'Pair status: INCOMPLETE\n\nTag: `{tag}`\nCommit: `{commit}`\n\n'
            'Firmware and companion must be used together. Software checks passed; '
            'radio hardware acceptance remains manual. Do not publish an incomplete pair.\n\n'
            'Flashing/restoring may brick the device or lose data. Use at your own risk; '
            'authors and contributors accept no responsibility for resulting damage or loss.\n\n'
            + binding
        )
        release = api.json(
            'POST',
            '/releases',
            {
                'tag_name': tag,
                'target_commitish': commit,
                'name': tag,
                'body': body,
                'draft': True,
                'prerelease': '-rc.' in tag,
            },
        )
    endpoint = '/releases/' + str(release['id'])
    assets = {}
    for asset in api.listing(endpoint + '/assets'):
        if asset['name'] in assets:
            raise ValueError('Duplicate release asset names')
        assets[asset['name']] = asset

    def mutable():
        check_tag(api, tag, commit)
        current = api.json('GET', endpoint)
        if (
            not current['draft']
            or current['tag_name'] != tag
            or current['target_commitish'] != commit
            or binding not in (current['body'] or '')
        ):
            raise ValueError('Release changed or was published while the pair was incomplete')

    def ensure(name, content):
        existing = assets.get(name)
        if existing and existing['state'] == 'uploaded':
            if existing['size'] != len(content) or api.download(existing) != content:
                raise ValueError('Existing asset differs; it cannot be overwritten: ' + name)
            return
        mutable()
        if existing:
            if existing['state'] != 'starter' or existing['size'] != 0:
                raise ValueError('Unknown release asset state: ' + name)
            # GitHub documents empty starter assets as safe to delete after failed uploads.
            api.json('DELETE', '/releases/assets/' + str(existing['id']))
        created = api.upload(release, name, content)
        if (
            created['state'] != 'uploaded'
            or created['size'] != len(content)
            or api.download(created) != content
        ):
            raise ValueError('Release asset upload verification failed: ' + name)
        assets[name] = created

    # Bind the partial attempt before attaching either package. Existing bytes never change.
    ensure(MANIFEST, data)
    for package in manifest['packages']:
        ensure(package['name'], packages[package['name']])
    check_tag(api, tag, commit)
    # Both verified assets freeze the pair even if this status update fails.
    current = api.json('GET', endpoint)
    if (
        current['tag_name'] != tag
        or current['target_commitish'] != commit
        or binding not in (current['body'] or '')
    ):
        raise ValueError('Release identity changed while attaching the pair')
    if current['draft'] and 'Pair status: INCOMPLETE' in (current['body'] or ''):
        api.json(
            'PATCH',
            endpoint,
            {'body': current['body'].replace('Pair status: INCOMPLETE', 'Pair status: COMPLETE')},
        )
    print('Release pair complete; existing package bytes retained:', tag)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--directory', type=Path, default=companion_release.ROOT / 'build/release')
    parser.add_argument('--tag', required=True)
    parser.add_argument('--commit', required=True)
    parser.add_argument('--repository', required=True)
    args = parser.parse_args()
    expected = companion_release.metadata(args.tag)
    if expected['commit'] != args.commit:
        raise ValueError('Publisher checkout differs from the tag event commit')
    publish(
        GitHub(args.repository, os.environ.get('GH_TOKEN', '')),
        args.directory,
        args.tag,
        args.commit,
        expected,
    )
