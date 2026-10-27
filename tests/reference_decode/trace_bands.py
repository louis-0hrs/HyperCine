#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
"""Read-only post-decode retained-band observation, not a temporal inverse breakpoint."""
import argparse
import json
from pathlib import Path
import subprocess
import sys
import contract
from measure import run, sha
from bgra_ab import collect


def band_hashes(raw, bands):
    if not raw or len(raw)>32*1024*1024 or not bands or len(bands)>72:
        raise ValueError('invalid bounded snapshot')
    expected = 0
    seen = set()
    result = []
    for band in bands:
        if any(type(band[k]) is not int for k in ('channel','wavelet_index','band','width','height','offset','size')):
            raise ValueError('invalid band descriptor')
        key = (band['channel'],band['wavelet_index'],band['band'])
        if key in seen or not (0<=key[0]<3 and 0<=key[1]<6 and 0<=key[2]<4):
            raise ValueError('invalid band identity')
        seen.add(key)
        if not (1<=band['width']<=1920 and 1<=band['height']<=1088) or band['offset'] != expected:
            raise ValueError('invalid band geometry')
        end=expected+band['size']
        if band['size'] != band['width']*band['height']*2 or end>len(raw):
            raise ValueError('invalid band storage')
        result.append(dict(**band,sha256=sha(raw[expected:end])))
        expected=end
    if expected != len(raw):
        raise ValueError('unaccounted snapshot bytes')
    return result


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--worker',type=Path,required=True)
    parser.add_argument('--packet',type=Path,required=True)
    parser.add_argument('--source',type=Path,required=True)
    parser.add_argument('--work',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    try:
        args.work.mkdir(parents=True,exist_ok=False)
        root=Path(__file__).resolve().parents[2]
        sources=[Path(__file__),Path(__file__).with_name('trace_snapshot.h'),Path(__file__).with_name('worker.cpp'),
            Path(__file__).with_name('bgra_ab.py'),root/'upstream/DecoderSDK/SampleDecoder.h',
            root/'upstream/Codec/decoder.h',root/'upstream/Codec/image.h',root/'upstream/Codec/wavelet.h']
        report=dict(copyright='Copyright (c) 2026 HyperCine Contributors.',license='MIT OR Apache-2.0',
            schema_version=1,source_commit=run(['git','rev-parse','HEAD']).decode().strip(),release_blocked=True,
            worker_sha256=sha(args.worker.read_bytes()),
            source_sha256={p.relative_to(root).as_posix():sha(p.read_bytes()) for p in sources},
            observation='Post-decode retained valid 16S bands; no upstream instruction or configuration changes',
            limitation='Snapshots after completion cannot locate the first chronological inverse mismatch; observer allocations affect scheduling',cases=[])
        for active in (False,True):
            case=collect(args.worker.resolve(),args.packet.resolve(),args.source.read_bytes(),'128',active,2,args.work)
            identities=set()
            for call in case['calls']:
                prefix=args.work/(('decode-bgra-on' if active else 'decode-bgra-off')+'-'+str(call['process'])+'-'+str(call['ordinal']))
                descriptor=contract.strict_document(Path(str(prefix)+'.bands.json').read_text())
                call['retained_bands']=band_hashes(Path(str(prefix)+'.bands').read_bytes(),descriptor['bands'])
                call['transform_workers']=descriptor['transform_workers']
                call['entropy_workers']=descriptor['entropy_workers']
                identities.update((b['channel'],b['wavelet_index'],b['band']) for b in call['retained_bands'])
            summary=[]
            for channel,level,band in sorted(identities,key=lambda k:(-k[1],k[2],k[0])):
                hashes=[b['sha256'] for c in case['calls'] for b in c['retained_bands']
                        if (b['channel'],b['wavelet_index'],b['band'])==(channel,level,band)]
                summary.append(dict(channel=channel,wavelet_index=level,band=band,
                    observed_calls=len(hashes),distinct_sha256=len(set(hashes))))
            case['retained_band_summary']=summary
            case['first_differing_retained_band_in_spatial_order']=next((b for b in summary if b['distinct_sha256']>1),None)
            report['cases'].append(case)
        args.output.write_text(json.dumps(report,indent=2,allow_nan=False)+'\n',encoding='utf-8')
        print(json.dumps([dict(active_metadata=c['active_metadata'],**c['summary'],
            first_differing_retained_band=c['first_differing_retained_band_in_spatial_order']) for c in report['cases']]))
        return 1
    except (OSError,ValueError,KeyError,TypeError,subprocess.SubprocessError):
        print('INVALID_RETAINED_BAND_OBSERVATION: release blocked',file=sys.stderr)
        return 2


if __name__=='__main__':
    raise SystemExit(main())
