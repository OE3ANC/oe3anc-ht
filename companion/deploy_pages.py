#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Stage published, verified companion archives for GitHub Pages without rebuilding."""
import argparse
import hashlib
import io
import json
import os
from pathlib import Path, PurePosixPath
import re
import stat
import sys
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from publish_release import GitHub, check_tag
from release_packages import MANIFEST, MAX_PACKAGE, canonical, validate_manifest


def version(tag):
    match = re.fullmatch(r'v([0-9]+)\.([0-9]+)\.([0-9]+)(?:-rc\.([0-9]+))?', tag)
    if not match:
        raise ValueError(
            f'Invalid version tag {tag!r}; expected vMAJOR.MINOR.PATCH '
            'or vMAJOR.MINOR.PATCH-rc.NUMBER'
        )
    major, minor, patch, candidate = match.groups()
    return int(major), int(minor), int(patch), candidate is None, int(candidate or 0)


def archive_files(data, tag, release, base_path):
    files = {}
    size = 0
    with zipfile.ZipFile(io.BytesIO(data)) as archive:
        for entry in archive.infolist():
            path = PurePosixPath(entry.filename)
            size += entry.file_size
            if (
                size > MAX_PACKAGE
                or entry.filename != path.as_posix()
                or path.is_absolute()
                or '..' in path.parts
                or '\\' in entry.filename
                or len(path.parts) < 2
                or path.parts[0] != tag
                or entry.is_dir()
                or entry.filename in files
                or stat.S_IFMT(entry.external_attr >> 16) not in (0, stat.S_IFREG)
            ):
                raise ValueError('Unsafe or oversized companion archive')
            files[entry.filename] = archive.read(entry)
    required = (
        'index.html',
        'style.css',
        'release.json',
        'src/app.mjs',
        'src/release.mjs',
        'src/site.mjs',
        'render/ht-ui.mjs',
        'render/ht-ui.wasm',
        'bootloader/burner_venus.bin',
    )
    if any(tag + '/' + name not in files for name in required):
        raise ValueError('Incomplete companion archive')
    if json.loads(files[tag + '/release.json']) != release:
        raise ValueError('Archived companion identity mismatch')
    # Generated modules are data declarations; parse them without executing archive code.
    for name, expected in (('release', release), ('site', {'basePath': base_path})):
        text = files[tag + '/src/' + name + '.mjs'].decode()
        prefix = 'export const ' + name + ' = '
        if (
            not text.startswith(prefix)
            or not text.endswith(';\n')
            or json.loads(text[len(prefix) : -2]) != expected
        ):
            raise ValueError('Archived companion module or hosting base mismatch')
    return files


def release_files(api, release, base_path):
    tag, commit = release['tag_name'], release['target_commitish']
    version(tag)
    assets = {}
    for asset in api.listing('/releases/' + str(release['id']) + '/assets'):
        if asset['name'] in assets:
            raise ValueError('Duplicate release asset names')
        assets[asset['name']] = asset

    def download(name):
        asset = assets.get(name)
        if not asset or asset['state'] != 'uploaded' or not 0 < asset['size'] <= MAX_PACKAGE:
            raise ValueError('Missing or invalid release asset: ' + name)
        data = api.download(asset)
        if len(data) != asset['size']:
            raise ValueError('Release asset size mismatch: ' + name)
        return data

    data = download(MANIFEST)
    if len(data) > 65536:
        raise ValueError('Oversized release manifest')
    manifest = validate_manifest(json.loads(data), tag, commit)
    binding = '<!-- oe3anc-ht-pair:' + hashlib.sha256(data).hexdigest() + ' -->'
    if (
        data != canonical(manifest)
        or binding not in (release['body'] or '')
        or release['draft']
        or 'Pair status: COMPLETE' not in (release['body'] or '')
    ):
        raise ValueError('Release is not a published, complete, bound pair')
    check_tag(api, tag, commit)
    companion = None
    for package in manifest['packages']:
        content = download(package['name'])
        if (
            len(content) != package['size']
            or hashlib.sha256(content).hexdigest() != package['sha256']
        ):
            raise ValueError('Release package checksum mismatch: ' + package['name'])
        if package['role'] == 'companion':
            companion = content
    files = archive_files(companion, tag, manifest['release'], base_path)
    current = api.json('GET', '/releases/' + str(release['id']))
    if (
        current['draft']
        or current['tag_name'] != tag
        or current['target_commitish'] != commit
        or binding not in (current['body'] or '')
        or 'Pair status: COMPLETE' not in (current['body'] or '')
    ):
        raise ValueError('Release changed while staging the website')
    check_tag(api, tag, commit)
    return files


def assemble(api, output, base_path, required_release=None, site_base_path=None):
    site_base_path = base_path if site_base_path is None else site_base_path
    for path in (base_path, site_base_path):
        if not re.fullmatch(r'/(?:[A-Za-z0-9_-]+/)*', path):
            raise ValueError('Invalid Pages base path')
    if not base_path.startswith(site_base_path):
        raise ValueError('Archive base path is outside the Pages hosting root')
    # Mount unchanged archives at their compiled URL base, including on a custom domain.
    mount = base_path[len(site_base_path) :]
    # Build the complete deployment from retained release assets, never from expiring CI artifacts.
    releases = [
        release
        for release in api.listing('/releases')
        if not release['draft'] and '<!-- oe3anc-ht-pair:' in (release['body'] or '')
    ]
    if not releases or (
        required_release is not None
        and not any(release['id'] == required_release for release in releases)
    ):
        raise ValueError('No published paired release to deploy')
    if output.exists() and any(output.iterdir()):
        raise ValueError('Pages staging directory must be empty')
    for release in releases:
        print('Published companion release:', release['id'], repr(release['tag_name']), flush=True)
    releases.sort(key=lambda release: version(release['tag_name']))
    tags = set()
    for release in releases:
        tag = release['tag_name']
        if tag in tags:
            raise ValueError('Multiple releases use this tag')
        tags.add(tag)
        for name, content in release_files(api, release, base_path).items():
            path = output / mount / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(content)
    latest = releases[-1]['tag_name']
    destination = './' + mount + latest + '/'
    (output / 'index.html').write_text(
        '<!doctype html>\n<html lang="en">\n<meta charset="utf-8">\n'
        '<meta name="viewport" content="width=device-width, initial-scale=1">\n'
        f'<meta http-equiv="refresh" content="0; url={destination}">\n'
        '<title>OE3ANC HT companion</title>\n'
        f'<p><a href="{destination}">Open companion {latest}</a></p>\n'
    )
    (output / '.nojekyll').touch()
    print('Pages site ready:', latest, 'with', len(releases), 'archived companions')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repository', required=True)
    parser.add_argument('--base-path', default='/oe3anc-ht/')
    parser.add_argument(
        '--site-base-path', help='Actual Pages root; inferred from repository CNAME'
    )
    parser.add_argument('--output', type=Path, default=Path('build/pages'))
    args = parser.parse_args()
    release_id = os.environ.get('PAGES_RELEASE_ID', '')
    site_base = args.site_base_path
    if site_base is None:
        cname = Path(__file__).resolve().parents[1] / 'CNAME'
        site_base = '/' if cname.is_file() else args.base_path
    assemble(
        GitHub(args.repository, os.environ.get('GH_TOKEN', '')),
        args.output,
        args.base_path,
        int(release_id) if release_id else None,
        site_base,
    )


if __name__ == '__main__':
    main()
