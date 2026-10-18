#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
"""Pre-registered CPU-affinity and SMT-topology A/B/C protocol for #157.

This is a versioned derivative of the frozen #138 matched-SHA 1080p BGRA
protocol. It changes only process placement and records the requested and
effective affinity before the worker can create an SDK decoder. The frozen
#138 script, worker and report are not modified.

No GPU/CUDA, production decoder, host setting or release gate is changed.
"""
import argparse
import json
import platform
import shutil
import subprocess
import sys
from collections import Counter
from datetime import datetime, timezone
from pathlib import Path

import contract
from measure import metrics, sha
from bgra_ab import quality, rgb, validate
from cross_host_1080 import build_config, cpu_model, threading_defaults

PACKET_SHA = '2b9a28dec5b015bf76a1ba3bce5a458167bd07bd061dbb59a97c00eece4fec52'
SOURCE_SHA = '45c166c3d6a3710d620ff5d738980224edf53cf9ce1bceb87267e474294afff1'
PROTOCOL_ID = 'topology-affinity-ab-1080-bgra-full-20261018'
PROTOCOL_VERSION = 1
GEOMETRY = (1920, 1080)
CALLS_PER_PROCESS = 5
PROCESSES_PER_ARM = 9
PRIMARY_PROCESSES = 3
EXTENSION_PROCESSES = 6
CHILD_TIMEOUT_SECONDS = 20
LAUNCHER_TIMEOUT_SECONDS = 40
# Nine balanced three-arm cycles. Each arm appears nine times and each arm
# occupies each position in the cycle three times, so slow host drift is not
# systematically assigned to one arm.
RUN_PERMUTATIONS = ('ABC', 'BCA', 'CAB', 'CBA', 'ACB', 'BAC', 'ABC', 'BCA', 'CAB')
# Optional fourth arm (used only if explicitly requested after A-C review).
RUN_PERMUTATIONS_D = ('ABCD', 'BCDA', 'CDAB', 'DABC', 'ABCD', 'BCDA', 'CDAB',
                      'DABC', 'ABCD')
ARM_IDS = ('A', 'B', 'C', 'D')


def popcount(mask):
    return int(mask).bit_count()


def lowest_bit(mask):
    return mask & -mask


def hex_mask(mask):
    return hex(int(mask))


def run_sequence(arms):
    if 'D' in arms:
        source = RUN_PERMUTATIONS_D
    else:
        source = RUN_PERMUTATIONS
    sequence = [arm for permutation in source for arm in permutation if arm in arms]
    counts = Counter(sequence)
    if any(counts[arm] != PROCESSES_PER_ARM for arm in arms):
        raise ValueError('run order does not give every selected arm nine processes')
    return sequence


def arm_masks(topology, arms):
    """Return requested masks and a BLOCKED_TOPOLOGY reason per arm."""
    if not topology.get('topology_available'):
        raise ValueError('processor topology is unavailable')
    if topology.get('processor_groups') != 1:
        raise ValueError('multi-group topology is not supported by this protocol')
    system = int(topology['system_affinity_mask'], 16)
    process = int(topology['process_affinity_mask'], 16)
    active = topology.get('active_logical_processors')
    if system == 0 or system != process or popcount(system) != active:
        raise ValueError('launcher process affinity is not the full single-group system mask')
    cores = []
    for core in topology.get('cores', []):
        if core.get('group') != 0:
            raise ValueError('processor core outside group 0')
        mask = int(core['mask'], 16)
        if mask == 0 or (mask & ~system) != 0:
            raise ValueError('invalid processor core mask')
        cores.append(mask)
    if len(cores) != topology.get('core_count'):
        raise ValueError('processor core count mismatch')
    if sum(popcount(core) for core in cores) != popcount(system):
        raise ValueError('core masks do not partition the system affinity mask')

    masks = {'A': {'requested': 'unrestricted', 'mask': system, 'reason': None}}
    if 'B' in arms:
        reason = None
        if len(cores) != 8:
            reason = 'expected eight physical cores for arm B'
        elif any(popcount(core) != 2 for core in cores):
            reason = 'expected two SMT siblings per physical core'
        elif popcount(sum(lowest_bit(core) for core in cores)) != 8:
            reason = 'one-sibling-per-core mask is not exactly eight logical processors'
        masks['B'] = {
            'requested': hex_mask(sum(lowest_bit(core) for core in cores)),
            'mask': sum(lowest_bit(core) for core in cores),
            'reason': reason,
        }
    if 'C' in arms:
        reason = None
        eligible = [core for core in cores if popcount(core) == 2]
        if len(eligible) < 4:
            reason = 'fewer than four physical cores expose two SMT siblings'
        elif popcount(sum(eligible[:4])) != 8:
            reason = 'both-siblings-from-four-cores mask is not exactly eight logical processors'
        selected = sum(eligible[:4])
        masks['C'] = {'requested': hex_mask(selected), 'mask': selected, 'reason': reason}
    if 'D' in arms:
        reason = None
        if len(cores) < 4:
            reason = 'fewer than four physical cores'
        elif popcount(sum(lowest_bit(core) for core in cores[:4])) != 4:
            reason = 'four-logical-processor mask is not exactly four'
        selected = sum(lowest_bit(core) for core in cores[:4])
        masks['D'] = {'requested': hex_mask(selected), 'mask': selected, 'reason': reason}
    return masks


