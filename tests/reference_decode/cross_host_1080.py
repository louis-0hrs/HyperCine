#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
"""Pre-registered matched-SHA 1080p cross-host protocol (one host arm).

The packet and source identities are fixed before execution. This script only
measures the local host arm; another host must run the same protocol with its
own worker binary and report its own configuration.
"""
import argparse
import json
import platform
import re
import subprocess
import sys
from pathlib import Path

import contract
from bgra_ab import collect
from measure import sha

PACKET_SHA = '2b9a28dec5b015bf76a1ba3bce5a458167bd07bd061dbb59a97c00eece4fec52'
SOURCE_SHA = '45c166c3d6a3710d620ff5d738980224edf53cf9ce1bceb87267e474294afff1'
PROTOCOL_ID = 'matched-sha-1080-bgra-full-20261018'


def bounded(command, timeout):
    result = subprocess.run(list(map(str, command)), capture_output=True, timeout=timeout)
    if result.returncode:
        raise ValueError('diagnostic subprocess failed')
    return result.stdout


def cpu_model():
    script = ('(Get-CimInstance Win32_Processor | Select-Object -First 1 '
              'Name,NumberOfCores,NumberOfLogicalProcessors | ConvertTo-Json -Compress)')
    try:
        raw = subprocess.run(['powershell', '-NoProfile', '-Command', script],
                             capture_output=True, timeout=30)
        value = json.loads(raw.stdout.decode()) if raw.returncode == 0 else {}
        if isinstance(value, dict):
            return dict(model=value.get('Name', 'UNKNOWN'),
                        cores=value.get('NumberOfCores', 'UNKNOWN'),
                        logical_processors=value.get('NumberOfLogicalProcessors', 'UNKNOWN'))
    except (OSError, ValueError, subprocess.SubprocessError):
        pass
    return dict(model='UNKNOWN', cores='UNKNOWN', logical_processors='UNKNOWN')


def build_config(worker, build_dir):
    """Record source-backed build properties without publishing local paths."""
    text = ''
    for candidate in sorted(build_dir.glob('CMakeFiles/*/CMakeCXXCompiler.cmake')):
        text = candidate.read_text(encoding='utf-8')
        break

    def field(name, default='UNKNOWN'):
        found = re.search(r'set\(' + name + r' "([^"]*)"\)', text)
        return found.group(1) if found else default

    cache = build_dir/'CMakeCache.txt'
    flags = 'UNKNOWN'
    if cache.is_file():
        found = re.search(r'^CMAKE_CXX_FLAGS_RELEASE:STRING=(.*)$',
                          cache.read_text(encoding='utf-8'), re.MULTILINE)
        flags = found.group(1) if found else flags
    image = worker.read_bytes()
    imports = [name.decode() for name in
               (b'MSVCP140.dll', b'VCRUNTIME140.dll', b'VCRUNTIME140_1.dll',
                b'api-ms-win-crt-runtime-l1-1-0.dll')
               if name in image]
    return dict(compiler_id=field('CMAKE_CXX_COMPILER_ID'),
                compiler_version=field('CMAKE_CXX_COMPILER_VERSION'),
                release_flags=flags,
                crt_link_mode='dynamic (/MD)' if imports else 'UNKNOWN',
                crt_imports=imports,
                cuda_linked=b'cudart' in image)


def threading_defaults(root):
    """Effective upstream defaults; no local override is applied."""
    config = (root/'upstream/Codec/config.h').read_text(encoding='utf-8')
    codec = (root/'upstream/Codec/codec.h').read_text(encoding='utf-8')

    def value(text, name):
        found = re.search(r'#define\s+' + name + r'\s+\(?([0-9_]+)', text)
        return found.group(1) if found else 'UNKNOWN'

    return dict(threaded_decoder=value(config, '_THREADED_DECODER'),
                entropy_threads='(_THREADED_DECODER)',
                interlaced_worker_threads='(_THREADED_DECODER) on Windows',
                threads_in_last_wavelet=value(codec, 'THREADS_IN_LAST_WAVELET'))


