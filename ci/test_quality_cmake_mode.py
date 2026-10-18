# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE.
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1]

class QualityMode(unittest.TestCase):
    def configure(self,*options):
        temporary=tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        result=subprocess.run(['cmake','-S',str(ROOT),'-B',temporary.name,
            '-DHYPERCINE_BUILD_QUALITY_STATE_ONLY=ON',*options],capture_output=True,text=True)
        return result,temporary.name

    def test_tests_off_registers_none(self):
        result,build=self.configure('-DHYPERCINE_BUILD_TESTS=OFF')
        self.assertEqual(result.returncode,0,result.stderr)
        tests=json.loads(subprocess.check_output(['ctest','--test-dir',build,'--show-only=json-v1'],text=True))
        self.assertEqual(tests['tests'],[])

    def test_incompatible_requested_modes_fail_closed(self):
        for option in ['HYPERCINE_BUILD_STANDALONE','HYPERCINE_ENABLE_CUDA',
                       'HYPERCINE_ENABLE_METAL','HYPERCINE_REQUIRE_EXTERNAL_DECODE']:
            with self.subTest(option=option):
                result,_=self.configure('-D'+option+'=ON')
                self.assertNotEqual(result.returncode,0)
                self.assertIn('Quality-state-only',result.stderr)

if __name__ == '__main__':
    unittest.main()