def first_difference(reference, pixels, width):
    difference = next((index for index, (a, b) in enumerate(zip(reference, pixels))
                       if a != b), None)
    if difference is None:
        return None
    return dict(byte_offset=difference, x=difference // 4 % width,
                y=difference // (width * 4), channel='BGRA'[difference % 4],
                reference=reference[difference], actual=pixels[difference])


def compare(reference, pixels, width):
    if reference == pixels:
        return dict(changed_bytes=0, max_abs_delta=0, psnr_db=None,
                    source_pixels_exact=True), None
    return metrics(reference, pixels), first_difference(reference, pixels, width)


def arm_summary(calls, processes):
    hashes = [call['pixel_sha256'] for call in calls]
    counts = Counter(hashes)
    modal = sorted(counts, key=lambda value: (-counts[value], value))[0] if counts else None
    per_process = []
    for process in sorted(processes):
        subset = [call for call in calls if call['process'] == process]
        subset_hashes = [call['pixel_sha256'] for call in subset]
        per_process.append(dict(
            process=process, calls=len(subset),
            distinct_pixel_hashes=len(set(subset_hashes)),
            within_process_divergent=len(set(subset_hashes)) > 1,
            divergent_vs_arm_modal=any(value != modal for value in subset_hashes),
            pixel_sha256s=sorted(set(subset_hashes))))
    psnr = [call['psnr_db'] for call in calls if call.get('psnr_db') is not None]
    decode = sorted(call['decode_ns'] / 1e6 for call in calls)
    wall = sorted(call['wall_ns'] / 1e6 for call in calls)
    return dict(
        processes=len(processes), calls=len(calls),
        distinct_pixel_hashes=len(set(hashes)),
        distinct_rgb_hashes=len({call['rgb_sha256'] for call in calls}),
        modal_pixel_sha256=modal,
        divergent_calls_vs_modal=sum(value != modal for value in hashes),
        per_call_divergent_rate=(sum(value != modal for value in hashes) / len(hashes)
                                 if hashes else None),
        divergent_processes=sum(item['within_process_divergent'] for item in per_process),
        per_process_event_rate=(sum(item['within_process_divergent'] for item in per_process)
                                / len(per_process) if per_process else None),
        per_process=per_process,
        psnr_min_db=min(psnr) if psnr else None,
        psnr_max_db=max(psnr) if psnr else None,
        decode_median_ms=decode[len(decode) // 2] if decode else None,
        decode_max_ms=decode[-1] if decode else None,
        wall_median_ms=wall[len(wall) // 2] if wall else None,
        wall_max_ms=wall[-1] if wall else None,
        decoder_statuses=sorted({call['decoder_status'] for call in calls}))


def parse_rows(stdout, width, height):
    rows = [contract.strict_document(line.decode('utf-8'))
            for line in stdout.splitlines() if line.strip()]
    if len(rows) != CALLS_PER_PROCESS or [row['ordinal'] for row in rows] != list(
            range(1, CALLS_PER_PROCESS + 1)):
        raise ValueError('incomplete worker observation')
    for row in rows:
        validate(row, True, width, height)
        if not row['output_valid'] or row['decoder_status'] != 0:
            raise ValueError('worker output invalid')
    return rows


def collect_call(row, arm, process, phase, run_position, occurrence, prefix,
                 source, width, height, arm_reference, process_reference,
                 quality_cache, rgb_cache, kept_outputs, output_root):
    ordinal = row['ordinal']
    output_path = Path(str(prefix) + '-' + str(ordinal) + '.bgra')
    pixels = output_path.read_bytes()
    if len(pixels) != width * height * 4:
        raise ValueError('invalid output buffer')
    pixel_sha = sha(pixels)
    if pixel_sha not in rgb_cache:
        rgb_cache[pixel_sha] = sha(rgb(pixels))
    if pixel_sha not in quality_cache:
        quality_cache[pixel_sha] = quality(source, pixels, width, height)
    row.update(
        arm=arm, process=process, phase=phase, run_position=run_position,
        occurrence=occurrence, sample_sha256=PACKET_SHA,
        pixel_sha256=pixel_sha, rgb_sha256=rgb_cache[pixel_sha],
        source_sha256=SOURCE_SHA, **quality_cache[pixel_sha])

    if arm_reference['pixels'] is None:
        arm_reference.update(pixels=pixels, process=process, ordinal=ordinal,
                             run_position=run_position)
    versus_arm, difference_arm = compare(arm_reference['pixels'], pixels, width)
    row['versus_arm_first'] = versus_arm
    row['first_difference_vs_arm_first'] = difference_arm
    row['arm_first_process'] = arm_reference['process']
    row['arm_first_ordinal'] = arm_reference['ordinal']
    row['arm_first_run_position'] = arm_reference['run_position']

    if process_reference['pixels'] is None:
        process_reference.update(pixels=pixels, ordinal=ordinal)
    versus_process, difference_process = compare(process_reference['pixels'], pixels, width)
    row['versus_process_first'] = versus_process
    row['first_difference_vs_process_first'] = difference_process
    row['process_first_ordinal'] = process_reference['ordinal']

    kept = kept_outputs.setdefault(arm, set())
    if pixel_sha not in kept:
        kept.add(pixel_sha)
        destination = output_root / arm / (pixel_sha + '.bgra')
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(output_path, destination)
    output_path.unlink()
    return row


def execute_process(launcher, worker, packet, prefix, mask, meta_path, raw_root,
                    run_position, arm):
    command = [str(launcher), '--mask', mask, '--meta', str(meta_path), '--',
               str(worker), 'decode-bgra-on', '1080', str(packet), str(prefix)]
    meta = None
    stdout = b''
    stderr = b''
    timed_out = False
    try:
        result = subprocess.run(command, capture_output=True,
                                timeout=LAUNCHER_TIMEOUT_SECONDS)
        stdout, stderr = result.stdout, result.stderr
        returncode = result.returncode
    except subprocess.TimeoutExpired as error:
        timed_out = True
        stdout = error.stdout or b''
        stderr = error.stderr or b''
        returncode = None
    if meta_path.is_file():
        try:
            meta = contract.strict_document(meta_path.read_text(encoding='utf-8'))
        except (OSError, ValueError):
            meta = None
    raw_root.mkdir(parents=True, exist_ok=True)
    (raw_root / ('run-%02d-%s.stdout.jsonl' % (run_position, arm))).write_bytes(stdout)
    (raw_root / ('run-%02d-%s.stderr.txt' % (run_position, arm))).write_bytes(stderr)
    return dict(returncode=returncode, timed_out=timed_out, meta=meta,
                stdout=stdout, stderr=stderr)


def invalid_reason(execution):
    if execution['timed_out']:
        return 'LAUNCHER_TIMEOUT'
    meta = execution['meta']
    if isinstance(meta, dict):
        if meta.get('affinity_ok') is not True:
            return 'AFFINITY_NOT_APPLIED'
        if meta.get('timeout') is True:
            return 'CHILD_TIMEOUT'
    if execution['returncode'] != 0:
        return 'CHILD_EXIT_%s' % execution['returncode']
    if not isinstance(meta, dict):
        return 'MISSING_META'
    return None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--worker', type=Path, required=True)
    parser.add_argument('--launcher', type=Path, required=True)
    parser.add_argument('--packet', type=Path, required=True)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--work', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--build', type=Path)
    parser.add_argument('--arms', default='A,B,C')
    args = parser.parse_args()
    try:
        arms = tuple(part.strip().upper() for part in args.arms.split(',') if part.strip())
        if not arms or any(arm not in ARM_IDS for arm in arms) or len(set(arms)) != len(arms):
            raise ValueError('invalid arm selection')
        root = Path(__file__).resolve().parents[2]
        build_dir = args.build.resolve() if args.build else root / 'build'
        packet_bytes = args.packet.read_bytes()
        source = args.source.read_bytes()
        if sha(packet_bytes) != PACKET_SHA or sha(source) != SOURCE_SHA:
            raise ValueError('pre-registered packet/source identity mismatch')
        if args.work.exists():
            raise ValueError('work directory must be fresh')
        args.work.mkdir(parents=True)
        raw_root = args.work / 'raw'
        output_root = args.work / 'outputs'
        pixels_root = args.work / 'pixels'
        raw_root.mkdir()
        output_root.mkdir()
        pixels_root.mkdir()
        width, height = GEOMETRY

        topology_result = subprocess.run([str(args.launcher.resolve()), '--topology'],
                                         capture_output=True, timeout=30)
        if topology_result.returncode:
            raise ValueError('topology probe failed')
        topology = contract.strict_document(topology_result.stdout.decode('utf-8'))
        masks = arm_masks(topology, arms)
        sequence = run_sequence(arms)
        probe_slots = tuple(range(0, len(sequence) + 1, PROCESSES_PER_ARM))

        source_commit = subprocess.run(
            ['git', 'rev-parse', 'HEAD'], cwd=root, capture_output=True,
            timeout=30, check=True).stdout.decode().strip()
        source_files = [
            Path(__file__), Path(__file__).with_name('affinity_launcher.cpp'),
            Path(__file__).with_name('worker.cpp'),
            Path(__file__).with_name('cross_host_1080.py'),
            Path(contract.__file__), Path(__file__).with_name('measure.py'),
            Path(__file__).with_name('bgra_ab.py'),
            root / 'docs/validation/TOPOLOGY_AFFINITY_AB_PROTOCOL.md',
            root / 'include/hypercine/canonical.h',
            root / 'upstream/Codec/config.h',
            root / 'upstream/Codec/codec.h',
            root / 'upstream/DecoderSDK/SampleDecoder.cpp',
            root / 'upstream/Codec/decoder.c',
            root / 'upstream/Codec/InvertHorizontalStrip16s.c',
        ]
        manifest = dict(
            schema_version=1,
            copyright='Copyright (c) 2026 HyperCine Contributors.',
            license='MIT OR Apache-2.0',
            protocol=dict(
                id=PROTOCOL_ID, version=PROTOCOL_VERSION, preregistered=True,
                derivative_of='matched-sha-1080-bgra-full-20261018 (#138)',
                frozen_138_source_commit='97c34500324b550da5f6444fe9d28c8729dea8d2',
                geometry=list(GEOMETRY), requested_format='BGRA',
                decoded_resolution='CFHD_DECODED_RESOLUTION_FULL',
                decoding_flags='CFHD_DECODING_FLAGS_NONE',
                active_metadata=True, processing_flags=65535,
                processes_per_arm=PROCESSES_PER_ARM,
                primary_processes=PRIMARY_PROCESSES,
                extension_processes=EXTENSION_PROCESSES,
                opens_per_process=CALLS_PER_PROCESS,
                child_timeout_seconds=CHILD_TIMEOUT_SECONDS,
                run_order=list(sequence),
                baseline_probe_slots=list(probe_slots),
                criteria='affinity not applied, timeout, decode error or invalid geometry '
                         '=> INVALID; more than one pixel hash in an arm => '
                         'NONDETERMINISTIC; single stable hash per arm with matching modal '
                         '=> NO_EFFECT_DETECTED in this sample only; release remains blocked'),
            sources=dict(source_sha256={path.relative_to(root).as_posix(): sha(path.read_bytes())
                                        for path in source_files},
                         source_commit=source_commit),
            host=dict(system=platform.system(), machine=platform.machine(),
                      python=platform.python_version(), **cpu_model(),
                      processor_groups=topology.get('processor_groups'),
                      active_logical_processors=topology.get('active_logical_processors'),
                      core_count=topology.get('core_count'),
                      smt_core_count=topology.get('smt_core_count'),
                      system_affinity_mask=topology.get('system_affinity_mask'),
                      process_affinity_mask=topology.get('process_affinity_mask')),
            topology=topology,
            build=dict(
                worker_sha256=sha(args.worker.read_bytes()),
                launcher_sha256=sha(args.launcher.read_bytes()),
                worker=build_config(args.worker.resolve(), build_dir),
                launcher=build_config(args.launcher.resolve(), build_dir),
                threading=threading_defaults(root)),
            arms={arm: dict(requested=masks[arm]['requested'],
                            mask=masks[arm]['mask'],
                            blocked_reason=masks[arm]['reason'])
                  for arm in arms},
            generated_utc=datetime.now(timezone.utc).isoformat())
        manifest_path = args.work / 'manifest.json'
        manifest_path.write_text(json.dumps(manifest, indent=2, allow_nan=False) + '\n',
                                 encoding='utf-8')
        manifest_sha = sha(manifest_path.read_bytes())
        print(json.dumps(dict(manifest_sha256=manifest_sha,
                              topology=topology, arms=manifest['arms'])))

        arm_records = {arm: dict(invalid=False, invalid_reason=None, calls=[],
                                 processes=[], skipped_after_invalid=False,
                                 arm_reference=dict(pixels=None, process=None, ordinal=None,
                                                    run_position=None))
                       for arm in arms}
        quality_cache = {}
        rgb_cache = {}
        kept_outputs = {}
        occurrence_counts = Counter()
        probe_records = []

        def run_probe(slot):
            probe_index = len(probe_records) + 1
            prefix = args.work / 'pixels' / ('probe-%02d' % slot)
            meta_path = raw_root / ('probe-%02d.meta.json' % slot)
            execution = execute_process(args.launcher.resolve(), args.worker.resolve(),
                                        args.packet.resolve(), prefix, 'unrestricted',
                                        meta_path, raw_root, slot, 'PROBE')
            reason = invalid_reason(execution)
            record = dict(probe_index=probe_index, run_slot=slot,
                          requested='unrestricted', affinity_ok=False,
                          invalid_reason=reason, calls=[])
            meta = execution['meta'] or {}
            record['effective_mask'] = meta.get('effective_mask')
            record['wall_ns'] = meta.get('wall_ns')
            record['affinity_ok'] = meta.get('affinity_ok') is True
            if reason is None:
                try:
                    rows = parse_rows(execution['stdout'], width, height)
                    for row in rows:
                        output_path = Path(str(prefix) + '-' + str(row['ordinal']) + '.bgra')
                        pixels = output_path.read_bytes()
                        if len(pixels) != width * height * 4:
                            raise ValueError('invalid probe output buffer')
                        pixel_sha = sha(pixels)
                        if pixel_sha not in rgb_cache:
                            rgb_cache[pixel_sha] = sha(rgb(pixels))
                        if pixel_sha not in quality_cache:
                            quality_cache[pixel_sha] = quality(source, pixels, width, height)
                        kept = kept_outputs.setdefault('PROBE', set())
                        if pixel_sha not in kept:
                            kept.add(pixel_sha)
                            destination = output_root / 'PROBE' / (pixel_sha + '.bgra')
                            destination.parent.mkdir(parents=True, exist_ok=True)
                            shutil.copyfile(output_path, destination)
                        row.update(probe_index=probe_index, run_slot=slot,
                                   sample_sha256=PACKET_SHA, pixel_sha256=pixel_sha,
                                   rgb_sha256=rgb_cache[pixel_sha],
                                   source_sha256=SOURCE_SHA, **quality_cache[pixel_sha])
                        record['calls'].append(row)
                        output_path.unlink()
                except (OSError, ValueError):
                    record['invalid_reason'] = 'INVALID_WORKER_OUTPUT'
            probe_records.append(record)

        for slot, arm in enumerate(sequence, start=1):
            if slot - 1 in probe_slots:
                run_probe(slot - 1)
            record = arm_records[arm]
            if record['invalid']:
                continue
            if masks[arm]['reason'] is not None:
                record['invalid'] = True
                record['invalid_reason'] = 'BLOCKED_TOPOLOGY: ' + masks[arm]['reason']
                continue
            occurrence_counts[arm] += 1
            occurrence = occurrence_counts[arm]
            phase = 'primary' if occurrence <= PRIMARY_PROCESSES else 'extension'
            prefix = args.work / 'pixels' / ('run-%02d-%s-p%d' % (slot, arm, occurrence))
            meta_path = raw_root / ('run-%02d-%s-p%d.meta.json' % (slot, arm, occurrence))
            execution = execute_process(args.launcher.resolve(), args.worker.resolve(),
                                        args.packet.resolve(), prefix,
                                        masks[arm]['requested'], meta_path, raw_root,
                                        slot, arm)
            reason = invalid_reason(execution)
            if reason is not None:
                record['invalid'] = True
                record['invalid_reason'] = reason
                record['skipped_after_invalid'] = occurrence < PROCESSES_PER_ARM
                continue
            try:
                rows = parse_rows(execution['stdout'], width, height)
                process_reference = dict(pixels=None, ordinal=None)
                for row in rows:
                    collected = collect_call(
                        row, arm, occurrence, phase, slot, occurrence, prefix, source,
                        width, height, record['arm_reference'], process_reference,
                        quality_cache, rgb_cache, kept_outputs, output_root)
                    collected['wall_ns'] = execution['meta'].get('wall_ns')
                    collected['effective_mask'] = execution['meta'].get('effective_mask')
                    collected['requested_mask'] = execution['meta'].get('requested_mask')
                    record['calls'].append(collected)
                record['processes'].append(dict(
                    process=occurrence, phase=phase, run_position=slot,
                    effective_mask=execution['meta'].get('effective_mask'),
                    requested_mask=execution['meta'].get('requested_mask'),
                    wall_ns=execution['meta'].get('wall_ns'),
                    decoder_statuses=sorted({row['decoder_status'] for row in rows})))
            except (OSError, ValueError):
                record['invalid'] = True
                record['invalid_reason'] = 'INVALID_WORKER_OUTPUT'
                record['skipped_after_invalid'] = occurrence < PROCESSES_PER_ARM
                continue
        if len(sequence) in probe_slots:
            run_probe(len(sequence))

        arms_report = []
        for arm in arms:
            record = arm_records[arm]
            if not record['calls']:
                arms_report.append(dict(
                    id=arm, requested=masks[arm]['requested'],
                    mask=hex_mask(masks[arm]['mask']),
                    blocked_reason=masks[arm]['reason'],
                    status='BLOCKED_TOPOLOGY' if masks[arm]['reason'] else 'INVALID',
                    invalid_reason=record['invalid_reason'], calls=[], summary=None,
                    contract=None))
                continue
            summary = arm_summary(record['calls'],
                                  [entry['process'] for entry in record['processes']])
            contract_document = dict(
                schema_version=1, sample_sha256=PACKET_SHA, encoder_packets_equal=None,
                calls=record['calls'],
                alternate_decoder={'status': 'UNAVAILABLE', 'reason': 'not_measured'})
            contract_result = contract.report(contract_document)
            status = 'INVALID' if record['invalid'] else (
                'NONDETERMINISTIC' if summary['distinct_pixel_hashes'] > 1 else 'STABLE')
            arms_report.append(dict(
                id=arm, requested=masks[arm]['requested'],
                mask=hex_mask(masks[arm]['mask']),
                blocked_reason=masks[arm]['reason'], status=status,
                invalid_reason=record['invalid_reason'],
                skipped_after_invalid=record['skipped_after_invalid'],
                summary=summary, contract=contract_result, calls=record['calls']))

        probe_hashes = [call['pixel_sha256'] for probe in probe_records
                        for call in probe['calls']]
        probe_summary = dict(
            probes=len(probe_records), calls=len(probe_hashes),
            distinct_pixel_hashes=len(set(probe_hashes)),
            affinity_ok=all(probe['affinity_ok'] for probe in probe_records),
            invalid_probes=sum(probe['invalid_reason'] is not None
                               for probe in probe_records),
            modal_pixel_sha256=(sorted(Counter(probe_hashes),
                                       key=lambda value: (-Counter(probe_hashes)[value], value))[0]
                                if probe_hashes else None))

        statuses = {arm['id']: arm['status'] for arm in arms_report}
        modals = {arm['id']: arm['summary']['modal_pixel_sha256']
                  for arm in arms_report if arm.get('summary')}
        blocked = any(status == 'BLOCKED_TOPOLOGY' for status in statuses.values())
        invalid = any(status == 'INVALID' for status in statuses.values())
        nondeterministic = any(status == 'NONDETERMINISTIC' for status in statuses.values())
        stable = any(status == 'STABLE' for status in statuses.values())
        same_modal = len(set(modals.values())) <= 1 if modals else None
        baseline_invalid = (not probe_summary['affinity_ok']
                            or probe_summary['invalid_probes'] > 0)
        baseline_nondeterministic = probe_summary['distinct_pixel_hashes'] > 1
        if blocked:
            outcome = 'BLOCKED_TOPOLOGY'
        elif invalid or baseline_invalid:
            outcome = 'INCONCLUSIVE_PROTOCOL_INVALID'
        elif baseline_nondeterministic:
            outcome = 'BASELINE_NONDETERMINISTIC'
        elif nondeterministic and stable:
            outcome = 'OBSERVED_AFFINITY_ASSOCIATED_DIVERGENCE'
        elif nondeterministic:
            outcome = 'DIVERGENCE_PRESENT_IN_ALL_ARMS'
        elif same_modal:
            outcome = 'NO_EFFECT_DETECTED'
        else:
            outcome = 'OBSERVED_AFFINITY_DEPENDENT_OUTPUT'

        document = dict(
            schema_version=1,
            copyright='Copyright (c) 2026 HyperCine Contributors.',
            license='MIT OR Apache-2.0',
            protocol=manifest['protocol'],
            protocol_manifest_sha256=manifest_sha,
            sources=manifest['sources'],
            host=manifest['host'],
            topology=topology,
            build=manifest['build'],
            arms=arms_report,
            baseline_probes=probe_records,
            baseline_summary=probe_summary,
            comparison=dict(statuses=statuses, modal_pixel_sha256=modals,
                            same_modal_across_arms=same_modal,
                            nondeterministic_arms=[arm for arm, status in statuses.items()
                                                   if status == 'NONDETERMINISTIC'],
                            stable_arms=[arm for arm, status in statuses.items()
                                         if status == 'STABLE'],
                            blocked_arms=[arm for arm, status in statuses.items()
                                          if status == 'BLOCKED_TOPOLOGY'],
                            invalid_arms=[arm for arm, status in statuses.items()
                                          if status == 'INVALID'],
                            statistical_note='Calls within one process are dependent. '
                                             'Rates are descriptive; no significance claim '
                                             'and no proof of absence.'),
            outcome=dict(
                classification=outcome,
                release_gate='UNRESOLVED', release_blocked=True,
                next_action='Report observed counts and exact SHAs; do not merge, release, '
                            'change upstream or treat a stable arm as absence of the bug.'),
            generated_utc=datetime.now(timezone.utc).isoformat())
        args.output.write_text(json.dumps(document, indent=2, allow_nan=False) + '\n',
                               encoding='utf-8')
        print(json.dumps(dict(manifest_sha256=manifest_sha,
                              outcome=document['outcome'],
                              baseline=probe_summary,
                              arms=[dict(id=arm['id'], status=arm['status'],
                                         summary=arm.get('summary') and {
                                             key: arm['summary'][key] for key in
                                             ('processes', 'calls', 'distinct_pixel_hashes',
                                              'modal_pixel_sha256', 'divergent_calls_vs_modal',
                                              'per_call_divergent_rate', 'per_process_event_rate',
                                              'psnr_min_db', 'psnr_max_db')})
                                    for arm in arms_report])))
        return 0 if outcome == 'NO_EFFECT_DETECTED' else 1
    except subprocess.TimeoutExpired:
        print('TIMEOUT: release blocked', file=sys.stderr)
        return 2
    except (OSError, ValueError, KeyError, TypeError, subprocess.SubprocessError) as error:
        print('INVALID_TOPOLOGY_MEASUREMENT: release blocked: %s' % type(error).__name__,
              file=sys.stderr)
        return 2


if __name__ == '__main__':
    raise SystemExit(main())
