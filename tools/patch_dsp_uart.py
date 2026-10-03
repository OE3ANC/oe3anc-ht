#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generate the C62 DSP image with its UART logger bypassed.

Only accepts the project's original pinned 48 kHz image and verifies the
patched hash. The user confirmed firmware operation without UART logs on
2026-10-02; detailed FM/M17 bench coverage was not provided.

Inferred from Xtensa disassembly at load address 0x60000000:
  0x461f4: initializes the log ring, creates `log_uart_print`, then calls
           0x46504 -> 0x46384 to configure UART pins/clocks/registers.
           Startup at 0x136f3 calls this with UART 2 and 115200 baud.
  0x461b4: variadic console formatter, including logging from audio services.
  0x46150: console formatter through the installed output callback; also
           reached by WASM printf wrappers at 0x279e8 and 0x27a18.

Preserve each windowed-ABI ENTRY instruction and replace the first six-byte
instruction with MOVI.N a2, 0; RETW.N; NOP.N. This returns before logger setup
or console formatting. Buffer formatting at 0x46170 remains intact. Names are
inferred, since the image has no symbol table. This is not proof that every
possible DSP UART path has been eliminated; verify startup and FM/M17 on target.
"""
import argparse
import hashlib
from pathlib import Path
import sys

SOURCE_SHA256 = '9579b617f048c646465118f6d0b9e1b765b4d7d811ff9c8649c5754fe45a8903'
PATCHED_SHA256 = 'fc0e2d7a8a2a991c831daf798a965743a178a582c6cbab9f9ef6a05c34628bdb'
SOURCE_SIZE = 466832
RETURN_ZERO = bytes.fromhex('0c02 1df0 3df0')
# Offset of ENTRY, its original bytes, and the complete following instruction.
PATCHES = (
    (0x46150, bytes.fromhex('364100 6e0a023c03fd')),
    (0x461B4, bytes.fromhex('36a100 ee0401085bc8')),
    (0x461F4, bytes.fromhex('366100 5ee5192c03e0')),
)


def patch_image(original):
    if len(original) != SOURCE_SIZE or hashlib.sha256(original).hexdigest() != SOURCE_SHA256:
        raise ValueError('input is not the pinned C62 48 kHz DSP image')
    patched = bytearray(original)
    for offset, expected in PATCHES:
        if original[offset : offset + len(expected)] != expected:
            raise ValueError(f'instruction mismatch at 0x{offset:x}')
        patched[offset + 3 : offset + 9] = RETURN_ZERO
    if hashlib.sha256(patched).hexdigest() != PATCHED_SHA256:
        raise ValueError('patched DSP image does not match the hardware-tested hash')
    return bytes(patched)


def self_test(original):
    patched = patch_image(original)
    assert len(patched) == len(original)
    cursor = 0
    for offset, expected in PATCHES:
        assert patched[cursor : offset + 3] == original[cursor : offset + 3]
        assert patched[offset : offset + 9] == expected[:3] + RETURN_ZERO
        cursor = offset + 9
    assert patched[cursor:] == original[cursor:]
    corrupt = bytearray(original)
    corrupt[0] ^= 1
    for invalid in (original[:-1], bytes(corrupt), patched):
        try:
            patch_image(invalid)
        except ValueError:
            pass
        else:
            raise AssertionError('accepted a truncated, changed, or already patched input')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        '--input',
        type=Path,
        default=Path(__file__).resolve().parents[1] / 'backends/c62/resources/dsp_firmware.bin',
    )
    parser.add_argument('--output', type=Path, help='patched image; refuses overwrite by default')
    parser.add_argument(
        '--replace-output',
        action='store_true',
        help='regenerate a build output; cannot replace the original image',
    )
    parser.add_argument('--self-test', action='store_true')
    args = parser.parse_args()
    if not args.self_test and args.output is None:
        parser.error('supply --output or --self-test')
    original = args.input.read_bytes()
    if args.self_test:
        self_test(original)
        print('PASS: patch boundaries, unchanged bytes, and invalid-image rejection')
    if args.output is not None:
        if args.output.resolve() == args.input.resolve() or (
            args.output.exists() and args.output.samefile(args.input)
        ):
            raise ValueError('output must not replace the original DSP image')
        patched = patch_image(original)
        with args.output.open('wb' if args.replace_output else 'xb') as output:
            output.write(patched)
        print(f'C62 DSP firmware without UART logging: {args.output}')
        print(f'SHA-256: {hashlib.sha256(patched).hexdigest()}')


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError) as error:
        print(error, file=sys.stderr)
        sys.exit(1)
