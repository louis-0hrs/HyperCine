#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
"""Bounded Phase2C CPU-reference recreation and temporal 128/256 inverse controls."""
import argparse
import json
import re
from pathlib import Path
import subprocess
import sys
import contract
from bgra_ab import collect
from measure import run, sha


def temporal_hashes(raw, document):
    if (not raw or len(raw)>32*1024*1024 or
        type(document['observer_calls_in_flight_after_decode']) is not int or
        document['observer_calls_in_flight_after_decode'] != 0):
        raise ValueError('invalid/incomplete temporal observation')
    events=document['events']
    if not isinstance(events,list) or not 1<=len(events)<=24:
        raise ValueError('invalid event count')
    expected=0
    orders=set()
    identities=set()
    result=[]
    for event in events:
        item=dict(event)
        integers=('channel','input_wavelet','width','height','output_width','output_height','begin','end',
                  'valid_flags_before','started_flags_before','scale','scratch_free_size')
        if any(type(item[k]) is not int for k in integers):
            raise ValueError('invalid temporal descriptor')
        identity=(item['channel'],item['input_wavelet'])
        if identity in identities or not (0<=identity[0]<3 and identity[1] in (1,2)):
            raise ValueError('duplicate boundary identity')
        identities.add(identity)
        if not 0<item['begin']<item['end'] or item['begin'] in orders or item['end'] in orders:
            raise ValueError('invalid event order')
        orders.update((item['begin'],item['end']))
        if not (1<=item['width']<=1920 and 1<=item['height']<=1088 and
                item['output_width']==2*item['width'] and item['output_height']==2*item['height']):
            raise ValueError('invalid boundary geometry')
        # Preserve 0x0e equality positives: the missing lowpass bit is observed
        # evidence, not malformed input to discard or turn into a skip.
        if (item['all_valid_before'] is not True or item['all_started_before'] is not True or
            item['valid_flags_before'] != item['started_flags_before'] or
            (item['started_flags_before']&14)!=14):
            raise ValueError('inconsistent macro/flags boundary')
        for phase in ('input_before','input_after','output_after'):
            segment=item[phase]
            size=item['width']*item['height']*8 if phase.startswith('input') else item['output_width']*item['output_height']*2
            if (type(segment['offset']) is not int or type(segment['size']) is not int or
                segment['offset']!=expected or segment['size']!=size or expected+size>len(raw)):
                raise ValueError('invalid temporal storage')
            data=raw[expected:expected+size]
            item[phase]=dict(sha256=sha(data),size=size)
            if phase.startswith('input'):
                band_size=size//4
                item[phase]['band_sha256']=[sha(data[b*band_size:(b+1)*band_size]) for b in range(4)]
            expected+=size
        item['input_changed_during_call']=item['input_before']['sha256']!=item['input_after']['sha256']
        result.append(item)
    if expected!=len(raw):
        raise ValueError('unaccounted temporal bytes')
    return result


def observe(worker, packet, source, case, root):
    root.mkdir(parents=True,exist_ok=False)
    evidence=collect(worker,packet,source,case,True,2,root)
    for call in evidence['calls']:
        prefix=root/('decode-bgra-on-'+str(call['process'])+'-'+str(call['ordinal']))
        meta=contract.strict_document(Path(str(prefix)+'.temporal.json').read_text())
        call['temporal_events']=temporal_hashes(Path(str(prefix)+'.temporal').read_bytes(),meta)
        if {(e['channel'],e['input_wavelet']) for e in call['temporal_events']}!={(c,l) for c in range(3) for l in (1,2)}:
            raise ValueError('missing inverse boundary')
        call['transform_workers']=meta['transform_workers'];call['entropy_workers']=meta['entropy_workers']
        call['observer_calls_in_flight_after_decode']=meta['observer_calls_in_flight_after_decode']
    summary=[]
    for level in (2,1):
        for channel in range(3):
            events=[e for c in evidence['calls'] for e in c['temporal_events'] if
                    e['channel']==channel and e['input_wavelet']==level]
            entry=dict(channel=channel,input_wavelet=level,observed_calls=len(events),
                input_mutated_calls=sum(e['input_changed_during_call'] for e in events),
                configurations=len({(e['scale'],e['inverse_prescale'],e['scratch_free_size'],e['width'],e['height']) for e in events}))
            for phase in ('input_before','input_after','output_after'):
                entry[phase+'_distinct_hashes']=len({e[phase]['sha256'] for e in events})
                if phase.startswith('input'):
                    entry[phase+'_distinct_band_hashes']=[len({e[phase]['band_sha256'][b] for e in events}) for b in range(4)]
            summary.append(entry)
    evidence['temporal_summary']=summary
    return evidence


