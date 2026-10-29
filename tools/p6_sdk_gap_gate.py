#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
"""Fail-closed original SDK sequence and hold-band packet gate (#46)."""
import argparse
import json
import subprocess
import tempfile
import sys
from pathlib import Path
from p6_writer_parity import load_transfer_index, require_pinned, sha_file, first_diff
from sdk_gap_corpus import PROFILES, FROZEN, packet_tables, sample


def verified(root, rel, index):
    if not isinstance(rel,str) or rel.startswith(('/',chr(92))) or '..' in Path(rel).parts or ':' in rel or chr(92) in rel:
        raise ValueError('unsafe corpus artifact path')
    path = root/rel
    if root.resolve() not in path.resolve().parents: raise ValueError('artifact escapes corpus')
    if rel not in index: raise ValueError('artifact absent from transfer index: '+rel)
    size, digest = index[rel]
    require_pinned(path,size,digest,index,root,rel)
    return path


def trace_contract(trace, count):
    if [f.get('frame_index') for f in trace] != list(range(count)):
        raise ValueError('trace count/order mismatch')


def validate_manifest(manifest, profile, kind, count):
    if (manifest.get('schema_version') != 1 or manifest.get('profile') != profile or
        manifest.get('kind') != kind or manifest.get('geometry') != [1920,1080] or
        manifest.get('timebase') != 'SDK default 30 fps' or
        manifest.get('sdk') != {'implementation':'original CFHD_EncodeSample','frozen_ref':FROZEN}):
        raise ValueError('stale or incompatible manifest')
    trace_contract(manifest['frames'], count)


