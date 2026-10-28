# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE.
"""Fail closed if the standalone external decoder loses checks with Python -O."""
import ast
from pathlib import Path
import subprocess
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]


class OptimizedStandaloneAcceptance(unittest.TestCase):
    def test_decoder_has_no_removable_acceptance_asserts(self):
        source = (ROOT / 'tests' / 'validate_standalone.py').read_text(encoding='utf-8')
        tree = ast.parse(source)
        self.assertFalse(any(isinstance(node, ast.Assert) for node in ast.walk(tree)))

    def test_injected_acceptance_error_survives_python_optimization(self):
        run = subprocess.run([sys.executable, '-O', '-c',
                              'from validation_support import require; '
                              'require(False, "synthetic bad decoder output")'],
                             cwd=ROOT / 'tests', capture_output=True, text=True)
        self.assertNotEqual(run.returncode, 0)
        self.assertIn('synthetic bad decoder output', run.stderr)


if __name__ == '__main__':
    unittest.main()
