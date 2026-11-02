#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
import copy
import hashlib
import json
import sys
import tempfile
import unittest
from unittest import mock
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
import p6_sdk_gap_gate as gate

class GapGateControls(unittest.TestCase):
    def test_exact_order_and_truncation(self):
        trace=[{'frame_index':i} for i in range(120)]
        gate.trace_contract(trace,120)
        with self.assertRaisesRegex(ValueError,'count/order'):gate.trace_contract(trace[:-1],120)
        trace[1],trace[2]=trace[2],trace[1]
        with self.assertRaisesRegex(ValueError,'count/order'):gate.trace_contract(trace,120)

    def test_same_length_packet_tamper(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);p=root/'sdk.cfhd';data=bytes(range(256))*4;p.write_bytes(data)
            index={'sdk.cfhd':(len(data),hashlib.sha256(data).hexdigest())}
            gate.verified(root,'sdk.cfhd',index)
            mutated=bytearray(data);mutated[87]^=1;p.write_bytes(mutated)
            self.assertEqual(p.stat().st_size,len(data))
            with self.assertRaisesRegex(SystemExit,'hash mismatch'):gate.verified(root,'sdk.cfhd',index)

    def test_historical_expected_to_fail_before_325(self):
        """Historical 78/120 stays FAIL; this checks preservation, not qualification."""
        import json
        path=Path(__file__).resolve().parents[1]/'docs/SDK_GAP_PARITY_46.json'
        self.assertEqual(hashlib.sha256(path.read_bytes()).hexdigest(),
                         '24ae84d6221e602f96d851a7a75874bc2ecd6caa89dcb8c481651dc4c797882c')
        record=json.loads(path.read_text())
        for profile,result in record['profiles'].items():
            self.assertEqual(len(result['frames']),120)
            self.assertEqual([r['frame_index'] for r in result['frames']],list(range(120)))
            matched=sum(r['equal'] for r in result['frames'])
            self.assertEqual(matched,result['matched'])
            self.assertEqual(matched,78 if profile in ('medium','high') else 120)
            if profile in ('medium','high'):
                self.assertEqual(result['status'],'FAIL')
                self.assertEqual([r['frame_index'] for r in result['frames'] if not r['equal']],
                                 [start+j for start in (17,33,49,65,81,97,113) for j in range(6)])
            self.assertEqual(result['status'],'PASS' if matched==120 else 'FAIL')
            for row in result['frames']:
                if row['equal']:
                    self.assertEqual(row['sdk_bytes'],row['writer_bytes'])
                    self.assertIsNone(row['first_diff'])
                    self.assertEqual(row['differing_byte_count'],0)
                else:
                    self.assertIn(row['first_diff'],[9,10])
                    self.assertGreater(row['differing_byte_count'],0)

    def test_recorded_negative_trajectory(self):
        import json
        path=Path(__file__).resolve().parents[1]/'docs/SDK_GAP_TRAJECTORY_46.json'
        self.assertEqual(hashlib.sha256(path.read_bytes()).hexdigest(),
                         '1b79278947c259fa010d4c5d33d9dafcb8fc30d412c45c0229501b70a1494a10')
        record=json.loads(path.read_text())
        for name,profile in record['profiles'].items():
            rows=profile['frames']
            self.assertEqual([r['frame'] for r in rows],list(range(40)))
            self.assertFalse(profile['hold_observed'])
            self.assertEqual(profile['in_band_frames'],0)
            for i,row in enumerate(rows):
                rate=0 if i==0 else rows[i-1]['sample_bytes']*240
                self.assertEqual(row['previous_sample_bitrate_bps'],rate)
                in_band=profile['ceiling_bps']*8//10<=rate<=profile['ceiling_bps']
                self.assertEqual(row['in_80_100_percent_band'],in_band)
                self.assertFalse(in_band)
                self.assertEqual(len(row['packet_quant_tags']),36)
                self.assertEqual(len(row['packet_scale_tags']),36)

    def test_negative_ramp_rejects_pseudo_pass(self):
        rows=[dict(frame_index=i,in_hold_band=False) for i in range(40)]
        result=dict(status='PASS',frames=rows,hold_witness_pairs=[])
        out={p+'/hold-ramp':copy.deepcopy(result) for p in gate.PROFILES[:3]}
        self.assertEqual(gate.reject_negative_ramp(out),'NON_QUALIFYING_NO_HOLD')
        for mutation in ('band','witness','packet','coverage'):
            bad=copy.deepcopy(out)
            if mutation=='band':bad['low/hold-ramp']['frames'][3]['in_hold_band']=True
            if mutation=='witness':bad['medium/hold-ramp']['hold_witness_pairs']=[[3,4]]
            if mutation=='packet':bad['high/hold-ramp']['status']='FAIL'
            if mutation=='coverage':bad.pop('low/hold-ramp')
            with self.assertRaises(ValueError):gate.reject_negative_ramp(bad)

    def test_missing_and_stale_manifest(self):
        m={'schema_version':1,'profile':'low','kind':'sequence-120f','geometry':[1920,1080],
           'timebase':'SDK default 30 fps',
           'sdk':{'implementation':'original CFHD_EncodeSample','frozen_ref':gate.FROZEN},
           'frames':[{'frame_index':i} for i in range(120)]}
        gate.validate_manifest(m,'low','sequence-120f',120)
        for key,value in [('schema_version',0),('profile','high'),('geometry',[128,128]),('sdk',{})]:
            bad=copy.deepcopy(m);bad[key]=value
            with self.assertRaisesRegex(ValueError,'stale'):gate.validate_manifest(bad,'low','sequence-120f',120)
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaisesRegex(ValueError,'absent'):gate.verified(Path(tmp),'missing',{})
            for rel in ['../packet','/packet','drive:packet','folder'+chr(92)+'packet']:
                with self.assertRaisesRegex(ValueError,'unsafe'):gate.verified(Path(tmp),rel,{})

