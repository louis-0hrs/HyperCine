#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE.
"""Identical native Mac recipes for hosted Actions and the private VM relay."""
import argparse
import os
import platform
import subprocess

LANES = ('standalone-release', 'standalone-sanitizers', 'quality-state')


def commands(lane):
    flags = ['-DCMAKE_OSX_ARCHITECTURES=arm64']
    if lane == 'quality-state':
        return [
            ['cmake', '-S', '.', '-B', 'build', '-DCMAKE_BUILD_TYPE=Release',
             '-DHYPERCINE_BUILD_QUALITY_STATE_ONLY=ON', *flags],
            ['cmake', '--build', 'build', '--config', 'Release', '--parallel', '2'],
            ['ctest', '--test-dir', 'build', '-C', 'Release', '--output-on-failure', '--no-tests=error'],
        ]
    config = 'Release' if lane == 'standalone-release' else 'RelWithDebInfo'
    if lane == 'standalone-sanitizers':
        flags += ['-DCMAKE_CXX_FLAGS=-fsanitize=address,undefined -fno-omit-frame-pointer',
                  '-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined']
    return [
        ['cmake', '-S', '.', '-B', 'build', '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=' + config,
         '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON', '-DHYPERCINE_BUILD_STANDALONE=ON',
         '-DHYPERCINE_BUILD_UPSTREAM=OFF', '-DHYPERCINE_ENABLE_CUDA=OFF',
         '-DHYPERCINE_BUILD_TESTS=ON', '-DHYPERCINE_BUILD_BENCHMARKS=OFF',
         '-DHYPERCINE_REQUIRE_EXTERNAL_DECODE=ON', *flags],
        ['cmake', '--build', 'build', '--parallel', '2'],
        ['python3', 'ci/test_standalone_gate.py'],
        ['python3', 'ci/run_standalone.py', '--system', 'Darwin', '--machine', 'arm64'],
    ]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--lane', choices=LANES, required=True)
    args = p.parse_args()
    if platform.system() != 'Darwin' or platform.machine() != 'arm64':
        p.error('native macOS arm64 required')
    # Do not inherit filters or private corpus bindings from an operator shell.
    os.environ.pop('HYPERCINE_GOLDEN_DIR', None)
    os.environ.pop('CTEST_TEST_FILTER', None)
    os.environ.update(ASAN_OPTIONS='detect_leaks=0:halt_on_error=1',
                      UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1',
                      CMAKE_BUILD_PARALLEL_LEVEL='2')
    subprocess.run(['python3', 'ci/check_runner_capability.py', '--capability', 'hc-macos-arm64'], check=True)
    for argv in commands(args.lane):
        subprocess.run(argv, check=True)


if __name__ == '__main__':
    main()
