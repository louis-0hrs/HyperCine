#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
"""Derivative affinity/topology arm for the frozen 1080p BGRA decoder protocol.

The worker applies and verifies its process affinity before creating any SDK
object. This script only measures bounded, independent fresh processes; it does
not modify #138 or certify any decoder or release gate.
"""
import argparse
import ctypes
import json
import platform
import subprocess
import sys
from pathlib import Path

import contract
from bgra_ab import quality
from cross_host_1080 import build_config, threading_defaults
from measure import metrics, sha

PROTOCOL_ID = 'epyc-vm-affinity-1080-bgra-20261018-v1'
PACKET_SHA = '2b9a28dec5b015bf76a1ba3bce5a458167bd07bd061dbb59a97c00eece4fec52'
SOURCE_SHA = '45c166c3d6a3710d620ff5d738980224edf53cf9ce1bceb87267e474294afff1'
WIDTH, HEIGHT = 1920, 1080
OPENS_PER_PROCESS = 5
ARM_ORDER = ('A', 'B', 'C', 'D')
# Pre-registered selection rule: an arm requests the lowest-numbered N active
# logical processors of the guest system mask. None means unrestricted.
ARM_LOGICAL_PROCESSORS = {'A': None, 'B': 4, 'C': 2, 'D': 1}
PRIMARY_PROCESSES = 3
EXTENSION_PROCESSES = 6


def first_difference(reference, actual):
    difference = next((i for i, (a, b) in enumerate(zip(reference, actual)) if a != b), None)
    if difference is None:
        return None
    return dict(byte_offset=difference, x=difference // 4 % WIDTH,
                y=difference // (WIDTH * 4), channel='BGRA'[difference % 4],
                reference=reference[difference], actual=actual[difference])


def arm_masks(system_mask):
    bits = [bit for bit in range(64) if system_mask >> bit & 1]
    masks = {}
    for arm, count in ARM_LOGICAL_PROCESSORS.items():
        if count is None:
            masks[arm] = None
            continue
        if len(bits) < count:
            raise ValueError('guest logical processor count below arm size')
        masks[arm] = sum(1 << bit for bit in bits[:count])
    return masks


def _cim_host():
    """Read-only CIM metadata; no host name, user, serial or domain is read."""
    script = ('$cpu = Get-CimInstance Win32_Processor | Select-Object -First 1 '
              'Name,NumberOfCores,NumberOfLogicalProcessors,MaxClockSpeed; '
              '$cs = Get-CimInstance Win32_ComputerSystem | Select-Object '
              'Manufacturer,Model,HypervisorPresent,TotalPhysicalMemory; '
              '$os = Get-CimInstance Win32_OperatingSystem | Select-Object '
              'Caption,Version,BuildNumber; '
              '[pscustomobject]@{cpu=$cpu;computer_system=$cs;operating_system=$os} '
              '| ConvertTo-Json -Compress -Depth 4')
    try:
        result = subprocess.run(['powershell', '-NoProfile', '-Command', script],
                                capture_output=True, timeout=30)
        if result.returncode == 0:
            value = json.loads(result.stdout.decode())
            if isinstance(value, dict):
                return value
    except (OSError, ValueError, subprocess.SubprocessError):
        pass
    return {}


class _SystemInfo(ctypes.Structure):
    _fields_ = [('wProcessorArchitecture', ctypes.c_ushort),
                ('wReserved', ctypes.c_ushort),
                ('dwPageSize', ctypes.c_uint32),
                ('lpMinimumApplicationAddress', ctypes.c_void_p),
                ('lpMaximumApplicationAddress', ctypes.c_void_p),
                ('dwActiveProcessorMask', ctypes.c_size_t),
                ('dwNumberOfProcessors', ctypes.c_uint32),
                ('dwProcessorType', ctypes.c_uint32),
                ('dwAllocationGranularity', ctypes.c_uint32),
                ('wProcessorLevel', ctypes.c_ushort),
                ('wProcessorRevision', ctypes.c_ushort)]


