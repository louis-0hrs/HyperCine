# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE.
import copy
import json
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

    def test_trace_json_rejects_duplicate_keys_and_ambiguous_roots(self):
        valid = json.dumps({'profile': 'FilmScan3', 'frames': [self.row]})
        self.assertEqual(fs3.load_trace(valid), [self.row])
        for raw in [valid.replace('"frames":', '"frames": [], "frames":', 1),
                    valid.replace('"frame_index":', '"frame_index": 99, "frame_index":', 1),
                    valid[:-1] + ', "ignored": NaN}',
                    valid[:-1] + ', "ignored": Infinity}',
                    valid.replace('FilmScan3', 'FilmScan1'),
                    valid.replace('"profile": "FilmScan3", ', ''),
                    '[]', 'null', '{"frames": null}', '{"other": []}',
                    '{"frames": [], "other": 1}']:
            with self.subTest(raw=raw[:80]), self.assertRaises(ValueError):
                fs3.load_trace(raw)

    def test_rejected_trace_diagnostics_do_not_echo_evidence(self):
        sentinel = 'SYNTHETIC_PRIVATE_EVIDENCE'
        for raw in ['{"' + sentinel + '":}',
                    '{"frames": [], "' + sentinel + '": 1}',
                    '{"frames": [], "frames": "' + sentinel + '"}']:
            with self.subTest(raw=raw), self.assertRaises(ValueError) as error:
                fs3.load_trace(raw)
            self.assertNotIn(sentinel, str(error.exception))

if __name__ == '__main__':
    unittest.main()
