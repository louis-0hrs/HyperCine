# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
import unittest
from unittest.mock import patch
import tempfile
from pathlib import Path
import measure
import bgra_ab
import trace_bands
import priority_probe
import analyze_dependency
import dependency_ab
import subprocess
import sys
import time


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
    def test_bounded_child_timeout_is_an_error(self):
        began=time.monotonic()
        with self.assertRaises(subprocess.TimeoutExpired):
            dependency_ab.bounded([sys.executable,'-c','import time; time.sleep(30)'],timeout=0.05)
        self.assertLess(time.monotonic()-began,5)

    def test_intervention_cannot_accept_corrupt_or_premature_stable_output(self):
        summary=dict(dependencies=45,inverse_events=90,missing_lowpass_at_any_inverse_entry=0,
            missing_producer_publication=0,consumer_entry_before_producer_return=0,
            consumer_return_before_producer_entry=0,mismatched_consumer_lowpass=0,inputs_changed_during_call=0)
        calls=[dict(decoder_status=0,output_valid=True,source_pixels_exact=False,psnr_db=43.8) for _ in range(15)]
        case=dict(calls=calls,summary=dict(distinct_rgb_hashes=1),dependency_summary=summary)
        self.assertTrue(dependency_ab.intervention_observed(case))
        for key in ('missing_lowpass_at_any_inverse_entry','missing_producer_publication',
                    'consumer_entry_before_producer_return','mismatched_consumer_lowpass','inputs_changed_during_call'):
            summary[key]=1
            self.assertFalse(dependency_ab.intervention_observed(case))
            summary[key]=0
        for delta in (dict(psnr_db=35.0),dict(psnr_db=None),dict(decoder_status=7),dict(output_valid=False)):
            changed=dict(case,calls=[dict(calls[0],**delta)]+calls[1:])
            self.assertFalse(dependency_ab.intervention_observed(changed))
        self.assertFalse(dependency_ab.intervention_observed(dict(case,calls=calls[:-1])))
        with self.assertRaises(ValueError):
            dependency_ab.timing([dict(decode_ns=0)])

    def test_equality_completion_macro_does_not_imply_lowpass_ready(self):
        producer=dict(channel=0,input_wavelet=2,begin=4,end=6,output_width=2,output_height=2,
            output_after=dict(sha256='a'*64))
        consumer=dict(channel=0,input_wavelet=1,begin=1,end=3,width=2,height=2,valid_flags_before=14,
            started_flags_before=14,all_valid_before=True,input_before=dict(band_sha256=['b'*64]*4),
            input_after=dict(band_sha256=['b'*64]*4))
        call=dict(process=1,ordinal=1,psnr_db=8,temporal_events=[producer,consumer])
        result=analyze_dependency.dependency(call,0)
        self.assertTrue(result['all_valid_macro'])
        self.assertFalse(result['lowpass_valid'])
        self.assertFalse(result['required_four_bands_valid'])
        self.assertTrue(result['consumer_return_before_producer_entry'])
        self.assertFalse(result['before_matches_producer'])
        consumer.update(begin=7,end=8,valid_flags_before=15,started_flags_before=15)
        result=analyze_dependency.dependency(call,0)
        self.assertTrue(result['required_four_bands_valid'])
        self.assertFalse(result['consumer_return_before_producer_entry'])
        self.assertFalse(result['consumer_entry_before_producer_return'])

    def test_temporal_snapshot_checks_macro_flags_and_real_before_after(self):
        event=dict(channel=0,input_wavelet=1,width=1,height=1,output_width=2,output_height=2,
            begin=1,end=2,valid_flags_before=15,started_flags_before=15,all_valid_before=True,
            all_started_before=True,scale=0,scratch_free_size=16,inverse_prescale=True,
            input_before=dict(offset=0,size=8),input_after=dict(offset=8,size=8),output_after=dict(offset=16,size=8))
        raw=bytes(range(24))
        document=dict(events=[event],observer_calls_in_flight_after_decode=0)
        observed=priority_probe.temporal_hashes(raw,document)[0]
        self.assertTrue(observed['input_changed_during_call'])
        self.assertEqual(len(observed['input_before']['band_sha256']),4)
        self.assertEqual(observed['output_after']['sha256'],measure.sha(raw[16:]))
        for changes in (dict(all_valid_before=False),dict(end=1),dict(valid_flags_before=14),
                        dict(input_after=dict(offset=7,size=8)),dict(width=True)):
            with self.assertRaises(ValueError):
                priority_probe.temporal_hashes(raw,dict(document,events=[dict(event,**changes)]))
        with self.assertRaises(ValueError):
            priority_probe.temporal_hashes(raw,dict(document,observer_calls_in_flight_after_decode=1))

    def test_retained_band_hashes_reject_invalid_or_overlapping_storage(self):
        raw=bytes([1,0,2,0,3,0,4,0])
        band=dict(channel=0,wavelet_index=2,band=1,width=2,height=2,offset=0,size=8)
        self.assertEqual(trace_bands.band_hashes(raw,[band])[0]['sha256'],measure.sha(raw))
        for updates in (dict(offset=1),dict(size=10),dict(width=0),dict(band=4),dict(size=True)):
            with self.assertRaises(ValueError):
                trace_bands.band_hashes(raw,[dict(band,**updates)])
        with self.assertRaises(ValueError):
            trace_bands.band_hashes(raw+raw,[band,dict(band,offset=8)])

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
