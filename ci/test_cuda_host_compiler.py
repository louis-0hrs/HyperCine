# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
"""The CUDA host-compiler guard must refuse only the split toolchain.

Two levels are checked. The first calls the CMake function with explicit
identities, so it runs on any host that has cmake at all and needs no CUDA
toolkit, no nvcc and no GPU. The second is the real configuration: with an
actual nvcc present, a newer C++ front end than nvcc's host compiler must be
refused at configure time rather than at link time.

Every refusal assertion also asserts the message. A guard that fails closed for
an unrelated reason would otherwise look identical to a correct one, which is
exactly the failure mode a fail-closed gate is supposed to prevent.
"""
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
MODULE = ROOT / 'cmake' / 'cuda_host_compiler.cmake'


def call_guard(cxx_id, cxx_version, host_id, host_version):
    """Invoke the guard in a throwaway project and return the configure result."""
    # CMake treats a backslash inside a quoted argument as an escape, so a
    # Windows build path has to be written with forward slashes. Getting this
    # wrong used to fail as 'Invalid character escape \a' rather than as a
    # guard verdict, which is the kind of false red this contract must not
    # produce on a platform it is supposed to stay silent on.
    module = MODULE.as_posix()
    script = (
        'cmake_minimum_required(VERSION 3.16)\n'
        'project(guard_probe NONE)\n'
        f'include("{module}")\n'
        'hypercine_require_consistent_cuda_host_compiler(\n'
        f'  "{cxx_id}" "{cxx_version}" "{host_id}" "{host_version}" "g++")\n'
        'message(STATUS "guard accepted")\n')
    with tempfile.TemporaryDirectory() as tmp:
        (Path(tmp) / 'CMakeLists.txt').write_text(script)
        build = Path(tmp) / 'build'
        return subprocess.run(
            ['cmake', '-S', tmp, '-B', str(build)],
            capture_output=True, text=True)


class GuardFunction(unittest.TestCase):

    def test_newer_cpp_front_end_than_nvcc_host_is_refused(self):
        result = call_guard('GNU', '14.2.0', 'GNU', '13.3.0')
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertIn('CUDA host compiler mismatch', result.stderr)
        # The remedy has to name the compiler the guard measured, or the message
        # sends the reader to a version that is not installed.
        self.assertIn('CMAKE_CXX_COMPILER=g++-13', result.stderr)

    def test_matching_versions_are_accepted(self):
        result = call_guard('GNU', '13.3.0', 'GNU', '13.3.0')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('guard accepted', result.stdout)

    def test_older_cpp_front_end_is_out_of_scope_and_accepted(self):
        # Documented scope: only the measured direction is judged. nvcc rejects
        # an unsupported host compiler itself during language enablement.
        result = call_guard('GNU', '12.2.0', 'GNU', '13.3.0')
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_other_front_ends_are_not_judged(self):
        # A Clang C++ front end next to an nvcc that uses GNU is a different link
        # graph; this guard has no measurement for it and must not guess.
        result = call_guard('Clang', '18.1.8', 'GNU', '13.3.0')
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_missing_version_is_not_a_refusal(self):
        for cxx, host in [('', '13.3.0'), ('14.2.0', '')]:
            with self.subTest(cxx=cxx, host=host):
                result = call_guard('GNU', cxx, 'GNU', host)
                self.assertEqual(result.returncode, 0, result.stderr)

    def test_same_major_different_minor_is_accepted(self):
        # The measured defect is a libstdc++ release difference, which tracks the
        # major version; a patch difference does not change the ABI.
        result = call_guard('GNU', '13.4.0', 'GNU', '13.2.0')
        self.assertEqual(result.returncode, 0, result.stderr)


def _tool(name):
    return shutil.which(name)


@unittest.skipUnless(_tool('nvcc') and _tool('cmake'),
                     'needs nvcc and cmake for the real configuration')
