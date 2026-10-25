#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
"""Fail closed when a CUDA-ON build silently stopped configuring targets.

Issue #233 exists because a defect inside a CUDA-gated source file used to
compile *nowhere*. A zero-exit build proves only that the targets the configure
step created were built. It says nothing about whether a target was created at
all. Narrowing a condition, e.g. turning `if(HYPERCINE_ENABLE_CUDA)` into
`if(HYPERCINE_ENABLE_CUDA AND WIN32)` in `benchmarks/CMakeLists.txt`, makes the
gate greener by deleting precisely the coverage it is meant to provide: the
build succeeds, the log is clean, and ten of eleven benchmark executables are
simply gone.

This is the same false-OKAY class already repaired in #222, #227 and #229, so
the gate asserts the inventory instead of trusting the exit code.

Three independent measurements, because each one is blind to a different way
of losing coverage:

  1. **Built executables** -- a configured target was not built.
  2. **Compiled translation units** -- a source file stopped being compiled
     even though the executables that used to contain it still link.
  3. **Registered CTest tests** -- a test executable still exists, but the
     `add_test()` registration that actually runs it is gone. Before this
     revision the script defined `MIN_REGISTERED_TESTS` and exposed
     `--min-registered-tests` but never read either one, so a configuration
     with fewer registered tests still printed "inventory complete".

Usage:
  python3 ci/check_cuda_build_inventory.py --build-dir build-cuda --log build.log
"""
import argparse
import json
import os
import re
import subprocess
import sys

# benchmarks/CMakeLists.txt: created inside if(HYPERCINE_ENABLE_CUDA).
CUDA_BENCHMARKS = (
    'benchmark_wavelet', 'benchmark_phase1b', 'benchmark_phase1c',
    'benchmark_phase2a', 'benchmark_phase2b', 'benchmark_phase2c',
    'benchmark_phase2d', 'benchmark_phase3a', 'benchmark_phase3a1',
    'profile_encoder',
)
# Created unconditionally, but part of the same artifact contract.
ALWAYS_BENCHMARKS = ('benchmark_codec',)
# tests/CMakeLists.txt: the CUDA-gated suites, including the five #230 include
# translation units whose omission this gate is partly built to catch.
CUDA_TESTS = (
    'test_entropy_capacity', 'test_entropy_parallel', 'test_hybrid_roundtrip',
    'test_phase2a', 'test_phase2b', 'test_phase2c', 'test_phase2d',
)

# Floors, not equalities: adding targets must never fail the gate. They are
# measured, not estimated, on the Linux CUDA-ON configuration
# (ubuntu-24.04, GCC, nvcc 12.4, CMake 3.31, Release) with the exact flags the
# workflow passes:
#
#   compiled objects        129   (45 C + 78 CXX + 6 CUDA, from the build log)
#   compiled TUs            126   (unique source files in compile_commands.json)
#   registered CTest tests   31   (ctest -N, Linux CUDA-ON)
#
# The previous revision documented "167 TUs / 22 tests". Neither number
# described the configuration: 167 counted every Ninja edge including links and
# custom commands, and 22 came from matching `Test #[0-9]`, which silently
# skips ctest's right-aligned single-digit lines (`Test  #1:`). Both metrics
# are now derived from the artefact that actually carries the answer, and the
# ctest parser asserts that its own line count agrees with ctest's reported
# total so a future format change fails loudly instead of undercounting again.
MIN_COMPILED_OBJECTS = 129
MIN_COMPILED_TUS = 126
MIN_REGISTERED_TESTS = 31

# ctest -N prints `  Test #10: name` and `  Test  #1: name`: the number is
# right-aligned, so single-digit ids carry an extra space.
TEST_LINE = re.compile(r'^\s*Test\s+#(\d+):\s*(.+?)\s*$')
TOTAL_TESTS_LINE = re.compile(r'^\s*Total Tests:\s*(\d+)\s*$')
# Ninja prints `[68/167] Building CXX object path/to/file.o`. Only compile
# edges count as compiled objects; links and custom commands do not match.
OBJECT_LINE = re.compile(r'^\[\d+/\d+\] Building (?:C|CXX|CUDA) object ')


def built_executable(build_dir, subdir, name):
    for candidate in (name, name + '.exe'):
        path = os.path.join(build_dir, subdir, candidate)
        if os.path.isfile(path) and os.access(path, os.X_OK):
            return path
    return None


def parse_ctest_listing(text):
    """Return (names, total) from `ctest -N` output.

    `total` is what ctest itself reports, `names` is what the line pattern
    found. They must agree; a disagreement means the output format moved and
    this gate would silently undercount -- the defect it exists to catch.
    """
    names = []
    total = None
    for line in text.splitlines():
        match = TEST_LINE.match(line)
        if match:
            names.append(match.group(2))
            continue
        match = TOTAL_TESTS_LINE.match(line)
        if match:
            total = int(match.group(1))
    return names, total