def summary(document):
    calls = document['calls']
    hashes = [c.get('pixel_sha256') for c in calls]
    psnr = [c['psnr_db'] for c in calls if c.get('psnr_db') is not None]
    timings = sorted(c['decode_ns']/1e6 for c in calls)
    modal = max(set(hashes), key=hashes.count) if hashes else None
    return dict(calls=len(calls), processes=len({c['process'] for c in calls}),
                distinct_pixel_hashes=len(set(hashes)),
                distinct_rgb_hashes=len({c.get('rgb_sha256') for c in calls}),
                divergent_calls=sum(h != modal for h in hashes),
                modal_pixel_sha256=modal,
                psnr_min_db=min(psnr) if psnr else None,
                psnr_max_db=max(psnr) if psnr else None,
                decode_median_ms=timings[len(timings)//2],
                decode_max_ms=timings[-1],
                decoder_statuses=sorted({c['decoder_status'] for c in calls}))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--worker', type=Path, required=True)
    parser.add_argument('--packet', type=Path, required=True)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--work', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--processes', type=int, default=3, choices=range(2, 7))
    parser.add_argument('--timeout', type=int, default=20, choices=range(5, 61))
    args = parser.parse_args()
    timeout = args.timeout
    try:
        root = Path(__file__).resolve().parents[2]
        packet = args.packet.read_bytes()
        source = args.source.read_bytes()
        if sha(packet) != PACKET_SHA or sha(source) != SOURCE_SHA:
            raise ValueError('pre-registered packet/source identity mismatch')
        if args.work.exists():
            raise ValueError('work directory must be fresh')
        args.work.mkdir(parents=True)
        runner = lambda command: bounded(command, timeout)
        document = collect(args.worker.resolve(), args.packet.resolve(), source,
                           '1080', True, args.processes, args.work, runner=runner)
        document['protocol'] = dict(
            id=PROTOCOL_ID, preregistered=True,
            script_sha256=sha(Path(__file__).read_bytes()),
            packet_sha256=PACKET_SHA, source_sha256=SOURCE_SHA,
            geometry=[1920, 1080], requested_format='BGRA',
            decoded_resolution='CFHD_DECODED_RESOLUTION_FULL',
            decoding_flags='CFHD_DECODING_FLAGS_NONE',
            active_metadata=True, processing_flags=65535,
            processes=args.processes, opens_per_process=5,
            child_timeout_seconds=timeout,
            criteria='timeout/decode error/invalid output => INVALID; any pixel hash '
                     'difference => REFERENCE_DECODER_NONDETERMINISTIC; stable but '
                     'unpaired encoder packet => still UNRESOLVED')
        document['host'] = dict(system=platform.system(), machine=platform.machine(),
                                python=platform.python_version(), **cpu_model())
        document['build'] = dict(worker_sha256=sha(args.worker.read_bytes()),
                                 source_commit=bounded(['git', 'rev-parse', 'HEAD'],
                                                       timeout).decode().strip(),
                                 threading=threading_defaults(root),
                                 **build_config(args.worker, args.worker.resolve().parents[2]))
        document['arm_summary'] = summary(document)
        document['result'] = contract.report(document)
        document['cross_host_state'] = (
            'WINDOWS_ARM_NONDETERMINISTIC' if document['result']['classification']
            == 'REFERENCE_DECODER_NONDETERMINISTIC' else
            'WINDOWS_ARM_STABLE_UNRESOLVED' if document['result']['classification']
            == 'REFERENCE_EVIDENCE_INCOMPLETE' else 'WINDOWS_ARM_BLOCKED')
        args.output.write_text(json.dumps(document, indent=2, allow_nan=False)+'\n',
                               encoding='utf-8')
        print(json.dumps(dict(result=document['result'],
                              cross_host_state=document['cross_host_state'],
                              summary=document['arm_summary'])))
        # Stable output does not clear the cross-host or encoder gates.
        return 0 if document['result']['classification'] == 'PASS' else 1
    except subprocess.TimeoutExpired:
        print('TIMEOUT: release blocked', file=sys.stderr)
        return 2
    except (OSError, ValueError, KeyError, TypeError, subprocess.SubprocessError):
        print('INVALID_CROSS_HOST_MEASUREMENT: release blocked', file=sys.stderr)
        return 2


if __name__ == '__main__':
    raise SystemExit(main())
