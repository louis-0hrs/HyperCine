#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
"""Bounded diagnostic-only inverse dependency intervention, never release PASS."""
import argparse
import json
from pathlib import Path
import re
import statistics
import subprocess
import sys
import time
import analyze_dependency
import contract
from bgra_ab import collect
from measure import run, sha
from priority_probe import temporal_hashes

TIMEOUT_SECONDS = 20  # Whole child process, including its five decoder opens.
INPUTS = {
    '128': ('60ed81a3f164d585ae4d3dc0c4af4154a626cef12311a61b4d2dcca80be04b14',
            'eead8e3032e188be2e13336cad0908ecd30d4fd61c6dcd15ff19e46c67b2035e'),
    '256': ('7655842a920a1d2f35f1cf568b21e99232004ce2a71e9ea5f4cf708da586f26e',
            '5cebced3caab3369b8b0007c3f36d43d1c356ecff54f49424ac35563055f8868')}


def bounded(command, timeout=TIMEOUT_SECONDS):
    # subprocess.run kills and reaps the child on TimeoutExpired, including all
    # SDK worker threads in that process. Never parse its partial pixel output.
    result = subprocess.run(list(map(str, command)), capture_output=True, timeout=timeout)
    if result.returncode:
        raise ValueError('diagnostic child failed')
    return result.stdout


def timing(calls):
    ns = [c['decode_ns'] for c in calls]
    if not ns or any(type(n) is not int or n <= 0 for n in ns):
        raise ValueError('missing/non-positive decode timing')
    return dict(median_ms=statistics.median(ns)/1e6, max_ms=max(ns)/1e6,
                scope='CFHD_DecodeSample only; observer copies inside call included; sidecar I/O and teardown excluded')


def temporal(case, root, enforced):
    dependencies = []
    for call in case['calls']:
        prefix = root/('decode-bgra-on-'+str(call['process'])+'-'+str(call['ordinal']))
        meta = contract.strict_document(Path(str(prefix)+'.temporal.json').read_text())
        if meta['dependency_enforced'] is not enforced:
            raise ValueError('wrong intervention arm')
        call['temporal_events'] = temporal_hashes(Path(str(prefix)+'.temporal').read_bytes(), meta)
        if {(e['channel'], e['input_wavelet']) for e in call['temporal_events']} != {(c,l) for c in range(3) for l in (1,2)}:
            raise ValueError('missing inverse boundary')
        for name in ('transform_workers', 'entropy_workers', 'readiness_checks', 'lowpass_deferrals', 'producer_deferrals'):
            if type(meta[name]) is not int or meta[name] < 0:
                raise ValueError('invalid worker/queue counter')
            call[name] = meta[name]
        if (enforced and meta['readiness_checks'] == 0) or (not enforced and meta['readiness_checks'] != 0):
            raise ValueError('queue interception missing or leaked into baseline')
        for event in call['temporal_events']:
            if type(event['required_producer_completed']) is not bool:
                raise ValueError('invalid producer publication')
        dependencies.extend(analyze_dependency.dependency(call,c) for c in range(3))
    events = [e for c in case['calls'] for e in c['temporal_events']]
    case['dependency_summary'] = dict(
        dependencies=len(dependencies), inverse_events=len(events),
        missing_lowpass_at_any_inverse_entry=sum(not (e['valid_flags_before']&1) for e in events),
        missing_producer_publication=sum(not e['required_producer_completed'] for e in events),
        consumer_entry_before_producer_return=sum(r['consumer_entry_before_producer_return'] for r in dependencies),
        consumer_return_before_producer_entry=sum(r['consumer_return_before_producer_entry'] for r in dependencies),
        mismatched_consumer_lowpass=sum(not r['before_matches_producer'] for r in dependencies),
        inputs_changed_during_call=sum(e['input_changed_during_call'] for e in events),
        lowpass_deferrals=sum(c['lowpass_deferrals'] for c in case['calls']),
        producer_deferrals=sum(c['producer_deferrals'] for c in case['calls']))


def intervention_observed(case):
    """Fail closed on stable corrupt output, missing producer, or absent calls."""
    summary = case['dependency_summary']
    bad = ('missing_lowpass_at_any_inverse_entry', 'missing_producer_publication',
           'consumer_entry_before_producer_return', 'consumer_return_before_producer_entry',
           'mismatched_consumer_lowpass', 'inputs_changed_during_call')
    calls = case['calls']
    return (len(calls) == 15 and summary['dependencies'] == 45 and summary['inverse_events'] == 90 and
            all(summary[k] == 0 for k in bad) and
            case['summary']['distinct_rgb_hashes'] == 1 and
            all(c['decoder_status'] == 0 and c['output_valid'] is True and
                (c['source_pixels_exact'] is True or c['psnr_db'] is not None and c['psnr_db'] > 35.0)
                for c in calls))


