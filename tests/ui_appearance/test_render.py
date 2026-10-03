#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Do not publish an overview when native ztest exits zero after a failure."""
import contextlib
import io
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
import render


class RenderResultTests(unittest.TestCase):
    def test_failed_or_missing_test_result_does_not_publish(self):
        for text, code in (
            ("PROJECT EXECUTION FAILED", 0),
            ("no final result", 0),
            ("PROJECT EXECUTION SUCCESSFUL\nPROJECT EXECUTION FAILED", 0),
            ("PROJECT EXECUTION SUCCESSFUL", 1),
        ):
            with self.subTest(text=text, code=code), tempfile.TemporaryDirectory() as directory:
                output = Path(directory) / "output"
                result = subprocess.CompletedProcess([], code, stdout=text)
                with patch.object(
                    sys, "argv", ["render.py", "--output", str(output)]
                ), patch.object(
                    render.subprocess, "run", return_value=result
                ), contextlib.redirect_stdout(
                    io.StringIO()
                ):
                    with self.assertRaisesRegex(RuntimeError, "checks failed"):
                        render.main()
                self.assertEqual(list(output.glob("*.png")), [])


if __name__ == "__main__":
    unittest.main()
