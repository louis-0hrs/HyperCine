#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
"""Authenticate #214 buffers and prepare/replay verbatim original SSE2 functions.

No new inverse arithmetic. Native x86 replay is separate from metadata and
private-file authentication; none establishes the archived Windows build path.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import platform
import re
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
MANIFEST = ROOT/'docs/validation/NEXT_INVERSE_STAGE_CONTRACT_144.json'
PHASE_A = Path('golden/windows/gop2/issue189-20261019-r1')
PHASE_B = Path('golden/windows/gop2/issue189-20261019-phaseb-r1')
SUPPORT_SHA256 = {
    'buffer.h': '8fe121a32fc0af8b0f97df52b7cdbbc6ae4eff4bf73c8dd04ac389301b55de9e',
    'buffer.c': '4de9e0e92417ba38d58843719c152aee1444b3e9749a3fb1ceb310f452e808ea',
    'image.h': '93ed2e580030274dda46257eaf08ac90717f2a526ccc11fb5b471dcdb3bf1d52',
    'temporal.c': 'c4e237f49f02cb9236c1a7eb08c54d2b943189ccb29a74648f9941513504ad87',
}



def unique(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError('Duplicate JSON key')
        result[key] = value
    return result


def read_json(path):
    return json.loads(path.read_text(encoding='utf-8'), object_pairs_hook=unique)


def identity(data):
    return {'byte_length': len(data), 'sha256': hashlib.sha256(data).hexdigest()}


def require(ok):
    if not ok:
        raise ValueError('Original binding/source mismatch')


def committed_bytes(name, revision='HEAD'):
    """Read canonical repository bytes; private corpus files stay byte-exact."""
    require(isinstance(name, str) and bool(name))
    path = PurePosixPath(name)
    require(not path.is_absolute() and '..' not in path.parts)
    require(str(path) == name and not any(c in name for c in ('\\', ':', '\0')))
    require(revision == 'HEAD' or re.fullmatch(r'[0-9a-f]{40,64}', revision) is not None)
    env = dict(os.environ, GIT_NO_REPLACE_OBJECTS='1')
    return subprocess.run(
        ['git', '--no-replace-objects', 'cat-file', 'blob', f'{revision}:{name}'],
        cwd=ROOT, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        check=True, timeout=30).stdout


def head_revision():
    return subprocess.run(
        ['git', '--no-replace-objects', 'rev-parse', '--verify', 'HEAD'],
        cwd=ROOT, env=dict(os.environ, GIT_NO_REPLACE_OBJECTS='1'),
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=True,
        timeout=30).stdout.decode('ascii').strip()


def audit():
    revision = head_revision()
    pins = read_json(MANIFEST)
    require(pins['schema']=='hypercine.next_inverse_stage_contract/1')
    require(pins['implementation_authorized'] is False)
    for name, sha in SUPPORT_SHA256.items():
        require(identity(committed_bytes('upstream/Codec/'+name, revision))['sha256']==sha)
    indices = {}
    for entry in pins['source_fingerprints']+pins['reference_indices']:
        data = committed_bytes(entry['id'], revision)
        require(identity(data)=={k:entry[k] for k in ('byte_length','sha256')})
        if entry in pins['reference_indices']:
            indices[entry['id']] = json.loads(data, object_pairs_hook=unique)
    require({c['id'] for c in pins['cases']} ==
            {f'{g}_ch{ch}_{name}' for g in ('1920x1080','3840x2160')
             for ch,name in enumerate(('Y','V','U'))})
    require(len(pins['cases'])==6)
    for case in pins['cases']:
        require(len(case['inputs'])==4)
        for ordinal,item in enumerate(case['inputs']+[case['expected']]):
            idx = indices[item['index_id']]
            inv = idx.get('original_stages',idx.get('artifacts'))
            orig = next(x for x in inv if x['id']==item['id'])
            require(all(orig[k]==v for k,v in item.items() if k!='index_id'))
            s = item['stage']; ch=case['inputs'][0]['stage']['channel']
            require((s['wavelet'],s['band'],s['channel'],s['level'],s['prescale']) ==
                    ((4,ordinal,ch,3,2) if ordinal<4 else (2,0,ch,2,0)))
            require(s['pitch']==2*s['width'] and item['byte_length']==s['pitch']*s['height'])
            require(s['width']==case['inputs'][0]['stage']['width']*(1 if ordinal<4 else 2))
            require(s['height']==case['inputs'][0]['stage']['height']*(1 if ordinal<4 else 2))
            packet = idx.get('input_packet')
            if packet is None:
                packet = next(x for x in inv if x['id']=='original_sdk.cfhd')
                packet = {k:packet[k] for k in ('sha256','byte_length')}
            require(packet==case['packet'])
    return pins


def authenticate(root, pins):
    cases=[]
    for case in pins['cases']:
        geometry=case['id'].split('_')[0]; files=[]
        for ordinal,item in enumerate(case['inputs']+[case['expected']]):
            name=Path(item['id'])
            require(not name.is_absolute() and '..' not in name.parts)
            base=(PHASE_B/geometry/'original' if ordinal in (1,2,3) else PHASE_A/geometry)
            path=root/base/name
            data=path.read_bytes()
            require(identity(data)=={k:item[k] for k in ('byte_length','sha256')})
            side=read_json(path.with_name(path.name+'.json'))
            require(side['status']=='AVAILABLE')
            require(all(side[k]==item[k] for k in ('sha256','byte_length','stage','producer','representation')))
            if ordinal in (1,2,3):
                require(side['input_packet']==case['packet'])
            else:
                require(case['packet']['sha256'] in side['input_sha256'])
            require(path.with_name(path.name+'.sha256').read_text()==item['sha256']+'  '+path.name+'\n')
            files.append(data)
        cases.append((case,files))
    return cases


def function(text, signature, occurrence=0):
    # Original function closing braces occupy their own unindented line.
    starts=[m.start() for m in re.finditer(re.escape(signature),text)]
    start=starts[occurrence]; end=text.index('\n}\n',start)+3
    result=text[start:end]
    require(result.count('{')==result.count('}'))
    return result


def source():
    revision = head_revision()
    def text(name):
        return committed_bytes('upstream/Codec/'+name, revision).decode('utf-8').replace('\r\n', '\n')
    spatial=text('spatial.c'); horizontal=text('InvertHorizontalStrip16s.c')
    # Keep the original copyright/license alongside every extracted function.
    license=spatial[:spatial.index('*/')+2]
    macros=[]
    for name,keys in [('image.h',('_SATURATE','SATURATE_16S','SATURATE','ALIGN16')),
                      ('filter.h',('ROUNDING','DivideByShift'))]:
        content=text(name)
        for key in keys:
            lines=re.findall(r'^#define '+re.escape(key)+r'(?=[\s(]).*$',content,re.M)
            require(bool(lines)); macros.append(lines[-1])
    header='''
#include <stddef.h>
#include <stdint.h>
#include <assert.h>
#include <emmintrin.h>
#include "buffer.h"
typedef int16_t PIXEL;
typedef struct { int width, height; } ROI;
#define XMMOPT 1
#define _FASTLOOP 1
#define _UNALIGNED 0
#define _DEQUANTIZE_IN_FSM 1
#define DEBUG 0
'''
    return '\n'.join([license,header,*macros,
        function(text('buffer.c'),'char *AllocScratchBuffer('),
        function(horizontal,'void InvertHorizontalStripDescale16s(',2),
        function(spatial,'void InvertSpatialQuantDescale16s(')])


def replay(cases,probe):
    results=[]
    for case,files in cases:
        s=case['inputs'][0]['stage']; width=s['width']*2
        command=[str(probe)]
        run=subprocess.run(command,input=struct.pack('<II',s['width'],s['height'])+b''.join(files[:4]),
                           stdout=subprocess.PIPE,stderr=subprocess.PIPE,timeout=30,check=True)
        actual=run.stdout; expected=files[4]
        require(len(actual)==len(expected))
        differences=[i for i in range(len(expected)//2) if actual[2*i:2*i+2]!=expected[2*i:2*i+2]]
        result={'id':case['id'],'equal':not differences,'mismatched_signed_samples':len(differences),
                'actual_sha256':hashlib.sha256(actual).hexdigest(),
                'expected_sha256':hashlib.sha256(expected).hexdigest()}
        if differences:
            i=differences[0]
            result['first_difference']={'x':i%width,'y':i//width,
                'actual':struct.unpack_from('<h',actual,2*i)[0],
                'expected':struct.unpack_from('<h',expected,2*i)[0]}
        results.append(result)
    return results


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--prepare-dir',type=Path)
    ap.add_argument('--authenticate',action='store_true')
    ap.add_argument('--probe',type=Path)
    args=ap.parse_args(); pins=audit()
    report={'metadata':'PASS','cases':6,'artifact_bindings':30,
            'private_authentication':'NOT_RUN','original_sse2_replay':'NOT_RUN',
            'archived_windows_build_signoff':'SEPARATE_OWNER_GATE'}
    if args.prepare_dir:
        prepared_source=source()
        args.prepare_dir.mkdir()  # Refuse reuse/overwrite of a previous capture.
        (args.prepare_dir/'original_descale.inc').write_bytes(prepared_source.encode('utf-8'))
        (args.prepare_dir/'buffer.h').write_bytes(committed_bytes('upstream/Codec/buffer.h'))
        report['original_source_preparation']='PASS'
        report['prepared_source_sha256']=identity(prepared_source.encode())['sha256']
    if args.authenticate or args.probe:
        bound=os.environ.get('HYPERCINE_GOLDEN_DIR')
        require(bool(bound))
        cases=authenticate(Path(bound),pins); report['private_authentication']='PASS_30_ARTIFACTS_AND_SIDECARS'
        if args.probe:
            require(platform.machine().lower() in ('x86_64','amd64') and sys.byteorder=='little')
            report['probe_sha256']=identity(args.probe.read_bytes())['sha256']
            report['host_architecture']=platform.machine()
            report['prepared_source_sha256']=identity(source().encode())['sha256']
            try:
                report['results']=replay(cases,args.probe.resolve())
            finally:
                # Also recheck after probe failures, timeouts and mismatches.
                require(authenticate(Path(bound),pins)==cases)
            report['original_sse2_replay']='PASS_6_OF_6' if all(r['equal'] for r in report['results']) else 'MISMATCH'
    print(json.dumps(report,indent=2))
    return 1 if report['original_sse2_replay']=='MISMATCH' else 0


if __name__=='__main__':
    try:
        sys.exit(main())
    except (OSError,ValueError,KeyError,TypeError,IndexError,StopIteration,subprocess.SubprocessError):
        print('Original Descale review BLOCKED or INVALID; paths/details redacted.',file=sys.stderr)
        sys.exit(1)
