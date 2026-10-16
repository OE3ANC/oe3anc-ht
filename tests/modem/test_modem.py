# SPDX-License-Identifier: GPL-3.0-or-later
"""Portable modem regression: python3 -m unittest discover -s tests/modem -v."""
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]


class ModemTests(unittest.TestCase):
    def test_portable_core(self):
        with tempfile.TemporaryDirectory(prefix="ht-modem-") as temporary:
            binary = pathlib.Path(temporary) / "modem-test"
            subprocess.run([
                "c++", "-std=c++14", "-Wall", "-Wextra", "-Werror", "-O2",
                "-fno-exceptions", "-fno-rtti", "-I", str(ROOT / "include"),
                str(ROOT / "modules/modem/kiss.cpp"),
                str(ROOT / "modules/modem/afsk.cpp"),
                str(ROOT / "tests/modem/main.cpp"), "-o", str(binary),
            ], check=True)
            subprocess.run([str(binary)], check=True)