def verify_map(worker, target, entropy_origin):
    path = worker.resolve().parents[1]/(target+'.map')
    lines = path.read_text().splitlines()
    origins = {'DecodeSample':'decoder_temporal.obj', 'DecodeEntropy':entropy_origin,
               'TransformInverseSpatialQuantLowpass':'CFHDCodecStatic:wavelet.obj'}
    if any(not any(re.search(r'\s'+symbol+r'\s', line) and origin in line for line in lines)
           for symbol, origin in origins.items()):
        raise ValueError('unverified diagnostic linkage')
    return dict(symbol_origins=origins, map_sha256=sha(path.read_bytes()))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('worker', 'temporal-worker', 'dependency-worker', 'packet128', 'source128',
                 'packet256', 'source256', 'work', 'output'):
        parser.add_argument('--'+name, type=Path, required=True)
    args = parser.parse_args()
    report = dict(schema_version=1, copyright='Copyright (c) 2026 HyperCine Contributors.', license='MIT OR Apache-2.0',
                  release_blocked=True, release_gate='UNRESOLVED', encoder_packets_equal=None,
                  exact_M4_failing_sample='UNAVAILABLE', execution_status='INCOMPLETE', cases=[])
    try:
        args.work.mkdir(parents=True, exist_ok=False)
        root = Path(__file__).resolve().parents[2]
        source_names = ('tests/CMakeLists.txt', 'tests/reference_decode/dependency_ab.py',
            'tests/reference_decode/worker.cpp', 'tests/reference_decode/temporal_observer.cpp',
            'tests/reference_decode/decoder_temporal.c', 'tests/reference_decode/entropy_dependency.c',
            'tests/reference_decode/priority_probe.py', 'tests/reference_decode/analyze_dependency.py',
            'tests/reference_decode/bgra_ab.py', 'tests/reference_decode/measure.py', 'tests/reference_decode/contract.py',
            'upstream/Codec/decoder.c', 'upstream/Codec/entropy_threading.c', 'upstream/Codec/image.h',
            'upstream/Codec/thread.h', 'upstream/Codec/wavelet.c')
        report.update(source_commit=run(['git','rev-parse','HEAD']).decode().strip(),
            source_sha256={n:sha((root/n).read_bytes()) for n in source_names},
            binary_sha256={n:sha(getattr(args,n).read_bytes()) for n in ('worker','temporal_worker','dependency_worker')},
            process_timeout_seconds=TIMEOUT_SECONDS, calls_per_process=5, fresh_processes_per_case_arm=3,
            linkage=dict(observed_original=verify_map(args.temporal_worker,'reference_decode_temporal','CFHDCodecStatic:entropy_threading.obj'),
                         dependency_enforced=verify_map(args.dependency_worker,'reference_decode_dependency','entropy_dependency.obj')))
        for geometry in ('128','256'):
            packet = getattr(args,'packet'+geometry).resolve()
            source = getattr(args,'source'+geometry).read_bytes()
            if (sha(packet.read_bytes()), sha(source)) != INPUTS[geometry]:
                raise ValueError('historical immutable input identity mismatch')
            for arm, worker, enforced in (('unmodified_sdk', args.worker, None),
                    ('observed_original', args.temporal_worker, False), ('dependency_enforced', args.dependency_worker, True)):
                report['active_case'] = dict(geometry=int(geometry), arm=arm)
                destination = args.work/(geometry+'-'+arm)
                destination.mkdir()
                began = time.monotonic()
                case = collect(worker.resolve(), packet, source, geometry, True, 3, destination, runner=bounded)
                if sha(packet.read_bytes()) != INPUTS[geometry][0]:
                    raise ValueError('immutable packet changed')
                case.update(arm=arm, subprocess_timeout_occurred=False, total_arm_wall_ms=(time.monotonic()-began)*1000,
                            decode_timing=timing(case['calls']))
                if enforced is not None:
                    temporal(case, destination, enforced)
                report['cases'].append(case)
        report.pop('active_case')
        report['execution_status'] = 'COMPLETED'
        report['diagnostic_intervention'] = ('OBSERVED_BOUNDED_EFFECT' if all(intervention_observed(c) for c in
            report['cases'] if c['arm']=='dependency_enforced') else 'INTERVENTION_FAILED')
        report['timing_comparisons'] = []
        for geometry in (128,256):
            cases = {c['arm']:c for c in report['cases'] if c['geometry']==[geometry,geometry]}
            for arm in ('observed_original','dependency_enforced'):
                for baseline in ('unmodified_sdk','observed_original'):
                    if arm == baseline:
                        continue
                    a,b = cases[arm]['decode_timing'],cases[baseline]['decode_timing']
                    report['timing_comparisons'].append(dict(geometry=geometry,arm=arm,baseline=baseline,
                        median_ratio=a['median_ms']/b['median_ms'],max_ratio=a['max_ms']/b['max_ms']))
    except subprocess.TimeoutExpired:
        report.update(execution_status='TIMEOUT_CHILD_KILLED_AND_REAPED', diagnostic_intervention='INCOMPLETE',
                      subprocess_timeout_occurred=True)
    except (OSError, ValueError, KeyError, TypeError, RecursionError, subprocess.SubprocessError):
        report.update(execution_status='INVALID_MEASUREMENT', diagnostic_intervention='INCOMPLETE')
    args.output.write_text(json.dumps(report,indent=2,allow_nan=False)+'\n',encoding='utf-8')
    print(json.dumps(dict(execution_status=report['execution_status'],diagnostic_intervention=report['diagnostic_intervention'],
        cases=[dict(geometry=c['geometry'],arm=c['arm'],**c['summary'],timing=c['decode_timing'],
                    dependency=c.get('dependency_summary')) for c in report['cases']])))
    return 1 if report['execution_status']=='COMPLETED' else 2


if __name__ == '__main__':
    raise SystemExit(main())
