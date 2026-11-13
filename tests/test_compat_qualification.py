# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
"""Corpus-free controls for the compatibility qualification matrix.

The matrix is evidence, so every guard below is a way it could lie: a row that
claims identity while carrying differing pixels, a verdict that contradicts its
own measurement, a decoder that was never named, a feature without a measured
case presented as a pass, or an artifact whose bytes no longer match its
recorded digest.

Synthetic fixtures only; no retained corpus and no external decoder is needed.
"""
import copy
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import compat_qualification as cq

FRAME = bytes(range(256)) * 256
OTHER = bytes(reversed(bytes(range(256)))) * 256

FIXTURES = (
    ('standalone', 'flat.cfhd', FRAME),
    ('standalone', 'flat.input-p10', FRAME),
    ('standalone', 'flat.p10', FRAME),
    ('standalone', 'flat.bgra', FRAME),
    ('standalone', 'canonical.cfhd', OTHER),
    ('standalone', 'canonical.input-p10', FRAME),
    ('standalone', 'canonical.p10', OTHER),
    ('standalone', 'canonical.bgra', FRAME),
    ('standalone', 'gradient.cfhd', FRAME),
    ('standalone', 'gradient.input-p10', FRAME),
    ('standalone', 'gradient.p10', FRAME),
    ('standalone', 'gradient.bgra', FRAME),
    ('windows-oracle', 'flat.sdk-p10', FRAME),
    ('windows-oracle', 'flat.sdk-bgra', FRAME),
    ('windows-oracle', 'canonical.sdk-p10', OTHER),
    ('windows-oracle', 'canonical.sdk-bgra', FRAME),
    ('windows-oracle', 'gradient.sdk-p10', FRAME),
    ('windows-oracle', 'gradient.sdk-bgra', FRAME),
    ('windows-oracle', 'control.cfhd', FRAME),
    ('windows-oracle', 'control.sdk-p10', FRAME),
    ('macos-filmscan3', 'flat.cfhd', FRAME),
    ('macos-filmscan3', 'gradient.cfhd', FRAME),
)


def digest(payload):
    return cq.sha256_bytes(payload)


class SyntheticTree:
    """Build or mutate a fixture tree with honest or dishonestly edited digests."""

    def __init__(self, root):
        self.root = root

    def write(self, tamper=None, drop=()):
        groups = {}
        for group, name, payload in FIXTURES:
            if name in drop:
                continue
            groups.setdefault(group, []).append((name, payload))
        for group, entries in groups.items():
            directory = self.root / group
            directory.mkdir(parents=True, exist_ok=True)
            records = []
            for name, payload in entries:
                (directory / name).write_bytes(payload)
                size, sha = len(payload), digest(payload)
                if tamper and name == tamper[0]:
                    sha = tamper[1]
                records.append({'file': name, 'bytes': size, 'sha256': sha})
            (directory / 'manifest.json').write_text(json.dumps({'files': records}))
        return self.root


