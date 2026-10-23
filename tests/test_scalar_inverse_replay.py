#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE.
"""Replay guard tests with explicitly SYNTHETIC storage; no SDK equality claim."""
import argparse
import copy
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

PROBE=None
import validate_decode_spatial_scalar as replay


class Contracts(unittest.TestCase):
    def test_missing_originals_reject_before_probe(self):
        with tempfile.TemporaryDirectory() as d:
            with self.assertRaises(OSError):replay.replay(Path(d),Path('nonexistent-probe'))

    def test_missing_binding_cli_fails_redacted(self):
        import os
        env=dict(os.environ);env.pop('HYPERCINE_GOLDEN_DIR',None)
        result=subprocess.run([sys.executable,str(Path(replay.__file__)),'--probe','absent'],
                              env=env,capture_output=True,text=True)
        self.assertEqual(result.returncode,1)
        self.assertEqual(result.stdout,'')
        self.assertEqual(result.stderr,'Original six-pair replay BLOCKED or MISMATCH; paths/details redacted.\n')

    def test_duplicate_case_rejected(self):
        pins=replay.read_json(replay.PINS);pins['cases'][1]=copy.deepcopy(pins['cases'][0])
        with self.assertRaises(ValueError):replay.originals(Path('absent'),pins)

    def test_ambiguous_json_rejected(self):
        with tempfile.TemporaryDirectory() as d:
            p=Path(d)/'sidecar.json';p.write_text('{"stage":1,"stage":2}')
            with self.assertRaises(ValueError):replay.read_json(p)

    def synthetic_originals(self, root):
        pins=replay.read_json(replay.PINS)
        for case in pins['cases']:
            w,h=case['packet_geometry'];name=f'{w}x{h}'
            for n,item in enumerate(case['inputs']+[case['expected']]):
                base=root/(replay.PHASE_B/name/'original' if n<4 else replay.PHASE_A/name)
                path=base/item['id'];path.parent.mkdir(parents=True,exist_ok=True)
                sample=b'\xa0\x0f' if n==0 else (b'\xe8\x03' if n==4 else b'\x00\x00')
                path.write_bytes(sample*(item['byte_length']//2))
                item['sha256']=replay.digest(path)['sha256']
                side={k:item[k] for k in ('sha256','byte_length','stage','producer','representation')}
                side['status']='AVAILABLE'
                if n<4:side['input_packet']=case['packet']
                else:side['input_sha256']=[case['packet']['sha256']]
                path.with_name(path.name+'.json').write_text(json.dumps(side))
                path.with_name(path.name+'.sha256').write_text(item['sha256']+'  '+path.name+'\n')
        return pins

    def test_synthetic_storage_binding_and_corruption_rejections(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d);pins=self.synthetic_originals(root)
            # Synthetic DC values authenticate only the harness, NEVER an SDK run.
            cases=replay.originals(root,pins)
            self.assertEqual(len(cases),6)
            item=cases[0][1][0][0];original=item.read_bytes()
            item.write_bytes(original[:-1])
            with self.assertRaises(ValueError):replay.originals(root,pins)
            item.write_bytes(original)
            side=item.with_name(item.name+'.json');saved=side.read_text()
            altered=json.loads(saved);altered['stage']['channel']=2
            side.write_text(json.dumps(altered))
            with self.assertRaises(ValueError):replay.originals(root,pins)
            side.write_text(saved)
            sha=item.with_name(item.name+'.sha256');sha.write_text('invalid')
            with self.assertRaises(ValueError):replay.originals(root,pins)

    def test_actual_probe_six_synthetic_dc_cases_and_corrupt_output(self):
        self.assertIsNotNone(PROBE,'Real scalar probe required; do not skip')
        with tempfile.TemporaryDirectory() as d:
            root=Path(d);pins=self.synthetic_originals(root)
            path=root/'SYNTHETIC_PINS.json';path.write_text(json.dumps(pins))
            with mock.patch.object(replay,'PINS',path):
                # These six source/expected cases are synthetic mathematical DC
                # controls, not the immutable original corpus or its identities.
                result=replay.replay(root,PROBE)
                self.assertEqual((result['cases'],result['original_artifacts']),(6,30))
                run=subprocess.run
                def corrupt(command,**kwargs):
                    answer=run(command,**kwargs)
                    out=Path(command[-1])/'stage.i16le'
                    data=out.read_bytes();out.write_bytes(bytes([data[0]^1])+data[1:])
                    return answer
                with mock.patch.object(replay.subprocess,'run',corrupt):
                    with self.assertRaises(ValueError):replay.replay(root,PROBE)

    def test_missing_required_standalone_registration_rejected(self):
        sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'ci'))
        import run_standalone as gate
        names={'scalar_inverse_arithmetic','scalar_inverse_rejection','scalar_inverse_vectors',
               'scalar_inverse_replay_contract'}|{'scalar_inverse_graph_workers_'+str(w) for w in (1,2,4,16)}
        self.assertTrue(names<=gate.REQUIRED)
        for name in names:
            with self.assertRaises(ValueError):gate.registered_tests({'tests':[{'name':n} for n in gate.REQUIRED-{name}]})


if __name__=='__main__':
    parser=argparse.ArgumentParser()
    parser.add_argument('--probe',type=Path,required=True)
    args,remaining=parser.parse_known_args()
    PROBE=args.probe.resolve()
    unittest.main(argv=[sys.argv[0]]+remaining)
