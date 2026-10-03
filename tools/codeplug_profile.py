#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Export or explicitly replace a stopped Linux emulator profile using version 1 JSON."""
import argparse
import copy
import fcntl
import os
from pathlib import Path
import re
import sys
import tempfile

import codeplug
import codeplug_wire as wire


class Profile:
    def __init__(self, directory, name):
        if not re.fullmatch(r'[A-Za-z0-9_-]{1,32}', name):
            codeplug.fail('$profile.name', 'expected 1..32 ASCII letters, digits, - or _')
        self.directory = Path(directory)
        if not directory or len(os.fsencode(directory)) >= 384:
            codeplug.fail('$profile.directory', 'expected 1..383 path bytes')
        self.path = self.directory / (name + '.bin')
        self.lock_path = self.directory / (name + '.bin.lock')
        self.lock_fd = None

    def __enter__(self):
        if self.lock_fd is not None:
            codeplug.fail('$profile', 'lock already held by this context')
        self.directory.mkdir(mode=0o700, exist_ok=True)
        fd = os.open(self.lock_path, os.O_CREAT | os.O_RDWR | os.O_CLOEXEC, 0o600)
        try:
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BaseException:
            os.close(fd)
            raise
        self.lock_fd = fd
        return self

    def __exit__(self, *ignored):
        os.close(self.lock_fd)
        self.lock_fd = None

    def load(self):
        if self.lock_fd is None:
            codeplug.fail('$profile', 'acquire profile lock before reading')
        try:
            with self.path.open('rb') as stream:
                data = stream.read(wire.MAX_PROFILE + 1)
            return wire.decode(data)
        except FileNotFoundError:
            return None, 0

    def protect_output(self, output):
        output = Path(output)
        directory = self.directory.resolve()
        for candidate in (output, output.resolve()):
            if candidate.parent.resolve() == directory and re.fullmatch(
                r'[A-Za-z0-9_-]{1,32}\.bin(?:\.lock)?', candidate.name
            ):
                codeplug.fail('$output', 'must not overwrite any profile or lock file')
        reserved_paths = [self.path, self.lock_path]
        for pattern in ('*.bin', '*.bin.lock'):
            reserved_paths.extend(self.directory.glob(pattern))
        for reserved in reserved_paths:
            if output.resolve() == reserved.resolve() or (
                output.exists() and reserved.exists() and os.path.samefile(output, reserved)
            ):
                codeplug.fail('$output', 'must not alias any profile or lock file')


def atomic_write(path, data):
    """One durable replacement; callers own the profile lock when appropriate."""
    path = Path(path)
    fd, temporary = tempfile.mkstemp(prefix=path.name + '.tmp.', dir=path.parent)
    published = False
    try:
        with os.fdopen(fd, 'wb') as stream:
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
        published = True
        directory = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC)
        try:
            os.fsync(directory)
        finally:
            os.close(directory)
    except OSError as error:
        if published:
            raise OSError(
                f'{path}: replacement is visible; durability uncertain: {error}'
            ) from error
        raise
    finally:
        if not published:
            Path(temporary).unlink(missing_ok=True)


def validate_backend(document):
    # Both implemented targets share the contract's bands, 5 W ceiling, FM,
    # CTCSS/DCS and M17 capabilities; neither currently exposes adjustable gain.
    codeplug.validate(document)
    if document['global']['gain']:
        codeplug.fail('$.global.gain', 'unsupported by the emulator backend (must be 0)')


def replace(profile, document, dry_run=False):
    validate_backend(document)
    if profile.lock_fd is None:
        codeplug.fail('$profile', 'acquire profile lock before replacement')
    previous, generation = profile.load()
    if generation == codeplug.MAX_ID:
        codeplug.fail('$profile.generation', 'exhausted; replacement cannot wrap')
    replacement = copy.deepcopy(codeplug.validate(document))
    if previous:
        for key in replacement['allocation']:
            replacement['allocation'][key] = max(
                previous['allocation'][key], replacement['allocation'][key]
            )
    data = wire.encode(replacement, generation + 1)
    if not dry_run:
        atomic_write(profile.path, data)
    return replacement, generation + 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=('export', 'import'))
    parser.add_argument('input', nargs='?', type=Path, help='JSON to import')
    parser.add_argument('--profile', required=True, help='explicit emulator profile name')
    parser.add_argument('--directory', default=os.getenv('HT_SETTINGS_DIR', 'ht-settings'))
    parser.add_argument('--output', type=Path, help='JSON export destination')
    parser.add_argument(
        '--replace', action='store_true', help='explicitly replace the selected profile'
    )
    parser.add_argument(
        '--dry-run',
        action='store_true',
        help='validate import and reconcile counters without publication',
    )
    args = parser.parse_args()
    if args.action == 'import':
        if args.input is None or args.output is not None or not (args.replace or args.dry_run):
            parser.error('import requires an input and --replace or --dry-run; accepts no --output')
    elif args.input is not None or args.output is None or args.replace or args.dry_run:
        parser.error('export requires --output; accepts no input, --replace or --dry-run')
    try:
        # Validate the entire import before creating directories/lock metadata.
        document = codeplug.load(args.input) if args.action == 'import' else None
        if document is not None:
            validate_backend(document)
        with Profile(args.directory, args.profile) as profile:
            if args.action == 'export':
                profile.protect_output(args.output)
                document, generation = profile.load()
                if document is None:
                    codeplug.fail('$profile', 'no codeplug to export')
                atomic_write(args.output, codeplug.canonical(document))
                action = 'Exported'
            else:
                document, generation = replace(profile, document, args.dry_run)
                action = 'Dry-run' if args.dry_run else 'Replaced'
        print(
            f'{action} profile {args.profile}: generation {generation}, '
            f'{len(document["channels"])} channels, {len(document["banks"])} banks; '
            f'ID high-water {document["allocation"]}'
        )
    except BlockingIOError:
        print(
            f'Profile {args.profile} is busy; stop its emulator before transfer.', file=sys.stderr
        )
        return 1
    except (OSError, codeplug.InvalidCodeplug) as error:
        print(error, file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