class Inventory(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def build(self, **kwargs):
        SyntheticTree(self.root).write(**kwargs)
        return cq.inventory(self.root)

    def test_inventory_binds_roles_and_payloads(self):
        artifacts, payloads = self.build()
        self.assertEqual(artifacts['standalone/flat.input-p10']['role'],
                         'encoder_input_p10')
        self.assertEqual(artifacts['windows-oracle/control.cfhd']['role'],
                         'legacy_gopro_packet')
        self.assertEqual(payloads['standalone/flat.cfhd'], FRAME)

    def test_digest_mismatch_is_refused(self):
        with self.assertRaises(ValueError) as caught:
            self.build(tamper=('control.cfhd', '0' * 64))
        self.assertEqual(str(caught.exception), 'artifact digest mismatch')

    def test_declared_length_mismatch_is_refused(self):
        SyntheticTree(self.root).write()
        manifest = self.root / 'standalone' / 'manifest.json'
        records = json.loads(manifest.read_text())
        for record in records['files']:
            if record['file'] == 'flat.cfhd':
                record['bytes'] = 65535
        manifest.write_text(json.dumps(records))
        with self.assertRaises(ValueError) as caught:
            cq.inventory(self.root)
        self.assertEqual(str(caught.exception), 'artifact length mismatch')

    def test_unknown_artifact_name_is_refused(self):
        SyntheticTree(self.root).write()
        manifest = self.root / 'standalone' / 'manifest.json'
        records = json.loads(manifest.read_text())
        records['files'].append({'file': 'unrelated.bin', 'bytes': 1,
                                 'sha256': '0' * 64})
        manifest.write_text(json.dumps(records))
        with self.assertRaises(ValueError) as caught:
            cq.inventory(self.root)
        self.assertEqual(str(caught.exception), 'unclassified reference artifact')

    def test_path_escape_is_refused(self):
        with self.assertRaises(ValueError):
            cq.bound_artifact(self.root, '../escape.p10', 3, '0' * 64)
        with self.assertRaises(ValueError):
            cq.bound_artifact(self.root, 'a/../../escape.p10', 3, '0' * 64)

    def test_missing_manifest_is_refused(self):
        (self.root / 'standalone').mkdir()
        with self.assertRaises(ValueError) as caught:
            cq.load_manifest(self.root / 'standalone' / 'manifest.json')
        self.assertEqual(str(caught.exception), 'missing fixture manifest')


class Matrix(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        SyntheticTree(self.root).write()
        self.artifacts, self.payloads = cq.inventory(self.root)
        self.matrix = cq.build_matrix(self.artifacts, self.payloads)

    def row(self, identifier):
        return next(row for row in self.matrix['rows'] if row['id'] == identifier)

    def test_build_passes_its_own_validation(self):
        cq.validate_matrix(self.matrix)
        cq.validate_matrix(self.matrix, self.root)

    def test_identical_row_may_bind_equal_digests(self):
        row = self.row('hypercine-packet-flat')
        self.assertEqual(row['status'], cq.STATUS_IDENTITY)
        for item in row['comparisons']:
            self.assertEqual(item['metrics']['mismatches'], 0)

    def test_differing_row_is_not_an_identity(self):
        row = self.row('hypercine-packet-canonical')
        self.assertEqual(row['status'], cq.STATUS_DIFFERENCE)
        self.assertTrue(any(item['metrics']['mismatches'] > 0
                            for item in row['comparisons']))

    def test_packets_without_capture_are_not_observable(self):
        row = self.row('hypercine-metal-filmscan3-packets')
        self.assertEqual(row['status'], cq.STATUS_NOT_OBSERVABLE)
        self.assertEqual(row['comparisons'], [])

    def test_identity_claim_with_measured_error_is_refused(self):
        broken = copy.deepcopy(self.matrix)
        item = broken['rows'][0]['comparisons'][0]
        item['metrics']['mismatches'] = 12
        item['metrics']['max_error'] = 3
        with self.assertRaises(ValueError) as caught:
            cq.validate_matrix(broken)
        self.assertEqual(str(caught.exception),
                         'IDENTICAL verdict contradicts the measured error')

    def test_difference_without_measured_error_is_refused(self):
        broken = copy.deepcopy(self.matrix)
        item = broken['rows'][0]['comparisons'][0]
        item['verdict'] = cq.VERDICT_DIFFERENT
        with self.assertRaises(ValueError) as caught:
            cq.validate_matrix(broken)
        self.assertEqual(str(caught.exception),
                         'DIFFERENT verdict without a measured difference')

    def test_status_contradicting_comparisons_is_refused(self):
        broken = copy.deepcopy(self.matrix)
        broken['rows'][1]['status'] = cq.STATUS_IDENTITY
        with self.assertRaises(ValueError) as caught:
            cq.validate_matrix(broken)
        self.assertEqual(str(caught.exception),
                         'row status contradicts its comparisons')

    def test_unbound_local_decoder_is_refused(self):
        broken = copy.deepcopy(self.matrix)
        broken['rows'][0]['comparisons'].append({
            'basis': cq.BASIS_LOCAL,
            'decoder': cq.recorded_decoder('ffmpeg-cfhd', 'n8.0'),
            'capture': {'artifact': 'local/x.p10', 'sha256': digest(FRAME)},
            'against': {'artifact': 'standalone/flat.p10', 'sha256': digest(FRAME)},
            'metrics': cq.metrics(0, 0, None, 32768),
            'verdict': cq.VERDICT_IDENTICAL,
        })
        with self.assertRaises(ValueError) as caught:
            cq.validate_matrix(broken)
        self.assertEqual(
            str(caught.exception),
            'local measurement without a bound decoder executable')

    def test_committed_comparison_may_not_claim_a_binary(self):
        broken = copy.deepcopy(self.matrix)
        broken['rows'][0]['comparisons'][0]['decoder']['binary_sha256'] = '0' * 64
        with self.assertRaises(ValueError) as caught:
            cq.validate_matrix(broken)
        self.assertEqual(
            str(caught.exception),
            'committed-artifact comparison must not claim a binary digest')

    def test_identity_outside_admitted_scope_is_refused(self):
        for field, value in (('scan', 'interlaced'), ('gop', 2),
                             ('pixel_format', 'rgba10le'), ('geometry', '1920x1080')):
            broken = copy.deepcopy(self.matrix)
            broken['rows'][0]['scope'][field] = value
            with self.assertRaises(ValueError) as caught:
                cq.validate_matrix(broken)
            self.assertIn('outside the admitted scope', str(caught.exception))

    def test_self_comparison_is_refused(self):
        broken = copy.deepcopy(self.matrix)
        item = broken['rows'][0]['comparisons'][0]
        item['against'] = item['capture']
        with self.assertRaises(ValueError) as caught:
            cq.validate_matrix(broken)
        self.assertEqual(str(caught.exception),
                         'comparison compares an artifact with itself')

    def test_backlog_entry_carrying_a_verdict_is_refused(self):
        broken = copy.deepcopy(self.matrix)
        broken['backlog'][0]['status'] = cq.STATUS_IDENTITY
        with self.assertRaises(ValueError) as caught:
            cq.validate_matrix(broken)
        self.assertEqual(str(caught.exception),
                         'backlog entry carries unexpected fields')

    def test_empty_backlog_is_refused(self):
        broken = copy.deepcopy(self.matrix)
        broken['backlog'] = []
        with self.assertRaises(ValueError) as caught:
            cq.validate_matrix(broken)
        self.assertEqual(str(caught.exception), 'backlog must be enumerated')

    def test_duplicate_row_id_is_refused(self):
        broken = copy.deepcopy(self.matrix)
        broken['rows'].append(copy.deepcopy(broken['rows'][0]))
        with self.assertRaises(ValueError) as caught:
            cq.validate_matrix(broken)
        self.assertEqual(str(caught.exception), 'duplicate matrix row id')

    def test_unknown_direction_is_refused(self):
        broken = copy.deepcopy(self.matrix)
        broken['rows'][0]['direction'] = 'universal-interoperability'
        with self.assertRaises(ValueError) as caught:
            cq.validate_matrix(broken)
        self.assertEqual(str(caught.exception),
                         'unknown compatibility direction')

    def test_zero_sample_comparison_is_refused(self):
        broken = copy.deepcopy(self.matrix)
        broken['rows'][0]['comparisons'][0]['metrics']['samples'] = 0
        with self.assertRaises(ValueError) as caught:
            cq.validate_matrix(broken)
        self.assertEqual(str(caught.exception), 'comparison without samples')

    def test_tampered_packet_digest_is_refused_on_disk(self):
        broken = copy.deepcopy(self.matrix)
        broken['rows'][0]['packet']['sha256'] = '1' * 64
        with self.assertRaises(ValueError) as caught:
            cq.validate_matrix(broken, self.root)
        self.assertEqual(str(caught.exception), 'artifact digest mismatch')

    def test_mutated_packet_bytes_are_refused_on_disk(self):
        (self.root / 'standalone' / 'flat.cfhd').write_bytes(OTHER)
        with self.assertRaises(ValueError) as caught:
            cq.validate_matrix(self.matrix, self.root)
        self.assertEqual(str(caught.exception), 'artifact digest mismatch')


class Metrics(unittest.TestCase):
    def test_identity_is_sample_level_not_byte_level(self):
        result = cq.diff_metrics(FRAME, FRAME, cq.SAMPLE_COUNT)
        self.assertEqual(result, {'psnr_db': None, 'max_error': 0,
                                  'mismatches': 0, 'samples': cq.SAMPLE_COUNT})

    def test_single_code_value_difference_is_one_mismatch(self):
        altered = bytearray(FRAME)
        altered[0] = (altered[0] + 1) % 256
        result = cq.diff_metrics(bytes(altered), FRAME, cq.SAMPLE_COUNT)
        self.assertEqual(result['mismatches'], 1)
        self.assertEqual(result['max_error'], 1)

    def test_length_mismatch_is_refused(self):
        with self.assertRaises(ValueError) as caught:
            cq.diff_metrics(FRAME[:-1], FRAME, cq.SAMPLE_COUNT)
        self.assertEqual(str(caught.exception), 'p10 buffer length differs')

    def test_wrong_sample_count_is_refused(self):
        with self.assertRaises(ValueError) as caught:
            cq.diff_metrics(FRAME, FRAME, 65536)
        self.assertEqual(str(caught.exception), 'unexpected p10 sample count')


if __name__ == '__main__':
    unittest.main()
