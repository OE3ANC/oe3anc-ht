#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check that Pages serves the staged entrypoints and release identities."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import time
from urllib.error import URLError
from urllib.request import Request, urlopen


def expected_content(site):
    paths = {site / 'index.html'}
    releases = list(site.rglob('release.json'))
    if not releases:
        raise ValueError('No staged companion releases')
    for release in releases:
        paths.update((release, release.parent / 'index.html', release.parent / 'src/site.mjs'))
    return {
        path.relative_to(site).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
        for path in sorted(paths)
    }


def verify(url, expected, timeout=720):
    if not url.startswith('https://') or not expected:
        raise ValueError('HTTPS Pages URL and expected content are required')
    deadline = time.monotonic() + timeout
    pending = dict(expected)
    while pending:
        for path, digest in list(pending.items()):
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                break
            request = Request(
                url.rstrip('/') + '/' + path + '?verify=' + str(time.time_ns()),
                headers={'Cache-Control': 'no-cache'},
            )
            try:
                with urlopen(request, timeout=min(10, remaining)) as response:
                    content = response.read(1024 * 1024 + 1)
                if hashlib.sha256(content).hexdigest() == digest:
                    del pending[path]
            except (URLError, TimeoutError):
                pass  # A missing file or old CDN response may resolve during propagation.
        if not pending:
            print(f'PASS: Pages serves all {len(expected)} expected content checks', flush=True)
            return
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise RuntimeError('Pages did not serve expected content: ' + ', '.join(pending))
        print('Waiting for Pages content: ' + ', '.join(pending), flush=True)
        time.sleep(min(20, remaining))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument('--site', type=Path)
    mode.add_argument('--url')
    args = parser.parse_args()
    if args.site:
        print('checks=' + json.dumps(expected_content(args.site), separators=(',', ':')))
    else:
        verify(args.url, json.loads(os.environ['PAGES_EXPECTED']))


if __name__ == '__main__':
    main()
