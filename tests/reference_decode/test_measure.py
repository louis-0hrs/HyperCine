# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
import unittest
from unittest.mock import patch
import tempfile
from pathlib import Path
import measure
import bgra_ab


class MeasurementTests(unittest.TestCase):
    def test_metric_known_delta_and_size(self):
        result = measure.metrics(bytes([0, 10, 255, 99]), bytes([0, 11, 252, 99]))
        self.assertEqual(result['changed_bytes'], 2)
        self.assertEqual(result['max_abs_delta'], 3)
        self.assertAlmostEqual(result['psnr_db'], 44.15140352195873)
        self.assertIsNone(measure.metrics(b'x', b'x')['psnr_db'])
        for a, b in ((b'', b''), (b'a', b'ab')):
            with self.assertRaises(ValueError):
                measure.metrics(a, b)

    def test_incomplete_worker_fails_closed(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            def worker(command):
                if command[1] == 'generate':
                    Path(command[3]).write_bytes(b'packet')
                    Path(command[4]).write_bytes(bytes(128*128*2))
                    return b''
                return b'{}\n'
            with patch.object(measure, 'run', side_effect=worker):
                with self.assertRaises((ValueError, KeyError)):
                    measure.measure(Path('worker'), '128', 2, root)

    def test_no_alternate_or_encoder_comparison_does_not_pass(self):
        with tempfile.TemporaryDirectory() as temp:
            def worker(command):
                if command[1] == 'generate':
                    Path(command[3]).write_bytes(b'packet')
                    Path(command[4]).write_bytes(bytes(128*128*2))
                    return b''
                return b'\n'.join(measure.json.dumps(dict(ordinal=i, decoder_status=7,
                     output_valid=False)).encode() for i in range(1, 6))
            with patch.object(measure, 'run', side_effect=worker):
                report = measure.measure(Path('worker'), '128', 2, Path(temp))
            self.assertEqual(report['alternate_decoder']['status'], 'UNAVAILABLE')
            self.assertIsNone(report['encoder_packets_equal'])
            self.assertEqual(report['result']['classification'], 'REFERENCE_DECODE_ERROR')
            self.assertTrue(report['result']['release_blocked'])


class BgraTests(unittest.TestCase):
    def test_quality_matches_phase2c_orientation_and_ignores_alpha(self):
        source = bytes([10,20,30,255, 80,90,100,255])
        pixels = bytes([80,90,100,0, 10,20,30,0])
        result = bgra_ab.quality(source,pixels,1,2)
        self.assertIsNone(result['psnr_db'])
        self.assertTrue(result['source_pixels_exact'])
        self.assertEqual(result['best_orientation'],'bottom_up')
        self.assertEqual(bgra_ab.rgb(source), bytes([10,80,20,90,30,100]))
        with self.assertRaises(ValueError):
            bgra_ab.quality(source,pixels[:-1],1,2)

    def test_metadata_error_cannot_hide_behind_decode_success(self):
        for failure in ('metadata_open_status','metadata_init_status','metadata_set_status'):
            row = dict(active_metadata=True,requested_format='BGRA',processing_flags=65535,
                metadata_open_status=0,metadata_init_status=0,metadata_set_status=0,
                decoder_status=0,output_valid=True,width=128,height=128,pitch=512,buffer_size=65536)
            row[failure] = 7
            actual = bgra_ab.validate(row,True,128,128)
            self.assertFalse(actual['output_valid'])
            actual.update(process=1,ordinal=1)
            self.assertEqual(bgra_ab.contract.classify(None,[actual]),'REFERENCE_DECODE_ERROR')

    def test_unapplied_metadata_and_configuration_drift_are_not_success(self):
        row = dict(active_metadata=False,requested_format='BGRA',processing_flags=0,
            metadata_open_status=-1,metadata_init_status=-1,metadata_set_status=-1,
            decoder_status=0,output_valid=True,width=128,height=128,pitch=512,buffer_size=65536)
        self.assertTrue(bgra_ab.validate(row.copy(),False,128,128)['output_valid'])
        with self.assertRaises(ValueError):
            bgra_ab.validate(row.copy(),True,128,128)
        wrong = row.copy(); wrong['processing_flags']=65535
        with self.assertRaises(ValueError):
            bgra_ab.validate(wrong,False,128,128)
        wrong = row.copy(); wrong['metadata_init_status']=0
        self.assertFalse(bgra_ab.validate(wrong,False,128,128)['output_valid'])


if __name__ == '__main__':
    unittest.main()
