# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE.
import contextlib
import io
import json
from pathlib import Path
import tempfile
import unittest
import xml.etree.ElementTree as ET

from summarize_qualification import main, summarize


def inventory(*names, oracle=False, gpu=False):
    props = []
    if oracle or gpu:
        props = [dict(name='LABELS', value=['reference-oracle' if oracle else 'gpu-required']),
                 dict(name='SKIP_RETURN_CODE', value=77)]
    return dict(kind='ctestInfo', tests=[dict(name=n, properties=props) for n in names])


def junit(body):
    return ET.fromstring('<testsuite>' + body + '</testsuite>')


class QualificationTests(unittest.TestCase):
    def test_success_is_execution_only(self):
        r = summarize(inventory('a'), junit('<testcase name="a" status="run"/>'), 0)
        self.assertEqual(r['status'], 'PASS')
        self.assertIn('no production or pixel qualification', r['scope'])

    def test_reference_skip_blocks(self):
        r = summarize(inventory('a', oracle=True), junit('<testcase name="a"><skipped/></testcase>'), 0)
        self.assertEqual(r['status'], 'BLOCKED')
        self.assertEqual(r['counts']['PASS'], 0)

    def test_gpu_skip_blocks(self):
        self.assertEqual(summarize(inventory('a', gpu=True),
                         junit('<testcase name="a"><skipped/></testcase>'), 0)['status'], 'BLOCKED')

    def test_real_ctest_return_skip_has_notrun_status(self):
        xml = junit('<testcase name="a" status="notrun"><skipped message="SKIP_RETURN_CODE=77"/></testcase>')
        self.assertEqual(summarize(inventory('a', oracle=True), xml, 0)['status'], 'BLOCKED')
        self.assertEqual(summarize(inventory('a'), xml, 0)['status'], 'INCONCLUSIVE')

    def test_unclassified_skip_is_inconclusive(self):
        self.assertEqual(summarize(inventory('a'),
                         junit('<testcase name="a"><skipped/></testcase>'), 0)['status'], 'INCONCLUSIVE')

    def test_fail_dominates_quarantine(self):
        r = summarize(inventory('a', 'b', oracle=True), junit(
            '<testcase name="a"><skipped/></testcase><testcase name="b"><failure/></testcase>'), 8)
        self.assertEqual(r['status'], 'FAIL')
        self.assertEqual(r['counts']['BLOCKED'], 1)

    def test_missing_result_registration_and_empty_suite(self):
        for inv, xml, required in [(inventory('a'), junit(''), []),
                                   (inventory(), junit(''), []),
                                   (inventory('a'), junit('<testcase name="a"/>'), ['critical'])]:
            with self.subTest(required=required, inv=inv):
                self.assertEqual(summarize(inv, xml, 0, required)['status'], 'INCONCLUSIVE')

    def test_stale_or_duplicate_results(self):
        self.assertEqual(summarize(inventory('a'), junit('<testcase name="old"/>'), 0)['status'], 'INCONCLUSIVE')
        with self.assertRaises(ValueError):
            summarize(inventory('a'), junit('<testcase name="a"/><testcase name="a"/>'), 0)
        with self.assertRaises(ValueError):
            summarize(inventory('a', 'a'), junit(''), 0)

    def test_exit_code_cannot_hide_failure(self):
        for code, xml in [(8, '<testcase name="a"/>'), (0, '<testcase name="a"><failure/></testcase>'),
                          (0, '<testcase name="a"><error/></testcase>')]:
            self.assertEqual(summarize(inventory('a'), junit(xml), code)['status'], 'FAIL')

    def test_disabled_unknown_status_and_unbound_failure(self):
        for status in ['disabled', 'notrun', 'mystery']:
            r = summarize(inventory('a', oracle=True),
                          junit('<testcase name="a" status="' + status + '"/>'), 0)
            self.assertEqual(r['status'], 'INCONCLUSIVE')
        self.assertEqual(summarize(inventory('a'),
                         junit('<testcase name="other"><failure/></testcase>'), 8)['status'], 'FAIL')

    def test_cli_exit_codes_and_malformed_input(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            inv, xml, out = (root / n for n in ('inventory.json', 'results.xml', 'report.json'))
            args = ['--inventory', str(inv), '--junit', str(xml), '--ctest-exit', '0', '--out', str(out)]
            for body, code in [('<testcase name="a"/>', 0),
                               ('<testcase name="a"><skipped/></testcase>', 77), ('', 2),
                               ('<testcase name="a"><failure/></testcase>', 1)]:
                inv.write_text(json.dumps(inventory('a', oracle=True)))
                xml.write_text('<testsuite>' + body + '</testsuite>')
                self.assertEqual(main(args), code)
                self.assertTrue(out.is_file())
            xml.write_text('<broken')
            with contextlib.redirect_stderr(io.StringIO()) as error:
                self.assertEqual(main(args), 1)
            self.assertNotIn(str(root), error.getvalue())


if __name__ == '__main__':
    unittest.main()
