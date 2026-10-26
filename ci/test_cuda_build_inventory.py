#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
"""Negative controls for the CUDA-ON build-inventory gate.

The gate exists to catch coverage that disappears while the build stays green.
These tests build a synthetic build tree in which every artefact is present and
then remove exactly one thing at a time, so each measurement is shown to fail
on the condition it claims to cover -- and to stay green otherwise.
"""
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest


HERE = Path(__file__).resolve().parent
CHECKER = HERE / "check_cuda_build_inventory.py"

BENCHMARKS = (
    "benchmark_wavelet", "benchmark_phase1b", "benchmark_phase1c",
    "benchmark_phase2a", "benchmark_phase2b", "benchmark_phase2c",
    "benchmark_phase2d", "benchmark_phase3a", "benchmark_phase3a1",
    "profile_encoder", "benchmark_codec",
)
CUDA_TESTS = (
    "test_entropy_capacity", "test_entropy_parallel", "test_hybrid_roundtrip",
    "test_phase2a", "test_phase2b", "test_phase2c", "test_phase2d",
)
# Registered-but-not-in-CUDA_TESTS; the gate counts the total, so these matter.
OTHER_TESTS = (
    'benchmark_artifact_contract',
    'benchmark_phase1b_artifact_contract',
    'benchmark_wavelet_artifact_contract',
    'cpu_isa_selection',
    'decode_task_graph_workers_1',
    'decode_task_graph_workers_16',
    'decode_task_graph_workers_2',
    'decode_task_graph_workers_4',
    'gop2_original_stage_contract',
    'gop2_reference_inventory_integrity',
    'scheduler_contract_195_workers_1',
    'scheduler_contract_195_workers_16',
    'scheduler_contract_195_workers_2',
    'scheduler_contract_195_workers_4',
    'test_avi_sample',
    'test_codec_roundtrip',
    'test_cuda_pyramid',
    'test_cuda_wavelet',
    'test_diagnostic_geometry_guard',
    'test_phase3a',
    'test_reference_oracle',
    'test_reference_release_inventory',
    'test_transform_wavelet',
    'test_yuy2_stability',
)


def other_tests():
    """The 24 registered tests that are not part of CUDA_TESTS (31 total)."""
    return OTHER_TESTS


def test_line(index, name):
    """Reproduce ctest's right-aligned numbering.

    Single-digit ids are printed with two spaces (`Test  #1:`), multi-digit
    with one (`Test #10:`). A pattern that assumes one space silently loses
    every single-digit test -- the defect this parser was rewritten for.
    """
    pad = " " if index < 10 else ""
    return "  Test {}#{}: {}".format(pad, index, name)


