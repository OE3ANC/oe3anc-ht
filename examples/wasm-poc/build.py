#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build and check a freestanding WASM module; no hardware access."""
import argparse
import hashlib
from pathlib import Path
import subprocess

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--clang', default=str(ROOT / '.deps/ci/emsdk/upstream/bin/clang'))
    parser.add_argument('--node', default='node')
    args = parser.parse_args()
    output = ROOT / 'build/wasm-poc/gain.wasm'
    output.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run([
        args.clang, '--target=wasm32', '-mcpu=mvp', '-std=c11', '-O2',
        '-Wall', '-Wextra', '-Werror', '-nostdlib', '-fno-builtin',
        '-Wl,--no-entry', '-Wl,--export=main', '-Wl,--export=probe',
        '-Wl,--export=buffer', '-Wl,--export=process', '-Wl,--export-memory',
        '-Wl,--initial-memory=65536', '-Wl,--max-memory=65536',
        '-Wl,-z,stack-size=8192', '-Wl,--strip-all',
        str(HERE / 'gain.c'), '-o', str(output),
    ], check=True)
    subprocess.run([args.node, str(HERE / 'check.mjs'), str(output)], check=True)
    data = output.read_bytes()
    print(f'{output}: {len(data)} bytes; SHA-256 {hashlib.sha256(data).hexdigest()}')


if __name__ == '__main__':
    main()