class _LogicalProcessor(ctypes.Structure):
    # SYSTEM_LOGICAL_PROCESSOR_INFORMATION on 64-bit Windows.
    _fields_ = [('ProcessorMask', ctypes.c_size_t),
                ('Relationship', ctypes.c_int),
                ('_padding', ctypes.c_int),
                ('Reserved', ctypes.c_ulonglong * 2)]


def _windows_topology():
    """Read-only guest topology via documented Windows APIs."""
    if sys.platform != 'win32':
        raise ValueError('topology discovery requires Windows')
    kernel32 = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel32.GetCurrentProcess.restype = ctypes.c_void_p
    kernel32.GetSystemInfo.argtypes = [ctypes.POINTER(_SystemInfo)]
    kernel32.GetProcessAffinityMask.argtypes = [
        ctypes.c_void_p, ctypes.POINTER(ctypes.c_size_t), ctypes.POINTER(ctypes.c_size_t)]
    kernel32.GetProcessAffinityMask.restype = ctypes.c_int
    kernel32.GetLogicalProcessorInformation.argtypes = [
        ctypes.c_void_p, ctypes.POINTER(ctypes.c_uint32)]
    kernel32.GetLogicalProcessorInformation.restype = ctypes.c_int
    kernel32.GetNumaHighestNodeNumber.argtypes = [ctypes.POINTER(ctypes.c_uint32)]
    kernel32.GetNumaHighestNodeNumber.restype = ctypes.c_int
    kernel32.GetNumaNodeProcessorMask.argtypes = [ctypes.c_uint32,
                                                  ctypes.POINTER(ctypes.c_size_t)]
    kernel32.GetNumaNodeProcessorMask.restype = ctypes.c_int
    kernel32.GetActiveProcessorGroupCount.restype = ctypes.c_ushort
    kernel32.GetActiveProcessorCount.argtypes = [ctypes.c_ushort]
    kernel32.GetActiveProcessorCount.restype = ctypes.c_uint32
    kernel32.GetPriorityClass.argtypes = [ctypes.c_void_p]
    kernel32.GetPriorityClass.restype = ctypes.c_uint32
    info = _SystemInfo()
    kernel32.GetSystemInfo(ctypes.byref(info))
    process_mask, system_mask = ctypes.c_size_t(), ctypes.c_size_t()
    if not kernel32.GetProcessAffinityMask(kernel32.GetCurrentProcess(),
                                           ctypes.byref(process_mask),
                                           ctypes.byref(system_mask)):
        raise ValueError('launcher affinity query failed')
    if process_mask.value != system_mask.value:
        raise ValueError('launcher already restricted; unrestricted arm invalid')
    length = ctypes.c_uint32(0)
    kernel32.GetLogicalProcessorInformation(None, ctypes.byref(length))
    if ctypes.get_last_error() != 122 or not length.value:
        raise ValueError('logical processor information unavailable')
    entries = length.value // ctypes.sizeof(_LogicalProcessor)
    buffer = (_LogicalProcessor * entries)()
    if not kernel32.GetLogicalProcessorInformation(buffer, ctypes.byref(length)):
        raise ValueError('logical processor information query failed')
    cores = [dict(mask=entry.ProcessorMask,
                  siblings=bin(entry.ProcessorMask).count('1'))
             for entry in buffer if entry.Relationship == 0]
    smt = [core for core in cores if core['siblings'] > 1]
    highest = ctypes.c_uint32(0)
    numa = []
    if kernel32.GetNumaHighestNodeNumber(ctypes.byref(highest)):
        for node in range(highest.value + 1):
            mask = ctypes.c_size_t(0)
            if kernel32.GetNumaNodeProcessorMask(node, ctypes.byref(mask)):
                numa.append(dict(node=node, mask=mask.value))
    groups = kernel32.GetActiveProcessorGroupCount()
    active = kernel32.GetActiveProcessorCount(0xffff)
    return dict(processor_architecture=info.wProcessorArchitecture,
                active_logical_processors=info.dwNumberOfProcessors,
                active_processor_count=active,
                processor_groups=groups,
                allocation_granularity=info.dwAllocationGranularity,
                page_size=info.dwPageSize,
                system_affinity_mask=system_mask.value,
                launcher_affinity_mask=process_mask.value,
                priority_class=kernel32.GetPriorityClass(kernel32.GetCurrentProcess()),
                physical_cores=len(cores),
                core_masks=[core['mask'] for core in cores],
                smt_core_masks=[core['mask'] for core in smt],
                numa_nodes=numa,
                unknowns=[] if cores else ['core/sibling map unavailable'])


