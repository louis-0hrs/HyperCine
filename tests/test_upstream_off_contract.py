#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
"""Issue #253: HYPERCINE_BUILD_UPSTREAM=OFF must configure, not fail at link time.

The guard added in #253 is only worth anything if the two directions are both
real, so this contract configures the project twice in throwaway directories and
compares them:

  * BUILD_UPSTREAM=OFF -- configure succeeds, no generated build rule mentions
    the frozen CFHDCodecStatic archive, the omission is reported at configure
    time, and the SDK-free targets are still registered.
  * BUILD_UPSTREAM=ON (default) -- the same targets ARE registered again, so a
    guard that quietly disabled everything would be caught here.

Configure-only, CPU-only, no GPU and no private corpus. Each child configure is
a fresh build directory outside the parent build tree.
"""
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

# Every target that was linked against the frozen archive without a guard.
# The first three are registered as CTest cases; benchmark_codec is a plain
# tool target, so its presence is checked in the generated build rules.
SDK_BACKED_TESTS = (
    'test_codec_roundtrip',
    'test_yuy2_stability',
    'test_avi_sample',
)
SDK_BACKED_TOOL = 'benchmark_codec'
SDK_BACKED_TARGETS = SDK_BACKED_TESTS + (SDK_BACKED_TOOL,)

# Registered by the SDK-free paths and therefore expected in every configure.
SDK_FREE_TESTS = (
    'test_reference_oracle',
    'test_transform_wavelet',
    'test_reference_release_inventory',
)

# Targets that reference the archive anywhere in a generated build tree.
ARCHIVE_RE = re.compile(r'CFHDCodecStatic')


def _generator_args():
    if shutil.which('ninja'):
        return ['-G', 'Ninja']
    return []


def _configure(build_dir, **defines):
    args = ['cmake', '-S', str(ROOT), '-B', str(build_dir)]
    args += _generator_args()
    args += ['-DCMAKE_BUILD_TYPE=Release']
    for key, value in defines.items():
        args.append('-D{}={}'.format(key, value))
    env = dict(os.environ)
    # Keep the child configure independent of the parent's cache variables.
    for name in list(env):
        if name.startswith('HYPERCINE_'):
            del env[name]
    completed = subprocess.run(args, capture_output=True, text=True, env=env)
    log = completed.stdout + completed.stderr
    return completed.returncode, log


def _tool_is_built(build_dir, name):
    """True when the build rules produce the benchmark tool binary."""
    for candidate in ('build.ninja', 'Makefile'):
        path = Path(build_dir) / candidate
        if not path.is_file():
            continue
        text = path.read_text(errors='ignore')
        if re.search(r'build\s+\S*{}:'.format(re.escape(name)), text):
            return True
        if re.search(r'/{}$'.format(re.escape(name)), text, re.MULTILINE):
            return True
    return False


def _registered_tests(build_dir):
    completed = subprocess.run(
        ['ctest', '--test-dir', str(build_dir), '-N'],
        capture_output=True, text=True)
    if completed.returncode != 0:
        raise AssertionError(
            'ctest -N failed in {}:\n{}{}'.format(
                build_dir, completed.stdout, completed.stderr))
    listed = completed.stdout + completed.stderr
    return {name for name in SDK_BACKED_TARGETS + SDK_FREE_TESTS
            if re.search(r'\b{}\b'.format(re.escape(name)), listed)}


def _archive_references(build_dir):
    hits = []
    for path in sorted(Path(build_dir).rglob('*')):
        if not path.is_file() or path.stat().st_size > 4 * 1024 * 1024:
            continue
        if path.name in ('CMakeCache.txt', 'ConfigureLog.txt'):
            continue
        try:
            text = path.read_text(errors='ignore')
        except OSError:
            continue
        for line in text.splitlines():
            if ARCHIVE_RE.search(line):
                # CMake keeps the target name in the project's own metadata;
                # only actual link rules matter here.
                if 'lCFHDCodecStatic' in line or 'CFHDCodecStatic.lib' in line:
                    hits.append('{}: {}'.format(path.relative_to(build_dir),
                                                line.strip()[:160]))
    return hits


class UpstreamOffContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = Path(tempfile.mkdtemp(prefix='hypercine-253-'))
        cls.off_dir = cls.tmp / 'upstream-off'
        cls.on_dir = cls.tmp / 'upstream-on'
        (cls.off_code, cls.off_log) = _configure(
            cls.off_dir,
            HYPERCINE_BUILD_UPSTREAM='OFF',
            HYPERCINE_BUILD_TESTS='ON',
            HYPERCINE_BUILD_STANDALONE='OFF')
        (cls.on_code, cls.on_log) = _configure(
            cls.on_dir,
            HYPERCINE_BUILD_UPSTREAM='ON',
            HYPERCINE_BUILD_TESTS='ON',
            HYPERCINE_BUILD_STANDALONE='OFF')

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def test_upstream_off_configures(self):
        self.assertEqual(self.off_code, 0,
                         'BUILD_UPSTREAM=OFF must configure:\n' + self.off_log[-4000:])

    def test_upstream_off_emits_no_archive_link_rule(self):
        self.assertEqual(self.off_code, 0, self.off_log[-2000:])
        hits = _archive_references(self.off_dir)
        self.assertEqual(
            hits, [],
            'generated build tree still links the frozen archive without the '
            'SDK built:\n' + '\n'.join(hits))

    def test_upstream_off_reports_the_omission(self):
        self.assertIn('HYPERCINE_BUILD_UPSTREAM=OFF', self.off_log)
        for target in SDK_BACKED_TARGETS:
            self.assertIn(target, self.off_log,
                          'omitted target {} is not named at configure time'.format(target))
        self.assertNotIn('HYPERCINE_BUILD_UPSTREAM=OFF',
                         self.on_log,
                         'default configure must not report an omission')

    def test_upstream_off_keeps_the_sdk_free_subset(self):
        self.assertEqual(self.off_code, 0, self.off_log[-2000:])
        registered = _registered_tests(self.off_dir)
        for name in SDK_FREE_TESTS:
            self.assertIn(name, registered,
                          '{} must stay registered with BUILD_UPSTREAM=OFF'.format(name))
        for name in SDK_BACKED_TESTS:
            self.assertNotIn(name, registered,
                             '{} cannot be registered without the SDK'.format(name))
        self.assertFalse(_tool_is_built(self.off_dir, SDK_BACKED_TOOL),
                         '{} must not have a build rule without the SDK'.format(
                             SDK_BACKED_TOOL))

    def test_upstream_on_negative_control(self):
        """The guard must not be vacuous: SDK ON registers the same targets."""
        self.assertEqual(self.on_code, 0, self.on_log[-4000:])
        registered = _registered_tests(self.on_dir)
        for name in SDK_BACKED_TESTS + SDK_FREE_TESTS:
            self.assertIn(name, registered,
                          '{} disappeared from the default configure'.format(name))
        self.assertTrue(_tool_is_built(self.on_dir, SDK_BACKED_TOOL),
                        '{} lost its build rule in the default configure'.format(
                            SDK_BACKED_TOOL))


if __name__ == '__main__':
    unittest.main(verbosity=2)
