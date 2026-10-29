#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
import copy
import hashlib
import sys
import tempfile
import unittest
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

    def test_recorded_parity(self):
        import json
        record=json.loads((Path(__file__).resolve().parents[1]/'docs/SDK_GAP_PARITY_46.json').read_text())
        for profile,result in record['profiles'].items():
            self.assertEqual(len(result['frames']),120)
            self.assertEqual([r['frame_index'] for r in result['frames']],list(range(120)))
            matched=sum(r['equal'] for r in result['frames'])
            self.assertEqual(matched,result['matched'])
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
        record=json.loads((Path(__file__).resolve().parents[1]/'docs/SDK_GAP_TRAJECTORY_46.json').read_text())
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

if __name__=='__main__':unittest.main()