def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('worker','temporal-worker','exporter','packet128','source128','work','output'):
        p.add_argument('--'+name,type=Path,required=True)
    args=p.parse_args()
    try:
        args.work.mkdir(parents=True,exist_ok=False)
        root=Path(__file__).resolve().parents[2]
        paths=[Path(__file__),Path(__file__).with_name('worker.cpp'),Path(__file__).with_name('phase2c_packet.cpp'),
            Path(__file__).with_name('temporal_observer.cpp'),Path(__file__).with_name('decoder_temporal.c'),
            Path(__file__).with_name('bgra_ab.py'),Path(contract.__file__),root/'src/cpu/entropy_parallel.cpp',
            root/'src/core/phase2c_encoder.cpp',root/'upstream/Codec/decoder.c',root/'upstream/Codec/wavelet.c',
            root/'upstream/Codec/image.h',root/'upstream/Codec/config.h']
        report=dict(copyright='Copyright (c) 2026 HyperCine Contributors.',license='MIT OR Apache-2.0',schema_version=1,
            source_commit=run(['git','rev-parse','HEAD']).decode().strip(),release_blocked=True,
            exact_M4_failing_sample=dict(status='UNAVAILABLE',reason='no authenticated original packet/SHA supplied locally'),
            source_sha256={x.relative_to(root).as_posix():sha(x.read_bytes()) for x in paths},
            binary_sha256={name:sha(getattr(args,name).read_bytes()) for name in ('worker','temporal_worker','exporter')})
        map_file=args.temporal_worker.resolve().parents[1]/'reference_decode_temporal.map'
        lines=map_file.read_text().splitlines()
        redirected=any(re.search(r'\sDecodeSample\s',line) and 'decoder_temporal.obj' in line for line in lines)
        original=any(re.search(r'\sTransformInverseSpatialQuantLowpass\s',line) and 'wavelet.obj' in line for line in lines)
        if not (redirected and original):
            raise ValueError('unverified observer linkage')
        report['linkage_verified']=dict(decode_sample='isolated decoder_temporal.obj',
            inverse_lowpass='original CFHDCodecStatic wavelet.obj',map_sha256=sha(map_file.read_bytes()))
        reference=args.work/'phase2c-reference.cfhd'; original=args.work/'phase2c-source.bgra'
        report['reference_export_state']=contract.strict_document(run([args.exporter.resolve(),reference,original]).decode())
        priority_dir=args.work/'priority-a';priority_dir.mkdir()
        report['priority_a']=collect(args.worker.resolve(),reference,original.read_bytes(),'1080',True,3,priority_dir)
        packet256=args.work/'control256.cfhd';source256=args.work/'control256.bgra'
        run([args.worker.resolve(),'generate-bgra','256',packet256,source256])
        control_dir=args.work/'control256';control_dir.mkdir()
        report['control256']=collect(args.worker.resolve(),packet256,source256.read_bytes(),'256',True,2,control_dir)
        report['temporal_cases']=[observe(args.temporal_worker.resolve(),packet,source.read_bytes(),case,args.work/('temporal-'+case))
            for case,packet,source in (('128',args.packet128.resolve(),args.source128),('256',packet256,source256))]
        report['instrumentation']='isolated recompile of pristine decoder.c with inverse-call redirection to before/after observer; original wavelet function and arguments unchanged; timing perturbed'
        args.output.write_text(json.dumps(report,indent=2,allow_nan=False)+'\n',encoding='utf-8')
        print(json.dumps(dict(priority_a=report['priority_a']['summary'],control256=report['control256']['summary'],
            temporal=[dict(geometry=c['geometry'],**c['summary'],boundaries=c['temporal_summary']) for c in report['temporal_cases']])) )
        return 1
    except (OSError,ValueError,KeyError,TypeError,RecursionError,subprocess.SubprocessError):
        print('INVALID_PRIORITY_PROBE: release blocked',file=sys.stderr)
        return 2


if __name__=='__main__':
    raise SystemExit(main())
