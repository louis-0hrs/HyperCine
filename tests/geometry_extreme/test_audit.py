# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
"""Boundary checks for the C0 report; no claim of codec compatibility."""
import json
import copy
from pathlib import Path
import unittest
import audit


class ArithmeticAudit(unittest.TestCase):
    def test_checked_sizes(self):
        for limit in ((1 << 32) - 1, audit.U64_MAX):
            self.assertEqual(audit.mul(limit, 1, limit), limit)
            self.assertEqual(audit.add(limit, 0, limit), limit)
            for fn, args in ((audit.mul, (limit, 2, limit)),
                             (audit.add, (limit, 1, limit)),
                             (audit.mul, (-1, 0, limit))):
                with self.assertRaises(OverflowError):
                    fn(*args)
        with self.assertRaises(OverflowError):
            audit.align(audit.U64_MAX, 16)

    def test_chunk_boundary(self):
        # Negative optional tag -0x22ff, low 16 bits all set.
        self.assertEqual(audit.chunk_header(0x2200, 67108860), 0xdd01ffff)
        for key in (0x2000, 0x2100, 0x2200):
            for count in (0, 4, 262144, audit.CHUNK_MAX):
                header = audit.chunk_header(key, count)
                tag = (-(header >> 16)) & 0xffff
                self.assertEqual(tag & 0xff00, key)
                self.assertEqual((((tag & 255) << 16) | (header & 65535)) * 4, count)
            for count in (-4, 1, audit.CHUNK_MAX + 4):
                with self.assertRaises(ValueError):
                    audit.chunk_header(key, count)

    def test_geometry_and_padding(self):
        row = audit.geometry(17520, 8040)
        self.assertEqual(row['pixels'], 140860800)
        self.assertEqual(row['bgra8_bytes'], 563443200)
        self.assertEqual(row['l3_luma_width'], 2190)
        self.assertEqual(row['l3_chroma_width'], 1095)
        self.assertEqual(row['l3_chroma_stride_samples'], 1096)
        self.assertEqual(row['width_mod_128'], 112)
        for invalid in ((17521, 8040), (65536, 8040), (17520, 8041)):
            with self.assertRaises(ValueError):
                audit.geometry(*invalid)

    def test_size_domain_rejects_non_integer_and_negative_alignment(self):
        for value in (-1, True, 1.5, '16', None):
            for fn, args in ((audit.align, (value, 16)),
                             (audit.align, (16, value)),
                             (audit.surface, (value, 24)),
                             (audit.surface, (128, value)),
                             (audit.geometry, (value, 24)),
                             (audit.geometry, (128, value)),
                             (audit.chunk_header, (0x2200, value))):
                with self.subTest(function=fn.__name__, value=value, args=args):
                    with self.assertRaises((ValueError, OverflowError)):
                        fn(*args)
        for fn in (audit.add, audit.mul):
            for args in ((True, 1), (1, 2.0), (1, 1, False), (1, 1, -1)):
                with self.assertRaises((ValueError, OverflowError)):
                    fn(*args)
        self.assertEqual(audit.align(0, 16), 0)
        self.assertEqual(audit.align(audit.U64_MAX - 15, 16), audit.U64_MAX - 15)

    def test_recorded_report(self):
        root = Path(__file__).resolve().parents[2]
        text = (root / 'docs/validation/extreme-geometry-c0.json').read_text()
        audit.verify_report(text)
        recorded = json.loads(text)
        for row in recorded['geometries']:
            self.assertEqual(row['sdk'], 'SDK_ORACLE_PENDING')
            self.assertEqual(row['ffmpeg'], 'REFERENCE_PENDING')

    def test_recorded_report_types_and_keys_are_binding(self):
        report = audit.report()
        audit.verify_report(json.dumps(report))
        for field in ('width', 'pixels', 'bgra8_bytes', 'l3_chroma_stride_samples',
                      'sample_content_limit_bytes'):
            changed = copy.deepcopy(report)
            changed['geometries'][0][field] = float(changed['geometries'][0][field])
            with self.subTest(field=field), self.assertRaises(ValueError):
                audit.verify_report(json.dumps(changed))
        changed = copy.deepcopy(report)
        changed['geometries'][0]['width_mod_128'] = False
        with self.assertRaises(ValueError):
            audit.verify_report(json.dumps(changed))
        raw = json.dumps(report)
        for changed in [raw.replace('"pixels":', '"pixels": 0, "pixels":', 1),
                        raw[:-1] + ', "ignored": NaN}', '[]', 'null']:
            with self.subTest(raw=changed[:80]), self.assertRaises(ValueError):
                audit.verify_report(changed)

    def test_recorded_report_preserves_order_and_pending_status(self):
        report = audit.report()
        report['geometries'].reverse()
        with self.assertRaises(ValueError):
            audit.verify_report(json.dumps(report))
        report = audit.report()
        report['geometries'][0]['sdk'] = 'PASS'
        with self.assertRaises(ValueError):
            audit.verify_report(json.dumps(report))


if __name__ == '__main__':
    unittest.main()