class RealConfiguration(unittest.TestCase):
    """The guard must fire from the project, not only in isolation.

    This half is environment-dependent: it needs a real nvcc and a second C++
    front end newer than nvcc's host compiler. Where that combination does not
    exist the class skips, and because CTest reports a Python unittest skip as
    a pass, it says so on stderr instead of disappearing quietly. The
    environment-independent half above is what the strict gate actually relies
    on; this one is the end-to-end proof on a host that can carry it.
    """

    @classmethod
    def setUpClass(cls):
        missing = [n for n in ('nvcc', 'cmake') if not _tool(n)]
        reason = ''
        if missing:
            reason = 'missing ' + ' '.join(missing)
        else:
            host = cls.nvcc_host_version()
            if not cls.newer_front_end(host):
                reason = ('no C++ front end newer than nvcc host compiler '
                          + repr(host))
        if reason:
            # CTest reports a Python unittest skip as a pass, so the reason is
            # written where a reader of the log will see it.
            print('NOT EXERCISED: real CUDA configure half skipped, ' + reason,
                  file=sys.stderr)

    def configure(self, compiler, extra=()):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        return subprocess.run(
            ['cmake', '-S', str(ROOT), '-B', temporary.name,
             '-DCMAKE_CXX_COMPILER=' + compiler,
             '-DHYPERCINE_BUILD_STANDALONE=ON',
             '-DHYPERCINE_BUILD_UPSTREAM=OFF',
             '-DHYPERCINE_ENABLE_CUDA=ON',
             '-DHYPERCINE_BUILD_TESTS=OFF',
             *extra],
            capture_output=True, text=True)

    @staticmethod
    def nvcc_host_version():
        """Ask nvcc which host compiler it actually uses.

        The toolkit version is not the host compiler version (a 12.4 toolkit
        may drive GCC 13), and nvcc reports the host compiler only after the
        CUDA language is enabled. This probe is therefore a real configure.
        """
        script = (
            'cmake_minimum_required(VERSION 3.16)\n'
            'project(host_probe LANGUAGES CXX CUDA)\n'
            'message(STATUS "HOST=${CMAKE_CUDA_HOST_COMPILER_ID} '
            '${CMAKE_CUDA_HOST_COMPILER_VERSION}")\n')
        with tempfile.TemporaryDirectory() as tmp:
            (Path(tmp) / 'CMakeLists.txt').write_text(script)
            result = subprocess.run(
                ['cmake', '-S', tmp, '-B', str(Path(tmp) / 'build')],
                capture_output=True, text=True)
        for line in result.stdout.splitlines():
            if 'HOST=' in line:
                return line.split('HOST=')[1].strip()
        return ''

    @staticmethod
    def newer_front_end(host):
        """Front ends strictly newer than the host compiler nvcc reports."""
        if not host.startswith('GNU') or len(host.split()) < 2:
            return []
        host_major = int(host.split()[1].split('.')[0])
        return [c for c in ('g++-16', 'g++-15', 'g++-14')
                if _tool(c) and int(c.rsplit('-', 1)[1]) > host_major]

    def test_cuda_project_refuses_split_toolchain(self):
        host = self.nvcc_host_version()
        if not host.startswith('GNU'):
            self.skipTest('nvcc host compiler is not GNU: ' + repr(host))
        newer = self.newer_front_end(host)
        if not newer:
            self.skipTest('no C++ front end newer than ' + host)
        result = self.configure(newer[0])
        if 'unsupported GNU version' in result.stderr:
            # nvcc uses this very front end as its own host compiler and rejects
            # it outright. That is the out-of-scope direction documented in the
            # guard: language enablement fails before the guard is reached.
            self.skipTest(newer[0] + ' is nvcc\'s own host compiler and is '
                          'rejected by nvcc itself')
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertIn('CUDA host compiler mismatch', result.stderr)


if __name__ == '__main__':
    unittest.main()
