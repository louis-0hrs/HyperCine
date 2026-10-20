#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE.
"""Read-only six-pair ORIGINAL replay. Missing bytes are failure, never skip/pass.

Uses only the selected loose original files. Does not copy/export/reseal originals.
Temporary scalar outputs are independently compared to actual original LL bytes.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT=Path(__file__).resolve().parents[1]
PINS=ROOT/'docs/validation/SCALAR_INVERSE_STAGE_PROPOSAL_144.json'
PHASE_A=Path('golden/windows/gop2/issue189-20261019-r1')
PHASE_B=Path('golden/windows/gop2/issue189-20261019-phaseb-r1')


def unique(pairs):
    result={}
    for k,v in pairs:
        if k in result: raise ValueError('Ambiguous original sidecar')
        result[k]=v
    return result


def read_json(path):
    return json.loads(path.read_text(encoding='utf-8'),object_pairs_hook=unique)


def digest(path):
    h=hashlib.sha256();size=0
    with path.open('rb') as stream:
        for chunk in iter(lambda:stream.read(1024*1024),b''):
            h.update(chunk);size+=len(chunk)
    return {'sha256':h.hexdigest(),'byte_length':size}


def originals(root,pins):
    if (pins.get('schema')!='hypercine.scalar_stage_proposal/1' or len(pins['cases'])!=6 or
        {(tuple(c['packet_geometry']),c['sdk_channel']) for c in pins['cases']} !=
        {(g,c) for g in [(1920,1080),(3840,2160)] for c in range(3)}):
        raise ValueError('Six pinned case identities required')
    result=[]
    for case in pins['cases']:
        w,h=case['packet_geometry'];name=f'{w}x{h}'
        files=[]
        if len(case['inputs'])!=4: raise ValueError('Four original input bands required')
        for ordinal,item in enumerate(case['inputs']+[case['expected']]):
            identifier=Path(item['id'])
            if identifier.is_absolute() or '..' in identifier.parts:
                raise ValueError('Unsafe original identifier')
            base=root/(PHASE_B/name/'original' if ordinal<4 else PHASE_A/name)
            path=base/identifier
            measured=digest(path)
            if measured!={k:item[k] for k in ('sha256','byte_length')}:
                raise ValueError('Original stage bytes differ')
            stage=item['stage']
            if item['byte_length']!=stage['pitch']*stage['height'] or stage['pitch']!=2*stage['width']:
                raise ValueError('Original compact row geometry invalid')
            sidecar=read_json(path.with_name(path.name+'.json'))
            for key in ('sha256','byte_length','stage','producer','representation'):
                if json.dumps(sidecar.get(key),sort_keys=True)!=json.dumps(item[key],sort_keys=True):
                    raise ValueError('Original sidecar binding differs')
            if sidecar.get('status')!='AVAILABLE':raise ValueError('Original unavailable')
            if ordinal<4:
                if (stage['wavelet'],stage['band'],stage['channel'])!=(5,ordinal,case['sdk_channel']):
                    raise ValueError('Original input stage identity differs')
                if sidecar.get('input_packet')!=case['packet']:raise ValueError('Original input packet differs')
            else:
                if case['packet']['sha256'] not in sidecar.get('input_sha256',[]):
                    raise ValueError('Original output packet differs')
            sha=path.with_name(path.name+'.sha256').read_text(encoding='ascii')
            if sha!=measured['sha256']+'  '+path.name+'\n':raise ValueError('Original SHA sidecar differs')
            files.append((path,measured))
        result.append((case,files))
    return result


def replay(root,probe):
    pins=read_json(PINS)
    # Authenticate ALL30 artifacts before invoking the candidate even once.
    cases=originals(root,pins)
    with tempfile.TemporaryDirectory(prefix='hypercine-scalar-replay-') as tmp:
        for n,(case,files) in enumerate(cases):
            stage=case['inputs'][0]['stage'];out=Path(tmp)/str(n)
            subprocess.run([str(probe),'--replay',str(stage['width']),str(stage['height']),
                *[str(p) for p,_ in files[:4]],str(case['sdk_channel']),str(out)],
                check=True,stdout=subprocess.PIPE,stderr=subprocess.PIPE,timeout=30)
            actual=out/'stage.i16le'
            if digest(actual)!=files[4][1]:raise ValueError('Original scalar stage equality failed')
            # Compare actual byte streams, not an expected file manufactured by
            # the candidate, and recheck originals after execution.
            with actual.open('rb') as a,files[4][0].open('rb') as e:
                while True:
                    left,right=a.read(65536),e.read(65536)
                    if left!=right:raise ValueError('Original scalar stage bytes differ')
                    if not left:break
            for path,original in files:
                if digest(path)!=original:raise ValueError('Original changed during replay')
    return {'status':'ORIGINAL_SIX_PAIR_EQUALITY_PASS','cases':6,'original_artifacts':30,
            'scope':'isolated_GOP2_derived_spatial_unit_only','windows_build_path_signoff':'SEPARATE_OWNER_GATE'}


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--probe',type=Path,required=True)
    ap.add_argument('--golden-dir',type=Path)
    args=ap.parse_args()
    bound=args.golden_dir or os.environ.get('HYPERCINE_GOLDEN_DIR')
    if not bound:raise ValueError('Original corpus binding required')
    print(json.dumps(replay(Path(bound),args.probe.resolve())))


if __name__=='__main__':
    try:main()
    except (OSError,ValueError,KeyError,TypeError,StopIteration,subprocess.SubprocessError):
        print('Original six-pair replay BLOCKED or MISMATCH; paths/details redacted.',file=sys.stderr)
        sys.exit(1)