def measure(root, index, encoder, work, profile, kind, count):
    manifest = json.loads(verified(root,profile+'/'+kind+'/manifest.json',index).read_text())
    validate_manifest(manifest,profile,kind,count)
    frames = manifest['frames']; inputs=[]; packets=[]
    sdk_trace=verified(root,profile+'/'+kind+'/packets/trace.tsv',index).read_text().splitlines()
    expected_trace=['%d\t%d\tframe_%d.cfhd' % (f['frame_index'],f['sdk_byte_length'],f['frame_index']) for f in frames]
    if sdk_trace!=expected_trace: raise ValueError('original SDK trace count/order/size mismatch')
    for f in frames:
        src=verified(root,f['input'],index); packet=verified(root,f['packet'],index)
        if (sha_file(src)!=f['input_sha256'] or src.stat().st_size!=f['input_byte_length'] or
            sha_file(packet)!=f['sdk_sha256'] or packet.stat().st_size!=f['sdk_byte_length']):
            raise ValueError('frame/transfer provenance mismatch')
        q,s,t,masks=packet_tables(packet.read_bytes())
        if (q,s,t,masks)!=(f['packet_quant'],f['packet_scale'],f['packet_tags'],f['metadata_mask_ranges']):
            raise ValueError('packet tables/tags differ from manifest')
        inputs.append(str(src));packets.append(packet)
    directory=work/profile/kind
    # A fresh directory is mandatory; stale emitted packets cannot fill gaps.
    if directory.exists(): raise ValueError('work output already exists')
    prefix=[sys.executable,str(encoder)] if encoder.suffix=='.py' else [str(encoder)]
    subprocess.run(prefix+['--coefficient-hash','off','--quality',profile,str(directory)]+inputs,
                   check=True,capture_output=True,text=True)
    trace=json.loads((directory/'trace.json').read_text())['frames'];trace_contract(trace,count)
    rows=[]
    for f,actual,path in zip(frames,trace,packets):
        idx=f['frame_index']; sdk=path.read_bytes(); ours=(directory/('frame_%04d.cfhd'%idx)).read_bytes()
        norm_sdk=sample.normalized_sample(sdk,sample.parse_sample(sdk),f['metadata_mask_ranges'])
        norm_ours=sample.normalized_sample(ours,sample.parse_sample(ours),f['metadata_mask_ranges'])
        q,s,t,m=packet_tables(ours)
        offsets=[] if norm_sdk==norm_ours else [i for i in range(min(len(norm_sdk),len(norm_ours))) if norm_sdk[i]!=norm_ours[i]]
        ranges=[]
        for offset in offsets:
            if ranges and ranges[-1][1]==offset: ranges[-1][1]=offset+1
            else: ranges.append([offset,offset+1])
        if len(norm_sdk)!=len(norm_ours): ranges.append([min(len(norm_sdk),len(norm_ours)),max(len(norm_sdk),len(norm_ours))])
        rows.append({'frame_index':idx,'sdk_bytes':len(sdk),'writer_bytes':len(ours),
            'equal':norm_sdk==norm_ours,'first_diff':first_diff(norm_sdk,norm_ours),
            'differing_byte_count':len(offsets)+abs(len(norm_sdk)-len(norm_ours)),
            'differing_offsets_first_32':offsets[:32],'differing_byte_ranges':ranges,
            'packet_tables_equal':q==f['packet_quant'] and s==f['packet_scale'],
            'previous_sample_bitrate':0 if idx==0 else frames[idx-1]['sdk_byte_length']*240,
            'sdk_quant':f['packet_quant'],'sdk_scale':f['packet_scale'],
            'sdk_tags':f['packet_tags']})
    # A hold witness needs prior entry, two consecutive actual in-band rates,
    # and stable changed packet tables, with scale tags unchanged. Low's base
    # and midpoint tables coincide: those tags cannot identify its hidden state
    # and therefore cannot yield a positive witness here.
    ceiling={'low':110000000,'medium':130000000,'high':150000000}.get(profile)
    witness=[];entered=False
    if ceiling:
        for i,row in enumerate(rows):
            bitrate=row['previous_sample_bitrate']
            if bitrate>ceiling: entered=True
            if bitrate<ceiling*8//10: entered=False
            row['in_hold_band']=ceiling*8//10<=bitrate<=ceiling
            row['noninitial_quant_tags']=row['sdk_quant']!=rows[0]['sdk_quant']
            if (profile!='low' and i and entered and row['in_hold_band'] and rows[i-1].get('in_hold_band') and
                row['noninitial_quant_tags'] and rows[i-1]['noninitial_quant_tags'] and
                row['sdk_quant']==rows[i-1]['sdk_quant'] and
                row['sdk_scale']==rows[i-1]['sdk_scale']==rows[0]['sdk_scale']):witness.append([i-1,i])
    return {'status':'PASS' if all(r['equal'] and r['packet_tables_equal'] for r in rows) else 'FAIL',
            'matched':sum(r['equal'] for r in rows),'requested':count,
            'hold_witness_pairs':witness,'frames':rows}


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--corpus',type=Path,required=True);p.add_argument('--index-sha256',required=True)
    p.add_argument('--encoder',type=Path,required=True);p.add_argument('--work',type=Path,required=True)
    p.add_argument('--report',type=Path,required=True);p.add_argument('--require-hold',action='store_true');p.add_argument('--sequences-only',action='store_true')
    a=p.parse_args();root=a.corpus.resolve()
    if sha_file(root/'TRANSFER_SHA256.tsv')!=a.index_sha256: raise ValueError('transfer index pin mismatch')
    index=load_transfer_index(root)
    # Verify every indexed artifact, including manifests and original capture traces.
    for rel in index: verified(root,rel,index)
    inventory=json.loads(verified(root,'manifest.json',index).read_text())
    if inventory.get('schema_version')!=1 or inventory.get('profiles')!=list(PROFILES):
        raise ValueError('stale root manifest')
    if inventory.get('capture_source_sha256')!=sha_file(verified(root,'provenance/sdk_packet_capture.cpp',index)):
        raise ValueError('capture source snapshot mismatch')
    expected_sources=['upstream/EncoderSDK/CFHDEncoder.cpp','upstream/EncoderSDK/SampleEncoder.cpp',
                      'upstream/Codec/encoder.c','upstream/Codec/quantize.c','upstream/Codec/config.h']
    frozen=inventory.get('frozen_source_sha256',{})
    if set(frozen)!=set(expected_sources): raise ValueError('missing frozen SDK provenance')
    for rel,digest in frozen.items():
        if sha_file(Path(__file__).resolve().parents[1]/rel)!=digest:
            raise ValueError('frozen SDK source hash mismatch: '+rel)
    a.work.mkdir(parents=True,exist_ok=True)
    run=Path(tempfile.mkdtemp(prefix='run-',dir=str(a.work)))
    out={}
    for profile in PROFILES:
        for kind,count in [('sequence-120f',120),('hold-ramp',40)]:
            if kind=='hold-ramp' and (profile=='filmscan1' or a.sequences_only):continue
            out[profile+'/'+kind]=measure(root,index,a.encoder.resolve(),run,profile,kind,count)
            print(profile+'/'+kind+' '+out[profile+'/'+kind]['status']+' '+str(out[profile+'/'+kind]['matched'])+'/'+str(count),flush=True)
    a.report.parent.mkdir(parents=True,exist_ok=True);a.report.write_text(json.dumps(out,indent=1)+'\n')
    failed=any(r['status']!='PASS' for r in out.values())
    missing_hold=[p for p in PROFILES[:3] if not out.get(p+'/hold-ramp',{}).get('hold_witness_pairs')]
    print(json.dumps({k:{x:v[x] for x in ('status','matched','requested','hold_witness_pairs')} for k,v in out.items()}))
    if a.require_hold and missing_hold: print('FAIL: no real packet hold witness for '+','.join(missing_hold));failed=True
    return int(failed)

if __name__=='__main__': raise SystemExit(main())
