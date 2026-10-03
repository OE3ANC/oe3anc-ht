#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Run a pinned independent DCS encoder to produce the BK4819 test vectors."""
import argparse
import hashlib
from pathlib import Path
import shutil
import subprocess
import tempfile

REVISION = "6f8afac8864e0349ecf8b10f91ce8f69273013db"
SHA256 = "44886c4c512257c2b9f932cc1fe49997ed1cb44684e7f2b28d4543fc7de00295"
URL = f"https://raw.githubusercontent.com/DualTachyon/uv-k5-firmware/{REVISION}/dcs.c"
DESTINATION = Path(__file__).resolve().parents[1] / "tests/bk4819/vectors/dcs.h"


def generate(source: Path) -> str:
    if hashlib.sha256(source.read_bytes()).hexdigest() != SHA256:
        raise ValueError(f"reference SHA-256 mismatch; expected source: {URL}")
    with tempfile.TemporaryDirectory(prefix="ht-dcs-vectors-") as directory:
        root = Path(directory)
        shutil.copyfile(source, root / "reference.c")
        # Only declarations needed by the independent upstream translation unit.
        (root / "dcs.h").write_text(
            "#include <stdint.h>\n"
            "typedef enum { CODE_TYPE_OFF, CODE_TYPE_CONTINUOUS_TONE, "
            "CODE_TYPE_DIGITAL, CODE_TYPE_REVERSE_DIGITAL } DCS_CodeType_t;\n"
        )
        (root / "main.c").write_text(
            '#include <stdio.h>\n#include "reference.c"\n'
            'int main(void) { for (unsigned i=0; i<512; ++i) '
            'printf("%06x\\n", DCS_CalculateGolay(i | 0x800U)); return 0; }\n'
        )
        subprocess.run(
            ["cc", "-std=c99", "-I", str(root), str(root / "main.c"), "-o", str(root / "vectors")],
            check=True,
        )
        words = subprocess.check_output([str(root / "vectors")], text=True).splitlines()
    if len(words) != 512:
        raise ValueError("expected 512 independent vectors")
    header = (
        "/* SPDX-License-Identifier: GPL-3.0-or-later */\n"
        "/* Generated mathematical output of DualTachyon's Apache-2.0 encoder.\n"
        f" * Reference: {URL}\n * SHA-256: {SHA256}\n"
        " * Reproduce using tools/generate_dcs_vectors.py --source <pinned dcs.c>. */\n"
        "#pragma once\n#include <stdint.h>\nstatic const uint32_t dcs_words[512] = {\n"
    )
    for index in range(0, 512, 8):
        header += "    " + ", ".join(f"0x{word}U" for word in words[index : index + 8]) + ",\n"
    return header + "};\n"


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True, help=URL)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    expected = generate(args.source)
    if args.check:
        if DESTINATION.read_text() != expected:
            raise SystemExit("DCS vectors differ; regenerate from pinned source")
    else:
        DESTINATION.parent.mkdir(parents=True, exist_ok=True)
        DESTINATION.write_text(expected)
    print("512 independent DCS vectors verified")
