#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
"""Issue #93: the CPU ISA target must resolve the toolchain's own <cpuid.h>.

The root CMakeLists.txt injects `upstream/Codec` into the header search path for
the SDK consumers. That directory contains GoPro's `cpuid.h`, and GCC's compiler
header uses the same guard symbol, so the frozen SDK header wins and
`__get_cpuid_max` / `__cpuid_count` disappear. PR #94 fixed it by resetting
INCLUDE_DIRECTORIES on the two SDK-free CPU ISA targets only.

That fix is a single `set_property()` pair with no test behind it: deleting it
re-breaks the full Linux SDK build while every configure-only check stays green.
This contract closes that gap. It configures the SDK-enabled build, then:

  * asserts the recorded compile command for `src/cpu/cpu_isa.cpp` and
    `tests/test_cpu_isa.cpp` carries no `upstream/` include directory;
  * compiles that exact recorded command and requires success, so the boundary
    is proven by the compiler rather than by reading a cache variable;
  * runs the same command again with `upstream/Codec` appended and requires the
    original `__get_cpuid_max` failure, so a guard that had become redundant
    (and a test that would therefore be vacuous) is caught;
  * checks that an SDK consumer still carries the upstream include, so an
    over-broad edit that stripped it everywhere fails here.

Configure-and-compile only: one small translation unit, no link of the frozen
archive, no GPU and no corpus.
"""
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

# SDK-free targets that lost the SDK include path in #94.
ISOLATED_SOURCES = ('src/cpu/cpu_isa.cpp', 'tests/test_cpu_isa.cpp')
ISOLATED_TARGETS = ('hypercine_cpu_isa', 'test_cpu_isa')

# A real SDK consumer that must keep the upstream include directories.
SDK_CONSUMER = 'src/core/quality_state.cpp'

UPSTREAM_CODEC = 'upstream/Codec'

# The exact diagnostic the issue reported on GCC/Clang.
CPUID_SYMBOLS = ('__get_cpuid_max', '__cpuid_count')

SKIP_MSVC = unittest.skipIf(
    sys.platform == 'win32',
    'MSVC uses <intrin.h>; the cpuid.h shadowing is a GCC/Clang-only defect')


def _generator_args():
    if shutil.which('ninja'):
        return ['-G', 'Ninja']
    return []


def _configure(build_dir):
    args = ['cmake', '-S', str(ROOT), '-B', str(build_dir)]
    args += _generator_args()
    args += [
        '-DCMAKE_BUILD_TYPE=Release',
        '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON',
        '-DHYPERCINE_BUILD_UPSTREAM=ON',
        '-DHYPERCINE_BUILD_TESTS=ON',
        '-DHYPERCINE_BUILD_STANDALONE=OFF',
        '-DHYPERCINE_ENABLE_CUDA=OFF',
        '-DHYPERCINE_ENABLE_METAL=OFF',
        '-DHYPERCINE_ENABLE_VULKAN=OFF',
        '-DHYPERCINE_BUILD_BENCHMARKS=OFF',
    ]
    env = dict(os.environ)
    for name in list(env):
        if name.startswith('HYPERCINE_'):
            del env[name]
    completed = subprocess.run(args, capture_output=True, text=True,
                               encoding='utf-8', errors='replace', env=env)
    return completed.returncode, completed.stdout + completed.stderr


def _sanitize(text):
    return text.replace(str(ROOT), '<root>')


def _entry(commands, suffix):
    for entry in commands:
        if entry['file'].replace('\\', '/').endswith(suffix):
            return entry
    raise AssertionError('no compile command recorded for ' + suffix)


def _include_dirs(entry):
    """Return every include directory in a recorded command, normalized."""
    directories = []
    tokens = shlex.split(entry['command'])
    index = 0
    while index < len(tokens):
        token = tokens[index]
        if token in ('-I', '/I'):
            index += 1
            if index < len(tokens):
                directories.append(tokens[index].replace('\\', '/'))
        elif token.startswith('-I') and len(token) > 2:
            directories.append(token[2:].replace('\\', '/'))
        elif token.startswith('/I') and len(token) > 2:
            directories.append(token[2:].replace('\\', '/'))
        index += 1
    return directories


