#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
"""Negative integrity/path tests; synthetic bytes never qualify an SDK oracle."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location(
    'gop2_inventory', Path(__file__).resolve().parents[1] / 'tools/gop2_reference_inventory.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class InventoryTests(unittest.TestCase):
    def test_reject_external_or_parent_ids(self):
        synthetic_share = '/'.join(('', '', 'example', 'share'))
        for value in ('../secret', '/secret', 'C:/secret', synthetic_share, 'a/../b',
                      'a\\b', 'a//b', 'a/./b', '', 'a\nprivate'):
            with self.subTest(value=value), self.assertRaises(ValueError):
                module.safe_id(value)

    def test_inventory_does_not_promote_claimed_reference(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / 'manifest.json').write_text(json.dumps({
                'claim': 'FIELDPLUS', 'private_path': 'C:/private/host',
                'status': 'PASS', 'artifacts': []}), encoding='utf-8')
            result = module.inventory(root)
            self.assertEqual(result['gop2_manifest_candidates'], ['manifest.json'])
            self.assertEqual({r['status'] for r in result['required']}, {'MISSING'})
            self.assertNotIn('C:/private', json.dumps(result))
            self.assertEqual(result['release_gate'], 'BLOCKED')

    def test_read_only_inventory_does_not_extract_archives(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            archive = root / 'private.zip'
            archive.write_bytes(b'not an archive')
            result = module.inventory(root)
            self.assertEqual(result['file_count'], 1)
            self.assertEqual(list(root.iterdir()), [archive])

    def test_verify_detects_bytes_length_sidecar_and_manifest_tampering(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            original = root / 'original_sdk.cfhd'
            original.write_bytes(b'synthetic test bytes')
            item = {'id': original.name, **module.digest(original)}
            module.write_new(root / (original.name + '.json'), item)
            (root / (original.name + '.sha256')).write_text(
                item['sha256'] + '  ' + original.name + '\n', encoding='ascii')
            module.write_new(root / 'manifest.json', {
                'schema': 'hypercine.gop2.manifest/1', 'artifacts': [item],
                'reference_status': 'MISSING'})
            (root / 'manifest.json.sha256').write_text(
                module.digest(root / 'manifest.json')['sha256'] + '  manifest.json\n', encoding='ascii')
            self.assertEqual(module.verify(root)['release_gate'], 'BLOCKED')
            for invalid in (b'synthetic test byteX', b'truncated'):
                original.write_bytes(invalid)
                with self.assertRaises(ValueError):
                    module.verify(root)
            original.write_bytes(b'synthetic test bytes')
            (root / (original.name + '.json')).write_text('{}', encoding='utf-8')
            with self.assertRaises(ValueError):
                module.verify(root)

    def test_no_overwrite(self):
        with tempfile.TemporaryDirectory() as temp:
            output = Path(temp) / 'manifest.json'
            module.write_new(output, {'original': True})
            with self.assertRaises(FileExistsError):
                module.write_new(output, {'original': False})
            self.assertEqual(json.loads(output.read_text()), {'original': True})

    def test_reject_incomplete_or_duplicated_comparison_matrix(self):
        with self.assertRaises(ValueError):
            module.validate_comparison_shape({'compared': 112, 'comparisons': []})
        with self.assertRaises(ValueError):
            module.validate_comparison_shape({'compared': 112, 'comparisons': [
                {'frame_index': 0, 'id': 'BGRA'}] * 112})


class ComparisonVerificationTests(unittest.TestCase):
    """Tiny synthetic files exercise integrity, never real codec qualification."""
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name) / 'candidate'
        self.refs = Path(self.temp.name) / 'references'
        self.root.mkdir()
        self.refs.mkdir()
        (self.refs / 'original_sdk.cfhd').write_bytes(b'synthetic packet')
        manifest = {'schema': 'hypercine.gop2.manifest/1', 'artifacts': [],
                    'reference_status': 'AVAILABLE', 'geometry': [1920, 1080]}
        self.write_hashed(self.refs / 'manifest.json', manifest)
        comparisons = []
        for frame in (0, 1):
            labels = ['BGRA', 'YU64_clipped']
            for channel in range(3):
                labels += [f'ch{channel}_resident_band{b}' for b in range(17)
                           if not ((frame == 0 and 11 <= b <= 13) or
                                   (frame == 1 and 14 <= b <= 16))]
                labels += [f'ch{channel}_{stage}' for stage in
                           ('wavelet4_ll', 'temporal_low', 'temporal_high', f'frame{frame}_base_ll')]
            for label in labels:
                ref = (f'{"YU64" if label == "YU64_clipped" else label}_call0_frame{frame}.raw'
                       if label in ('BGRA', 'YU64_clipped') else f'stages/{label}.i16le')
                reference_path = module.under(self.refs, ref)
                reference_path.parent.mkdir(exist_ok=True)
                reference_path.write_bytes(ref.encode('ascii'))
                artifact = f'frame{frame}_{label}.bin'
                candidate_path = self.root / artifact
                candidate_path.write_bytes(reference_path.read_bytes())
                measured = module.digest(candidate_path)
                comparisons.append({'id': label, 'frame_index': frame, 'artifact': artifact,
                                    'reference_id': ref, 'actual': measured, 'expected': measured,
                                    'actual_bytes': measured['byte_length'], 'equal': True,
                                    'input_sha256': module.digest(self.refs / 'original_sdk.cfhd')['sha256']})
        # Include every synthetic original so the generic reference verifier
        # exercises bytes and both sidecars rather than a vacuous empty index.
        for path in sorted(p for p in self.refs.rglob('*') if p.is_file()
                           and p.suffix not in ('.json', '.sha256')):
            item = {'id': path.relative_to(self.refs).as_posix(), **module.digest(path)}
            manifest['artifacts'].append(item)
            self.write_hashed(path.with_name(path.name + '.json'), item)
            path.with_name(path.name + '.sha256').write_text(
                item['sha256'] + '  ' + path.name + '\n', encoding='ascii')
        self.write_hashed(self.refs / 'manifest.json', manifest)
        self.index = {'schema': 'hypercine.gop2.comparison_index/1', 'compared': 112,
                      'mismatched': 0, 'comparisons': comparisons,
                      'candidate_sha': '7ed4a82ed97598454c2680bd264dedf259e6e7dc',
                      'geometry': [1920, 1080], 'reference_status': 'AVAILABLE',
                      'reference_manifest': module.digest(self.refs / 'manifest.json'),
                      'input_packet': module.digest(self.refs / 'original_sdk.cfhd')}
        self.report = {'schema': 'hypercine.gop2.comparison/1', 'width': 1920, 'height': 1080,
                       'candidate_sha': self.index['candidate_sha'], 'compared': 112,
                       'mismatched': 0, 'comparisons': [dict(c) for c in comparisons]}
        self.save()

    def write_hashed(self, path, value):
        path.write_text(json.dumps(value), encoding='utf-8')
        path.with_name(path.name + '.sha256').write_text(
            module.digest(path)['sha256'] + '  ' + path.name + '\n', encoding='ascii')

    def save(self):
        self.write_hashed(self.root / 'comparison.json', self.report)
        self.index['comparison_report'] = module.digest(self.root / 'comparison.json')
        self.write_hashed(self.root / 'comparison_index.json', self.index)
        for item in self.index['comparisons']:
            self.write_hashed(self.root / (item['artifact'] + '.json'), item)
            (self.root / (item['artifact'] + '.sha256')).write_text(
                item['actual']['sha256'] + '  ' + item['artifact'] + '\n', encoding='ascii')

    def test_valid_synthetic_integrity_control(self):
        self.assertEqual(module.verify_comparison(self.root, self.refs)['mismatched'], 0)

    def test_reject_forged_equal_and_total_with_resealed_hashes(self):
        item = self.index['comparisons'][0]
        (self.root / item['artifact']).write_bytes(b'different synthetic bytes')
        item['actual'] = module.digest(self.root / item['artifact'])
        item['actual_bytes'] = item['actual']['byte_length']
        self.report['comparisons'][0].update(item)
        self.save()
        with self.assertRaises(ValueError):
            module.verify_comparison(self.root, self.refs)

    def test_honest_mismatch_remains_a_mismatch(self):
        item = self.index['comparisons'][0]
        (self.root / item['artifact']).write_bytes(b'different synthetic bytes')
        item['actual'] = module.digest(self.root / item['artifact'])
        item['actual_bytes'] = item['actual']['byte_length']
        item['equal'] = False
        self.index['mismatched'] = self.report['mismatched'] = 1
        self.report['comparisons'][0].update(item)
        self.save()
        self.assertEqual(module.verify_comparison(self.root, self.refs)['mismatched'], 1)

    def test_reject_false_mismatch_total(self):
        self.index['mismatched'] = 1
        self.save()
        with self.assertRaises(ValueError):
            module.verify_comparison(self.root, self.refs)

    def test_reject_nonboolean_equal(self):
        self.index['comparisons'][0]['equal'] = 1
        self.save()
        with self.assertRaises(ValueError):
            module.verify_comparison(self.root, self.refs)

    def test_reject_changed_available_manifest(self):
        manifest = json.loads((self.refs / 'manifest.json').read_text())
        manifest['note'] = 'new synthetic provenance'
        self.write_hashed(self.refs / 'manifest.json', manifest)
        with self.assertRaises(ValueError):
            module.verify_comparison(self.root, self.refs)

    def test_reject_wrong_stage_reference_mapping(self):
        item = self.index['comparisons'][2]
        replacement = 'stages/synthetic_other.i16le'
        (self.refs / replacement).write_bytes((self.refs / item['reference_id']).read_bytes())
        item['reference_id'] = replacement
        self.save()
        with self.assertRaises(ValueError):
            module.verify_comparison(self.root, self.refs)

    def test_reject_wrong_candidate_identity(self):
        self.index['candidate_sha'] = '0' * 40
        self.save()
        with self.assertRaises(ValueError):
            module.verify_comparison(self.root, self.refs)

    def test_reject_changed_reference_manifest(self):
        manifest = json.loads((self.refs / 'manifest.json').read_text())
        manifest['reference_status'] = 'UNSTABLE_REFERENCE'
        self.write_hashed(self.refs / 'manifest.json', manifest)
        with self.assertRaises(ValueError):
            module.verify_comparison(self.root, self.refs)

    def test_reject_stale_per_artifact_packet_identity(self):
        self.index['comparisons'][0]['input_sha256'] = '0' * 64
        self.report['comparisons'][0]['input_sha256'] = '0' * 64
        self.save()
        with self.assertRaises(ValueError):
            module.verify_comparison(self.root, self.refs)

    def test_reject_float_geometry(self):
        self.index['geometry'] = [1920.0, 1080.0]
        self.save()
        with self.assertRaises(ValueError):
            module.verify_comparison(self.root, self.refs)

    def test_reject_wrong_geometry(self):
        self.index['geometry'] = [3840, 2160]
        self.save()
        with self.assertRaises(ValueError):
            module.verify_comparison(self.root, self.refs)

    def test_reject_report_index_disagreement(self):
        self.report['comparisons'][0]['equal'] = False
        self.report['mismatched'] = 1
        self.save()
        with self.assertRaises(ValueError):
            module.verify_comparison(self.root, self.refs)

    def test_reject_boolean_frame_identity(self):
        self.index['comparisons'][0]['frame_index'] = False
        self.save()
        with self.assertRaises(ValueError):
            module.verify_comparison(self.root, self.refs)


if __name__ == '__main__':
    unittest.main()
