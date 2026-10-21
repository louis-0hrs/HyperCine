#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
"""Source/binding guard tests; synthetic storage is never original SSE2 parity."""
import copy
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
import replay_original_descale as review


class Contracts(unittest.TestCase):
    def test_pinned_metadata_and_original_function_selection(self):
        self.assertEqual(len(review.audit()['cases']),6)
        source=review.source()
        self.assertIn('Copyright 2017 GoPro',source)
        self.assertEqual(source.count('void InvertHorizontalStripDescale16s('),1)
        self.assertIn('_mm_adds_epi16(out_epi16, out_epi16)',source)
        self.assertIn('_mm_packs_epi32',source)
        self.assertNotIn('cpu_dispatch(',source)

    def test_missing_binding_fails_without_disclosing_paths(self):
        env=dict(os.environ);env.pop('HYPERCINE_GOLDEN_DIR',None)
        result=subprocess.run([sys.executable,str(Path(review.__file__)),'--authenticate'],
                              env=env,capture_output=True,text=True)
        self.assertEqual(result.returncode,1)
        self.assertEqual(result.stdout,'')
        self.assertEqual(result.stderr,'Original Descale review BLOCKED or INVALID; paths/details redacted.\n')

    def test_preparation_refuses_existing_directory(self):
        with tempfile.TemporaryDirectory() as d:
            result=subprocess.run([sys.executable,str(Path(review.__file__)),'--prepare-dir',d],
                                  capture_output=True,text=True)
            self.assertEqual(result.returncode,1)
            self.assertEqual(list(Path(d).iterdir()),[])

    def test_duplicate_json_is_rejected(self):
        with self.assertRaises(ValueError):
            json.loads('{"stage":1,"stage":2}',object_pairs_hook=review.unique)

    def test_synthetic_storage_authentication_rejects_corruption(self):
        pins=copy.deepcopy(review.audit())
        with tempfile.TemporaryDirectory() as d:
            root=Path(d)
            for case in pins['cases']:
                geometry=case['id'].split('_')[0]
                for n,item in enumerate(case['inputs']+[case['expected']]):
                    base=(review.PHASE_B/geometry/'original' if n in (1,2,3) else review.PHASE_A/geometry)
                    path=root/base/item['id'];path.parent.mkdir(parents=True,exist_ok=True)
                    data=b'\0'*item['byte_length'];item.update(review.identity(data));path.write_bytes(data)
                    side={k:item[k] for k in ('sha256','byte_length','stage','producer','representation')}
                    side.update(status='AVAILABLE',input_packet=case['packet'],input_sha256=[case['packet']['sha256']])
                    path.with_name(path.name+'.json').write_text(json.dumps(side))
                    path.with_name(path.name+'.sha256').write_text(item['sha256']+'  '+path.name+'\n')
            self.assertEqual(len(review.authenticate(root,pins)),6)
            item=pins['cases'][0]['expected'];path=root/review.PHASE_A/'1920x1080'/item['id']
            original=path.read_bytes();path.write_bytes(original[:-1])
            with self.assertRaises(ValueError):review.authenticate(root,pins)
            path.write_bytes(original)
            side=path.with_name(path.name+'.json');metadata=json.loads(side.read_text())
            metadata['stage']['channel']=2;side.write_text(json.dumps(metadata))
            with self.assertRaises(ValueError):review.authenticate(root,pins)

    def test_replay_reports_signed_first_difference_and_bad_extent(self):
        case={'id':'SYNTHETIC','inputs':[{'stage':{'width':2,'height':1}}]}
        files=[b'\0'*4]*4+[b'\0'*16]
        fake=subprocess.CompletedProcess([],0,stdout=b'\xff\xff'+b'\0'*14,stderr=b'')
        with patch.object(review.subprocess,'run',return_value=fake):
            result=review.replay([(case,files)],Path('synthetic-probe'))[0]
            self.assertEqual(result['mismatched_signed_samples'],1)
            self.assertEqual(result['first_difference'],{'x':0,'y':0,'actual':-1,'expected':0})
            fake.stdout=b''
            with self.assertRaises(ValueError):review.replay([(case,files)],Path('synthetic-probe'))


if __name__=='__main__': unittest.main()