def host_document():
    try:
        topology = _windows_topology()
    except (OSError, ValueError) as error:
        topology = dict(unknown_reason=str(error), system_affinity_mask=None)
    cim = _cim_host()
    unknown = []
    if not topology.get('physical_cores'):
        unknown.append('physical-core map')
    if not topology.get('numa_nodes'):
        unknown.append('NUMA processor masks')
    if not cim:
        unknown.append('CIM operating-system metadata')
    return dict(system=platform.system(), machine=platform.machine(),
                python=platform.python_version(),
                cpu=(cim.get('cpu') or {}), computer_system=(cim.get('computer_system') or {}),
                operating_system=(cim.get('operating_system') or {}),
                topology=topology, unknowns=unknown)


def run_arm(worker, packet, source, packet_sha, system_mask, arm, mask, order, work, timeout,
            quality_cache):
    calls = []
    arm_first = arm_first_sha = None
    for process, phase in order:
        prefix = work / (arm + '-' + str(process))
        command = [str(worker), 'decode-bgra-on', '1080', str(packet), str(prefix)]
        if mask is not None:
            command.append(str(mask))
        result = subprocess.run(command, capture_output=True, timeout=timeout)
        if result.returncode:
            raise ValueError('diagnostic subprocess failed')
        rows = [contract.strict_document(line.decode()) for line in result.stdout.splitlines()]
        if len(rows) != OPENS_PER_PROCESS or [r['ordinal'] for r in rows] != list(range(1, 6)):
            raise ValueError('incomplete worker')
        first_of_process = first_of_process_sha = None
        for row in rows:
            if (row.get('affinity_requested') is not (mask is not None)
                    or row.get('affinity_requested_mask') != (mask or 0)
                    or row.get('affinity_system_mask') != system_mask
                    or row.get('affinity_effective_mask') != (mask if mask is not None
                                                             else row.get('affinity_system_mask'))):
                raise ValueError('affinity verification drift')
            if (row.get('decoder_status') != 0 or row.get('output_valid') is not True
                    or row.get('width') != WIDTH or row.get('height') != HEIGHT
                    or row.get('pitch') != WIDTH * 4 or row.get('buffer_size') != WIDTH * HEIGHT * 4
                    or row.get('requested_format') != 'BGRA' or row.get('active_metadata') is not True
                    or row.get('processing_flags') != 65535
                    or any(row.get(name) != 0 for name in
                           ('metadata_open_status', 'metadata_init_status', 'metadata_set_status'))):
                raise ValueError('invalid decode observation')
            path = Path(str(prefix) + '-' + str(row['ordinal']) + '.bgra')
            pixels = path.read_bytes()
            if len(pixels) != WIDTH * HEIGHT * 4:
                raise ValueError('invalid output buffer')
            pixel_sha = sha(pixels)
            cached = quality_cache.get(pixel_sha)
            if cached is None:
                cached = (quality(source, pixels, WIDTH, HEIGHT), sha(
                    pixels[0::4] + pixels[1::4] + pixels[2::4]))
                quality_cache[pixel_sha] = cached
            source_quality, rgb_sha = cached
            row.update(process=process, phase=phase, sample_sha256=packet_sha,
                       pixel_sha256=pixel_sha, rgb_sha256=rgb_sha,
                       psnr_db=source_quality['psnr_db'],
                       source_pixels_exact=source_quality['source_pixels_exact'],
                       versus_source=source_quality)
            if first_of_process is None:
                first_of_process, first_of_process_sha = pixels, pixel_sha
            if arm_first is None:
                arm_first, arm_first_sha = pixels, pixel_sha
            identical = dict(changed_bytes=0, max_abs_delta=0, psnr_db=None,
                             source_pixels_exact=True)
            if pixel_sha == first_of_process_sha:
                row['versus_process_first'] = dict(identical, first_difference=None)
            else:
                row['versus_process_first'] = metrics(first_of_process, pixels)
                row['versus_process_first']['first_difference'] = first_difference(
                    first_of_process, pixels)
            if pixel_sha == arm_first_sha:
                row['versus_arm_first'] = dict(identical, first_difference=None)
            else:
                row['versus_arm_first'] = metrics(arm_first, pixels)
                row['versus_arm_first']['first_difference'] = first_difference(arm_first, pixels)
            keep = (pixels is first_of_process or pixels is arm_first
                    or pixel_sha != first_of_process_sha)
            if not keep:
                path.unlink()
            calls.append(row)
    return calls