class TraceTypes(unittest.TestCase):
    def test_accept_integer_indices(self):
        gate.trace_contract([{'frame_index':i} for i in range(7)],7)
        gate.trace_contract([],0)
    def test_reject_boolean_indices(self):
        for index in (0,1):
            trace=[{'frame_index':i} for i in range(7)]
            trace[index]['frame_index']=bool(index)
            with self.subTest(index=index),self.assertRaises(ValueError): gate.trace_contract(trace,7)
    def test_reject_float_indices(self):
        for index in range(7):
            trace=[{'frame_index':i} for i in range(7)]
            trace[index]['frame_index']=float(index)
            with self.subTest(index=index),self.assertRaises(ValueError): gate.trace_contract(trace,7)
    def test_reject_null_string_missing_indices(self):
        for value in (None,'0'):
            with self.subTest(value=value),self.assertRaises(ValueError): gate.trace_contract([{'frame_index':value}],1)
        with self.assertRaises(ValueError): gate.trace_contract([{}],1)
    def test_reject_bad_outer_shapes(self):
        for value in (None,{},'',(),False,0):
            with self.subTest(value=value),self.assertRaises(ValueError): gate.trace_contract(value,0)
    def test_reject_bad_frame_shapes(self):
        for value in (None,[],False,'0',0):
            with self.subTest(value=value),self.assertRaises(ValueError): gate.trace_contract([value],1)
    def test_reject_wrong_order_and_count(self):
        for trace,count in (([],1),([{'frame_index':1}],1),([{'frame_index':0}]*2,2),([],False),([],0.0),([],-1)):
            with self.subTest(trace=trace,count=count),self.assertRaises(ValueError): gate.trace_contract(trace,count)


class ManifestTypes(unittest.TestCase):
    def manifest(self):
        return {'schema_version':1,'profile':'low','kind':'sequence-120f',
                'geometry':[1920,1080],'timebase':'SDK default 30 fps',
                'sdk':{'implementation':'original CFHD_EncodeSample','frozen_ref':gate.FROZEN},
                'frames':[{'frame_index':i} for i in range(120)]}

    def validate(self, manifest):
        gate.validate_manifest(manifest,'low','sequence-120f',120)

    def test_accept_json_integer_manifest(self):
        self.validate(json.loads(json.dumps(self.manifest())))

    def test_reject_noninteger_schema_version(self):
        for value in (True,False,1.0,'1',None):
            manifest=self.manifest();manifest['schema_version']=value
            with self.subTest(value=value),self.assertRaises(ValueError):self.validate(manifest)

    def test_reject_float_geometry(self):
        for value in ([1920.0,1080],[1920,1080.0],[1920.0,1080.0]):
            manifest=self.manifest();manifest['geometry']=value
            with self.subTest(value=value),self.assertRaises(ValueError):self.validate(manifest)

    def test_reject_bad_geometry_shapes(self):
        for value in (None,True,1920,'1920x1080',{},[],[1920],[1920,1080,1],
                      [True,1080],[1920,'1080'],(1920,1080)):
            manifest=self.manifest();manifest['geometry']=value
            with self.subTest(value=value),self.assertRaises(ValueError):self.validate(manifest)

    def test_reject_bad_outer_shapes(self):
        for value in (None,True,1,1.0,'',[],()):
            with self.subTest(value=value),self.assertRaises(ValueError):self.validate(value)

    def test_reject_missing_required_fields(self):
        for field in self.manifest():
            manifest=self.manifest();del manifest[field]
            with self.subTest(field=field),self.assertRaises(ValueError):self.validate(manifest)

    def test_measure_rejects_pinned_malformed_manifest_before_encoder(self):
        # Exercise the actual public comparison entry with hash-pinned synthetic
        # JSON, not just the helper. No original artifacts or fake parity claim.
        for field,value in (('schema_version',True),('schema_version',1.0),
                            ('geometry',[1920.0,1080])):
            with self.subTest(field=field,value=value),tempfile.TemporaryDirectory() as tmp:
                root=Path(tmp);manifest=self.manifest();manifest[field]=value
                rel='low/sequence-120f/manifest.json';path=root/rel
                path.parent.mkdir(parents=True);data=json.dumps(manifest).encode();path.write_bytes(data)
                index={rel:(len(data),hashlib.sha256(data).hexdigest())}
                with mock.patch.object(gate.subprocess,'run') as encoder:
                    with self.assertRaisesRegex(ValueError,'stale or incompatible manifest'):
                        gate.measure(root,index,root/'encoder',root/'work','low','sequence-120f',120)
                    encoder.assert_not_called()
                self.assertFalse((root/'work').exists())


if __name__=='__main__':unittest.main()
