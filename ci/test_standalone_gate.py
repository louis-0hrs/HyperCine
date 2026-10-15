# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
"""Regression checks for false-green CI results, independent of a codec build."""
from pathlib import Path
import tempfile
import unittest
from run_standalone import REQUIRED, registered_tests, completed_tests


class StandaloneGateTests(unittest.TestCase):
    def test_unit_only_inventory_fails(self):
        with self.assertRaises(ValueError):
            registered_tests({'tests': [{'name': 'standalone_color'}]})

    def test_missing_external_decode_fails(self):
        with self.assertRaises(ValueError):
            registered_tests({'tests': [{'name': n} for n in REQUIRED if n != 'standalone_external_decode']})

    def test_complete_inventory(self):
        self.assertEqual(registered_tests({'tests': [{'name': n} for n in REQUIRED]}), REQUIRED)

    def check_xml(self, xml, names, expect_error):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'result.xml'
            path.write_text(xml)
            if expect_error:
                with self.assertRaises(ValueError):
                    completed_tests(path, names)
            else:
                self.assertEqual(completed_tests(path, names), len(names))

    def test_skipped_disabled_failed_or_missing_results_fail(self):
        for body in ('<skipped/>', '<failure/>', '<error/>'):
            self.check_xml('<testsuite><testcase name="decode" status="run">'+body+'</testcase></testsuite>', {'decode'}, True)
        self.check_xml('<testsuite><testcase name="decode" status="notrun"/></testsuite>', {'decode'}, True)
        self.check_xml('<testsuite/>', {'decode'}, True)
        self.check_xml('<testsuite><testcase name="decode" status="run"/><testcase name="decode" status="run"/></testsuite>', {'decode'}, True)

    def test_completed_pass(self):
        self.check_xml('<testsuite><testcase name="decode" status="run"/></testsuite>', {'decode'}, False)


if __name__ == '__main__':
    unittest.main()
