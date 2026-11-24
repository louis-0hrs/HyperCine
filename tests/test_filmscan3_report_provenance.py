#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
"""Exercise the FilmScan3 report writer with synthetic private CLI paths."""
from contextlib import redirect_stdout
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

import validate_filmscan3 as filmscan3
from validation_support import portable_command


SENTINEL = 'SYNTHETIC_PRIVATE_VALUE'
VALIDATOR = Path(filmscan3.__file__).resolve()


class FilmScan3ReportProvenance(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name) / SENTINEL
        self.root.mkdir()
        self.report = self.root / 'report.json'
        self.output = self.root / 'output'
        self.probe = self.root / 'probe'
        self.encoder = self.root / 'encoder'
        self.encoder.write_bytes(b'synthetic test binary')
        self.arguments = ['--probe', str(self.probe), f'--encoder={self.encoder}',
                          '--report', str(self.report), '--output-dir', str(self.output)]

    def write_report(self, arguments, compiler):
        # Only the comparison and optional private inventory are replaced.
        # The production entry point parses argv and serializes its report.
        with patch.object(sys, 'argv', [str(self.root / VALIDATOR.name), *arguments]), \
                patch.dict(os.environ, {'HYPERCINE_TEST_COMPILER': compiler}), \
                patch.object(filmscan3.gate, 'inspect_inventory', return_value={'status': 'PASS'}), \
                patch.object(filmscan3, 'compare_run', return_value={'frames': [], 'status': 'PASS'}) as compare, \
                redirect_stdout(io.StringIO()):
            filmscan3.main()
        encoded = self.report.read_text(encoding='utf-8')
        self.assertNotIn(SENTINEL, encoded)
        result = json.loads(encoded)
        self.assertNotIn('command', result)
        return result, compare.call_args.args

    def test_written_report_uses_only_placeholders_and_canonical_geometry(self):
        cpu_encoder = self.root / 'cpu-encoder'
        cpu_encoder.write_bytes(b'synthetic comparison binary')
        corpus = self.root / 'corpus'
        ffmpeg = self.root / 'ffmpeg'
        result, compare_args = self.write_report(
            [*self.arguments, '--cpu-encoder', str(cpu_encoder), f'--corpus-root={corpus}',
             '--ffmpeg', str(ffmpeg), '--geometry=03840x02160', '--sequence', '--inject-metadata'],
            f'AppleClang 21.0.0 ({self.root / "compiler"})',
        )
        self.assertEqual(result['command_template'], [
            'python3', 'tests/validate_filmscan3.py', '--probe', '${HYPERCINE_PROBE}',
            '--encoder', '${HYPERCINE_ENCODER}', '--report', '${HYPERCINE_REPORT}',
            '--output-dir', '${HYPERCINE_OUTPUT_DIR}', '--cpu-encoder', '${HYPERCINE_CPU_ENCODER}',
            '--corpus-root', '${HYPERCINE_GOLDEN_DIR}', '--ffmpeg', '${HYPERCINE_FFMPEG}',
            '--geometry', '3840x2160', '--sequence', '--inject-metadata',
        ])
        self.assertEqual(result['compiler'], 'AppleClang 21.0.0')
        self.assertEqual(compare_args[3:], (corpus, self.output.resolve(), ffmpeg, True, cpu_encoder, (3840, 2160)))

    def test_default_geometry_and_unqualified_compiler(self):
        result, compare_args = self.write_report(self.arguments, str(self.root / 'compiler'))
        self.assertNotIn('--geometry', result['command_template'])
        self.assertEqual(result['compiler'], 'NOT_RECORDED')
        self.assertEqual(compare_args[-1], (1920, 1080))

    def test_invalid_arguments_are_rejected_before_report_or_encode(self):
        for extra, reason in (
            ([f'--{SENTINEL}', str(self.root)], 'invalid FilmScan3 arguments'),
            (['--geometr', '1920x1080'], 'invalid FilmScan3 arguments'),
            ([f'--sequence={SENTINEL}'], 'invalid FilmScan3 arguments'),
            (['--geometry', f'1920x1080{SENTINEL}'], 'expects exactly WIDTHxHEIGHT'),
            (['--geometry=1920x0'], 'width and height must be positive'),
            (['--geometry=99999999999x2160'], 'exceeds the encoder range'),
        ):
            with self.subTest(reason=reason):
                python = [sys.executable, '-B', *(['-O'] if sys.flags.optimize else [])]
                run = subprocess.run([*python, str(VALIDATOR), *self.arguments, *extra],
                                     text=True, capture_output=True, timeout=15)
                self.assertEqual(run.returncode, 2)
                self.assertIn(reason, run.stderr)
                self.assertNotIn(SENTINEL, run.stderr + run.stdout)
                self.assertFalse(self.report.exists())
                self.assertFalse(self.output.exists())

    def test_template_allowlist_rejects_invalid_geometry_and_unknown_option(self):
        self.assertEqual(portable_command(VALIDATOR.name, ['--geometry', '001920x01080'])[-2:],
                         ['--geometry', '1920x1080'])
        for arguments in (['--geometry'], [f'--geometry={SENTINEL}'], ['--unknown', SENTINEL]):
            with self.subTest(arguments=arguments), self.assertRaises(ValueError):
                portable_command(VALIDATOR.name, arguments)


if __name__ == '__main__':
    unittest.main()
