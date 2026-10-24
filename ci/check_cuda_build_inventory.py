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

Usage:
  python3 ci/check_cuda_build_inventory.py --build-dir build-cuda --log build.log
"""
import argparse
import os
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

# Floors, not equalities: adding targets must not fail the gate, removing them
# must. The Linux CUDA-ON configuration measured at #233 is 167 TUs / 22 tests.
MIN_TRANSLATION_UNITS = 165
MIN_REGISTERED_TESTS = 22


def built_executable(build_dir, subdir, name):
    for candidate in (name, name + '.exe'):
        path = os.path.join(build_dir, subdir, candidate)
        if os.path.isfile(path) and os.access(path, os.X_OK):
            return path
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--build-dir', required=True)
    ap.add_argument('--log', required=True)
    ap.add_argument('--min-translation-units', type=int,
                    default=MIN_TRANSLATION_UNITS)
    ap.add_argument('--min-registered-tests', type=int,
                    default=MIN_REGISTERED_TESTS)
    args = ap.parse_args()

    failures = []

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

    tus = sum(1 for line in lines
              if line.startswith('[') and ']' in line and '/' in line)
    if tus < args.min_translation_units:
        failures.append(
            'only {} translation units compiled, expected at least {}: a target '
            'was removed from the configuration rather than repaired'.format(
                tus, args.min_translation_units))

    if failures:
        print('FAIL: CUDA-ON build inventory check')
        for item in failures:
            print('  - ' + item)
        return 1

    print('PASS: CUDA-ON build inventory complete '
          '({} benchmark executables, {} test executables, {} translation units, '
          '0 error lines)'.format(
              len(CUDA_BENCHMARKS) + len(ALWAYS_BENCHMARKS),
              len(CUDA_TESTS), tus))
    return 0


if __name__ == '__main__':
    sys.exit(main())