class InventoryGateTests(unittest.TestCase):
    def setUp(self):
        work = tempfile.TemporaryDirectory()
        self.addCleanup(work.cleanup)
        self.root = Path(work.name)
        (self.root / "benchmarks").mkdir()
        (self.root / "tests").mkdir()

        for name in BENCHMARKS:
            self._executable(self.root / "benchmarks" / name)
        for name in CUDA_TESTS:
            self._executable(self.root / "tests" / name)

        # 129 compile objects across 126 unique sources (three dual entries, as
        # the real configuration has for a header-only TU used twice).
        sources = ["src/unit_{:03d}.cpp".format(i) for i in range(126)]
        entries = [{"file": s} for s in sources]
        entries.extend({"file": sources[0]}, )
        (self.root / "compile_commands.json").write_text(
            json.dumps(entries), encoding="utf-8")

        self._log(129, errors=0)

        self.names = sorted(other_tests()) + list(CUDA_TESTS)
        self._ctestfile(self.names)

    def _executable(self, path):
        path.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
        path.chmod(0o755)

    def _log(self, objects, errors=0):
        lines = []
        for i in range(objects):
            lines.append("[{}/{}] Building CXX object src/unit.o".format(i + 1, objects))
        lines.append("[{}/{}] Linking CXX executable benchmarks/benchmark_codec".format(
            objects + 1, objects + 1))
        lines.extend("error: synthetic failure" for _ in range(errors))
        (self.root / "build.log").write_text("\n".join(lines) + "\n",
                                             encoding="utf-8")

    def _ctestfile(self, names, total=None):
        body = "\n".join('add_test({} "/bin/true")'.format(n) for n in names)
        (self.root / "CTestTestfile.cmake").write_text(body + "\n",
                                                       encoding="utf-8")
        # ctest is not configured here, so the listing is supplied through a
        # shim that emits exactly what ctest -N emits for this test file.
        listing = ["Test project " + str(self.root)]
        for i, name in enumerate(names, start=1):
            listing.append(test_line(i, name))
        listing.append("")
        listing.append("Total Tests: {}".format(len(names) if total is None else total))
        if os.name == 'nt':
            # Windows cannot execute a POSIX shebang (WinError193). Keep the
            # real checker subprocess and all its negative controls; only the
            # synthetic ctest provider uses a native launcher.
            shim = self.root / 'ctest-shim.cmd'
            script = self.root / 'ctest-shim.py'
            script.write_text('from pathlib import Path\n'
                              'print(Path(__file__).with_suffix(".txt").read_text(encoding="utf-8"), end="")\n',
                              encoding='utf-8')
            script.with_suffix('.txt').write_text('\n'.join(listing) + '\n', encoding='utf-8')
            shim.write_text('@"{}" "{}"\n'.format(sys.executable, script), encoding='utf-8')
        else:
            shim = self.root / "ctest-shim"
            shim.write_text("#!/bin/sh\ncat <<'CTEST_EOF'\n" + "\n".join(listing)
                            + "\nCTEST_EOF\n", encoding="utf-8")
            shim.chmod(0o755)
        self.ctest = str(shim)

    def run_gate(self, *extra):
        result = subprocess.run(
            [sys.executable, str(CHECKER), "--build-dir", str(self.root),
             "--log", str(self.root / "build.log"), "--ctest", self.ctest,
             *extra],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        return result.returncode, result.stdout

    # -- positive baseline ---------------------------------------------------
    def test_complete_tree_passes(self):
        code, out = self.run_gate()
        self.assertEqual(code, 0, out)
        self.assertIn("31 CTest tests registered", out)

    # -- one removed artefact at a time --------------------------------------
    def test_deregistered_test_fails_even_when_executable_exists(self):
        """The review's required negative control."""
        names = [n for n in self.names if n != "test_phase2d"]
        self._ctestfile(names)
        self.assertTrue((self.root / "tests" / "test_phase2d").is_file(),
                        "executable must still exist for this control to mean anything")
        code, out = self.run_gate()
        self.assertEqual(code, 1, out)
        self.assertIn("has no CTest registration", out)
        self.assertIn("expected at least 31", out)

    def test_missing_test_executable_fails(self):
        (self.root / "tests" / "test_phase2d").unlink()
        code, out = self.run_gate()
        self.assertEqual(code, 1, out)
        self.assertIn("never built", out)

    def test_fewer_compiled_objects_fails(self):
        self._log(100)
        code, out = self.run_gate()
        self.assertEqual(code, 1, out)
        self.assertIn("objects were compiled", out)

    def test_fewer_translation_units_fails(self):
        entries = [{"file": "src/unit_{:03d}.cpp".format(i)} for i in range(100)]
        (self.root / "compile_commands.json").write_text(json.dumps(entries),
                                                        encoding="utf-8")
        code, out = self.run_gate()
        self.assertEqual(code, 1, out)
        self.assertIn("translation units are registered", out)

    def test_build_errors_fail(self):
        self._log(129, errors=1)
        code, out = self.run_gate()
        self.assertEqual(code, 1, out)
        self.assertIn("error line", out)

    # -- parser regression guards -------------------------------------------
    def test_single_digit_tests_are_counted(self):
        """`Test  #1:` must count; the old `Test #[0-9]` pattern lost them."""
        self.assertEqual(test_line(1, "x"), "  Test  #1: x")
        self.assertEqual(test_line(10, "x"), "  Test #10: x")
        sys.path.insert(0, str(HERE))
        import check_cuda_build_inventory as checker
        names, total = checker.parse_ctest_listing(
            "\n".join([test_line(1, "a"), test_line(2, "b"), test_line(10, "c"),
                       "Total Tests: 3"]) + "\n")
        self.assertEqual(len(names), 3)
        self.assertEqual(total, 3)

    def test_parse_disagreement_fails_loudly(self):
        """If ctest's total and the parsed lines disagree, the gate must fail."""
        self._ctestfile(self.names, total=40)
        code, out = self.run_gate()
        self.assertEqual(code, 1, out)
        self.assertIn("output format changed", out)


if __name__ == "__main__":
    unittest.main()