def arm_summary(calls):
    hashes = [call['pixel_sha256'] for call in calls]
    modal = max(set(hashes), key=hashes.count)
    psnr = [call['versus_source']['psnr_db'] for call in calls
            if call['versus_source']['psnr_db'] is not None]
    timings = sorted(call['decode_ns'] / 1e6 for call in calls)
    process_hashes = {}
    for call in calls:
        process_hashes.setdefault(call['process'], []).append(call['pixel_sha256'])
    processes = len(process_hashes)
    divergent_processes = sum(len(set(values)) > 1 for values in process_hashes.values())
    phases = {phase: [call for call in calls if call['phase'] == phase]
              for phase in ('primary', 'extension')}
    return dict(calls=len(calls), processes=processes,
                primary_calls=len(phases['primary']), extension_calls=len(phases['extension']),
                distinct_pixel_hashes=len(set(hashes)),
                distinct_rgb_hashes=len({call['rgb_sha256'] for call in calls}),
                divergent_calls=sum(digest != modal for digest in hashes),
                divergent_calls_vs_process_first=sum(
                    call['versus_process_first']['changed_bytes'] > 0 for call in calls),
                divergent_calls_vs_arm_first=sum(
                    call['versus_arm_first']['changed_bytes'] > 0 for call in calls),
                divergent_processes=divergent_processes,
                stable_processes=processes - divergent_processes,
                modal_pixel_sha256=modal,
                process_pixel_hashes={str(process): sorted(set(values))
                                      for process, values in sorted(process_hashes.items())},
                psnr_min_db=min(psnr) if psnr else None,
                psnr_max_db=max(psnr) if psnr else None,
                decode_median_ms=timings[len(timings) // 2], decode_max_ms=timings[-1],
                decoder_statuses=sorted({call['decoder_status'] for call in calls}),
                timings_are_vm_observations_not_benchmark=True,
                classification=contract.classify(None, calls))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--worker', type=Path, required=True)
    parser.add_argument('--packet', type=Path, required=True)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--work', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--arms', default=','.join(ARM_ORDER))
    parser.add_argument('--processes', type=int, default=PRIMARY_PROCESSES + EXTENSION_PROCESSES)
    parser.add_argument('--reverse-order', action='store_true')
    parser.add_argument('--timeout', type=int, default=20)
    args = parser.parse_args()
    try:
        arms = [arm.strip() for arm in args.arms.split(',') if arm.strip()]
        if not arms or any(arm not in ARM_ORDER for arm in arms):
            raise ValueError('invalid arm selection')
        if args.processes < PRIMARY_PROCESSES + 1 or args.processes > 9:
            raise ValueError('process count outside prespecified bound')
        if args.work.exists():
            raise ValueError('work directory must be fresh')
        packet, source = args.packet.read_bytes(), args.source.read_bytes()
        if sha(packet) != PACKET_SHA or sha(source) != SOURCE_SHA:
            raise ValueError('pre-registered packet/source identity mismatch')
        host = host_document()
        system_mask = host['topology'].get('system_affinity_mask')
        if system_mask is None:
            raise ValueError('guest affinity mask unavailable')
        masks = arm_masks(system_mask)
        args.work.mkdir(parents=True)
        worker = args.worker.resolve()
        root = Path(__file__).resolve().parents[2]
        build = build_config(worker, worker.parents[2])
        if build.get('cuda_linked'):
            raise ValueError('worker links CUDA; CPU-only control required')
        order = [(process, 'primary' if process <= PRIMARY_PROCESSES else 'extension')
                 for process in range(1, args.processes + 1)]
        if args.reverse_order:
            order.reverse()
        document = dict(schema_version=1, release_blocked=True,
                        protocol=dict(
                            id=PROTOCOL_ID, derivative_of='matched-sha-1080-bgra-full-20261018',
                            preregistered=True, script_sha256=sha(Path(__file__).read_bytes()),
                            worker_source_sha256=sha(Path(__file__).with_name('worker.cpp').read_bytes()),
                            worker_sha256=sha(worker.read_bytes()),
                            source_commit=subprocess.run(
                                ['git', 'rev-parse', 'HEAD'], capture_output=True,
                                timeout=args.timeout, check=True).stdout.decode().strip(),
                            packet_sha256=PACKET_SHA, source_sha256=SOURCE_SHA,
                            geometry=[WIDTH, HEIGHT], requested_format='BGRA',
                            decoded_resolution='CFHD_DECODED_RESOLUTION_FULL',
                            decoding_flags='CFHD_DECODING_FLAGS_NONE',
                            active_metadata=True, processing_flags=65535,
                            opens_per_process=OPENS_PER_PROCESS,
                            processes_per_arm=args.processes,
                            primary_processes=PRIMARY_PROCESSES,
                            extension_processes=args.processes - PRIMARY_PROCESSES,
                            child_timeout_seconds=args.timeout,
                            arm_order=[arm for arm in (ARM_ORDER if not args.reverse_order
                                                       else tuple(reversed(ARM_ORDER)))
                                       if arm in arms],
                            arm_rule='A unrestricted; B/C/D request the lowest-numbered '
                                     '4/2/1 active guest logical processors',
                            requested_masks={arm: masks[arm] for arm in arms},
                            criteria='affinity set/verification failure, timeout, decode error, '
                                     'invalid geometry or metadata => INVALID; any pixel hash '
                                     'difference => REFERENCE_DECODER_NONDETERMINISTIC; stable '
                                     'output without encoder pair remains UNRESOLVED',
                            confirmatory_trigger='run fresh 9-process arms in reversed order when '
                                                 'arm A and any reduced arm differ in stability '
                                                 'direction or by at least 5/45 divergent calls'),
                        host=host, build=dict(threading=threading_defaults(root), **build),
                        arms={})
        quality_cache = {}
        for arm in arms:
            calls = run_arm(worker, args.packet.resolve(), source, PACKET_SHA, system_mask,
                            arm, masks[arm], order, args.work, args.timeout, quality_cache)
            document['arms'][arm] = dict(requested_mask=masks[arm], calls=calls,
                                         summary=arm_summary(calls))
        args.output.write_text(json.dumps(document, indent=2, allow_nan=False) + '\n',
                               encoding='utf-8')
        print(json.dumps({arm: document['arms'][arm]['summary'] for arm in arms}))
        return 0
    except subprocess.TimeoutExpired:
        print('TIMEOUT: release blocked', file=sys.stderr)
        return 2
    except (OSError, ValueError, KeyError, TypeError, subprocess.SubprocessError):
        print('INVALID_AFFINITY_MEASUREMENT: release blocked', file=sys.stderr)
        return 2


if __name__ == '__main__':
    raise SystemExit(main())
