# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE.
import copy
import unittest
import validate_filmscan3 as fs3
import validate_windows_reference as gate

class TraceContract(unittest.TestCase):
    def setUp(self):
        self.frame=next(f for f in gate.load_manifest(gate.CORPUS/'manifest.json')['frames'] if f['width']==1920)
        self.row={key:copy.deepcopy(self.frame[key]) for key in
                  ['frame_index','fs_rate_limiter','previous_sample_bytes','quant','scale']}
        self.row['sample_bytes']=self.frame['sdk_byte_length']
        self.row['coefficient_fnv64']=[b['sdk_coeff_fnv64'] for b in self.frame['bands']]

    def test_valid_trace(self):
        fs3.validate_trace([self.row],[self.frame])

    def test_numeric_aliases(self):
        for key in ['frame_index','fs_rate_limiter','previous_sample_bytes','sample_bytes']:
            row=copy.deepcopy(self.row);row[key]=float(row[key])
            with self.subTest(key=key),self.assertRaises(ValueError):
                fs3.validate_trace([row],[self.frame])
        for key in ['quant','scale']:
            row=copy.deepcopy(self.row);row[key]=[float(v) for v in row[key]]
            with self.subTest(key=key),self.assertRaises(ValueError):
                fs3.validate_trace([row],[self.frame])

    def test_empty_inventory(self):
        with self.assertRaises(ValueError):
            fs3.validate_trace([],[])

    def test_duplicate_json_keys_rejected_before_trace_validation(self):
        import json
        valid = json.dumps({'profile': 'FilmScan3', 'frames': [self.row]})
        self.assertEqual(fs3.load_trace(valid), [self.row])
        for raw in (valid.replace('"frames":', '"frames": [], "frames":', 1),
                    valid.replace('"frame_index":', '"frame_index": 999, "frame_index":', 1)):
            with self.subTest(raw=raw), self.assertRaises(ValueError):
                fs3.load_trace(raw)

    def test_invalid_trace_json_roots_and_constants(self):
        for raw in ('[]', 'null', '{}', '{"frames": [], "ignored": NaN}',
                    '{"frames": [], "ignored": 1e400}'):
            with self.subTest(raw=raw), self.assertRaises(ValueError):
                fs3.load_trace(raw)

if __name__ == '__main__':
    unittest.main()