def _has_upstream_include(directories):
    return [d for d in directories
            if UPSTREAM_CODEC in d or d.replace('\\', '/').rstrip('/').endswith('upstream')]


def _compile(entry, work_dir, extra_includes=(), output='probe.o'):
    tokens = shlex.split(entry['command'])
    arguments = []
    index = 0
    while index < len(tokens):
        token = tokens[index]
        if token == '-o':
            index += 2          # drop the original object path as well
            continue
        if token == '-c':
            arguments.append(token)
            index += 1
            continue
        arguments.append(token)
        index += 1
    for include in extra_includes:
        arguments.append('-I' + str(include))
    target = Path(work_dir) / output
    arguments += ['-o', str(target)]
    completed = subprocess.run(arguments, capture_output=True, text=True,
                               encoding='utf-8', errors='replace', cwd=str(work_dir))
    return completed.returncode, completed.stdout + completed.stderr


class CpuIsaIncludeBoundaryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = Path(tempfile.mkdtemp(prefix='hypercine-93-'))
        cls.build_dir = cls.tmp / 'sdk-build'
        code, log = _configure(cls.build_dir)
        if code != 0:
            raise unittest.SkipTest(
                'SDK-enabled configure unavailable here (exit {}):\n{}'.format(
                    code, _sanitize(log[-2000:])))
        commands_path = cls.build_dir / 'compile_commands.json'
        if not commands_path.is_file():
            raise unittest.SkipTest('compiler commands were not exported')
        cls.commands = json.loads(commands_path.read_text(encoding='utf-8'))
        cls.work = cls.tmp / 'work'
        cls.work.mkdir()

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def test_recorded_commands_are_present(self):
        for suffix in ISOLATED_SOURCES + (SDK_CONSUMER,):
            entry = _entry(self.commands, suffix)
            self.assertTrue(Path(entry['file']).is_absolute())

    @SKIP_MSVC
    def test_cpu_isa_targets_carry_no_upstream_include(self):
        for suffix in ISOLATED_SOURCES:
            entry = _entry(self.commands, suffix)
            leaked = _has_upstream_include(_include_dirs(entry))
            self.assertEqual(
                leaked, [],
                '{} resolves the frozen SDK header directory; #93 regression:\n{}'
                .format(suffix, '\n'.join(leaked)))

    @SKIP_MSVC
    def test_sdk_consumer_keeps_upstream_include(self):
        """Guards against 'fix' that strips the include path from everything."""
        entry = _entry(self.commands, SDK_CONSUMER)
        self.assertTrue(
            _has_upstream_include(_include_dirs(entry)),
            '{} lost its upstream include directories; the #94 correction must '
            'stay scoped to the SDK-free CPU ISA targets'.format(SDK_CONSUMER))

    @SKIP_MSVC
    def test_recorded_command_compiles(self):
        entry = _entry(self.commands, 'src/cpu/cpu_isa.cpp')
        code, log = _compile(entry, self.work, output='clean.o')
        self.assertEqual(
            code, 0,
            'the recorded CPU ISA compile command does not build:\n'
            + _sanitize(log[-4000:]))

    @SKIP_MSVC
    def test_upstream_include_still_breaks_the_translation_unit(self):
        """Negative control: proves the boundary is load-bearing, not cosmetic."""
        entry = _entry(self.commands, 'src/cpu/cpu_isa.cpp')
        code, log = _compile(entry, self.work,
                             extra_includes=(ROOT / UPSTREAM_CODEC,),
                             output='shadowed.o')
        self.assertNotEqual(
            code, 0,
            'adding the frozen SDK header directory still compiles; the #93 '
            'guard would be redundant and this contract is vacuous')
        missing = [symbol for symbol in CPUID_SYMBOLS if symbol in log]
        self.assertTrue(
            missing,
            'the probe failed for an unrelated reason, not the CPUID shadowing '
            'recorded in #93:\n' + _sanitize(log[-4000:]))


if __name__ == '__main__':
    unittest.main(verbosity=2)