def registered_tests(build_dir, ctest='ctest'):
    """Run `ctest -N` against the build tree. Returns (names, total, error)."""
    try:
        result = subprocess.run(
            [ctest, '--test-dir', build_dir, '-N'],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            text=True, timeout=600)
    except (OSError, subprocess.SubprocessError) as exc:
        return [], None, 'cannot run ctest on {}: {}'.format(build_dir, exc)
    if result.returncode != 0:
        return [], None, 'ctest -N exited {}: {}'.format(
            result.returncode, (result.stdout or '').strip()[:400])
    names, total = parse_ctest_listing(result.stdout)
    if total is None:
        return names, None, "ctest -N printed no 'Total Tests:' line"
    return names, total, None


def compiled_tus(build_dir):
    """Unique source files the configure step registered for compilation.

    Derived from `compile_commands.json` rather than from Ninja progress lines,
    which also cover links and custom commands and therefore cannot be called
    translation units.
    """
    path = os.path.join(build_dir, 'compile_commands.json')
    try:
        with open(path, encoding='utf-8') as handle:
            entries = json.load(handle)
    except (OSError, ValueError) as exc:
        return None, 'cannot read {}: {}'.format(path, exc)
    if not isinstance(entries, list):
        return None, '{} is not a compile-commands list'.format(path)
    files = {entry.get('file') for entry in entries
             if isinstance(entry, dict) and entry.get('file')}
    if not files:
        return None, '{} lists no source files'.format(path)
    return len(files), None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--build-dir', required=True)
    ap.add_argument('--log', required=True)
    ap.add_argument('--min-compiled-objects', type=int,
                    default=MIN_COMPILED_OBJECTS)
    ap.add_argument('--min-compiled-tus', type=int, default=MIN_COMPILED_TUS)
    ap.add_argument('--min-registered-tests', type=int,
                    default=MIN_REGISTERED_TESTS)
    ap.add_argument('--ctest', default='ctest',
                    help='ctest executable to probe (default: ctest)')
    args = ap.parse_args()

    failures = []

    # 1) Built executables.
    missing = []
    for name in CUDA_BENCHMARKS + ALWAYS_BENCHMARKS:
        if not built_executable(args.build_dir, 'benchmarks', name):
            missing.append('benchmarks/' + name)
    for name in CUDA_TESTS:
        if not built_executable(args.build_dir, 'tests', name):
            missing.append('tests/' + name)
    if missing:
        failures.append(
            'configured-target inventory is incomplete: {} of {} expected '
            'executables were never built: {}'.format(
                len(missing),
                len(CUDA_BENCHMARKS) + len(ALWAYS_BENCHMARKS) + len(CUDA_TESTS),
                ', '.join(missing)))

    # 2) Build log: errors, and the number of objects actually compiled.
    try:
        with open(args.log, encoding='utf-8', errors='replace') as handle:
            lines = handle.read().splitlines()
    except OSError as exc:
        print('FAIL: CUDA-ON build inventory check')
        print('  - cannot read build log: {}'.format(exc))
        return 1

    errors = [line for line in lines if 'error:' in line]
    if errors:
        failures.append('build log contains {} error line(s)'.format(len(errors)))

    compiled = sum(1 for line in lines if OBJECT_LINE.match(line))
    if compiled < args.min_compiled_objects:
        failures.append(
            'only {} objects were compiled, expected at least {}: a source '
            'file stopped being compiled rather than being repaired'.format(
                compiled, args.min_compiled_objects))

    # 3) Registered CTest tests -- existence of an executable is not
    #    registration, and only registration makes a test actually run.
    names, total, error = registered_tests(args.build_dir, args.ctest)
    if error:
        failures.append('test-registration inventory unavailable: ' + error)
    else:
        if total != len(names):
            failures.append(
                'ctest reports {} registered tests but only {} could be '
                'parsed: the ctest -N output format changed, so this gate '
                'cannot count registrations reliably'.format(total, len(names)))
        if total < args.min_registered_tests:
            failures.append(
                'only {} CTest tests are registered, expected at least {}: a '
                'test was removed from the registration inventory'.format(
                    total, args.min_registered_tests))
        for name in CUDA_TESTS:
            if name not in names:
                failures.append(
                    'CUDA test {} has no CTest registration: its executable '
                    'may exist, but nothing would run it'.format(name))

    # 4) Compiled translation units, independent of the log.
    tus, error = compiled_tus(args.build_dir)
    if error:
        failures.append('translation-unit inventory unavailable: ' + error)
    elif tus < args.min_compiled_tus:
        failures.append(
            'only {} translation units are registered for compilation, '
            'expected at least {}: a source file was dropped from the '
            'configuration'.format(tus, args.min_compiled_tus))

    if failures:
        print('FAIL: CUDA-ON build inventory check')
        for item in failures:
            print('  - ' + item)
        return 1

    print('PASS: CUDA-ON build inventory complete '
          '({} benchmark executables, {} test executables, {} objects '
          'compiled, {} translation units registered, {} CTest tests '
          'registered, 0 error lines)'.format(
              len(CUDA_BENCHMARKS) + len(ALWAYS_BENCHMARKS),
              len(CUDA_TESTS), compiled, tus, total))
    return 0


if __name__ == '__main__':
    sys.exit(main())
