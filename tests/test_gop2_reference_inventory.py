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


if __name__ == '__main__':
    unittest.main()
