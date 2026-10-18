#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
"""Derive producer/consumer ordering from measured inverse boundary events."""
import argparse
import json
from pathlib import Path
import sys
import contract
from measure import sha


def dependency(call, channel):
    events=[e for e in call['temporal_events'] if e['channel']==channel]
    if len(events)!=2 or {e['input_wavelet'] for e in events}!={1,2}:
        raise ValueError('missing/duplicate dependency')
    producer=next(e for e in events if e['input_wavelet']==2)
    consumer=next(e for e in events if e['input_wavelet']==1)
    if producer['output_width']!=consumer['width'] or producer['output_height']!=consumer['height']:
        raise ValueError('non-matching dependency geometry')
    for e in events:
        if type(e['begin']) is not int or type(e['end']) is not int or not 0<e['begin']<e['end']:
            raise ValueError('invalid boundary order')
    orders={producer['begin'],producer['end'],consumer['begin'],consumer['end']}
    if len(orders)!=4:
        raise ValueError('duplicate temporal ordinal')
    valid=consumer['valid_flags_before'];started=consumer['started_flags_before']
    if type(valid) is not int or type(started) is not int:
        raise ValueError('invalid completion flags')
    return dict(channel=channel,process=call['process'],ordinal=call['ordinal'],
        source_psnr_db=call['psnr_db'],producer_begin=producer['begin'],producer_end=producer['end'],
        consumer_begin=consumer['begin'],consumer_end=consumer['end'],
        consumer_return_before_producer_entry=consumer['end']<producer['begin'],
        consumer_entry_before_producer_return=consumer['begin']<producer['end'],
        all_valid_macro=consumer['all_valid_before'],valid_flags_before=valid,started_flags_before=started,
        lowpass_valid=bool(valid&1),required_four_bands_valid=(valid&15)==15,
        producer_output_sha256=producer['output_after']['sha256'],
        consumer_lowpass_before_sha256=consumer['input_before']['band_sha256'][0],
        consumer_lowpass_after_sha256=consumer['input_after']['band_sha256'][0],
        before_matches_producer=producer['output_after']['sha256']==consumer['input_before']['band_sha256'][0],
        after_matches_producer=producer['output_after']['sha256']==consumer['input_after']['band_sha256'][0])


def analyze(document):
    cases=[]
    for case in document['temporal_cases']:
        records=[dependency(call,c) for call in case['calls'] for c in range(3)]
        suspects=[r for r in records if not r['lowpass_valid'] or r['consumer_return_before_producer_entry'] or not r['before_matches_producer']]
        cases.append(dict(geometry=case['geometry'],sample_sha256=case['sample_sha256'],
            dependencies=len(records),missing_lowpass_at_entry=sum(not r['lowpass_valid'] for r in records),
            consumer_return_before_producer_entry=sum(r['consumer_return_before_producer_entry'] for r in records),
            macro_true_with_incomplete_required_bands=sum(r['all_valid_macro'] is True and not r['required_four_bands_valid'] for r in records),
            mismatched_consumer_lowpass=sum(not r['before_matches_producer'] for r in records),observations=suspects))
    return dict(schema_version=1,copyright='Copyright (c) 2026 HyperCine Contributors.',license='MIT OR Apache-2.0',
        release_blocked=True,cases=cases,
        interpretation='Original BANDS_ALL_VALID equality can be true for valid=started=0x0e, despite missing required lowpass bit. Observed ordering concerns isolated instrumented decoder only.',
        limitations='Entry/return counters bracket observer snapshots and original calls, not individual instructions. Overlap alone is not proof; consumer-return-before-producer-entry is the stronger observation. Original M4 failing packet remains unavailable.')


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('evidence',type=Path);p.add_argument('--output',type=Path,required=True)
    args=p.parse_args()
    try:
        result=analyze(contract.strict_document(args.evidence.read_text()))
        result['input_evidence_sha256']=sha(args.evidence.read_bytes())
        result['analyzer_sha256']=sha(Path(__file__).read_bytes())
        args.output.write_text(json.dumps(result,indent=2,allow_nan=False)+'\n',encoding='utf-8')
        print(json.dumps([{k:v for k,v in c.items() if k!='observations'} for c in result['cases']]))
        return 1
    except (OSError,ValueError,KeyError,TypeError,RecursionError):
        print('INVALID_DEPENDENCY_EVIDENCE: release blocked',file=sys.stderr);return 2


if __name__=='__main__':
    raise SystemExit(main())
