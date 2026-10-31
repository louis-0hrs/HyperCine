# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
"""Check retained Windows #321 artifacts; optionally replay 25 bounded frames.

Private paths and process diagnostics are never included in the report.
Matching artifacts/retained records do not authenticate historical execution.
"""
import argparse
import json
import ntpath
import os
from pathlib import Path
import re
import struct
import subprocess
import sys

from verify_windows_sdk_corpus import (
    INDEXES, WINDOWS, file_sha, parse_packet, require, safe_file, verify_index,
)


def bound_file(path, expected):
    require(path.is_file() and not path.is_symlink(), 'missing or linked artifact')
    digest = file_sha(path)
    require(digest == expected, 'artifact SHA-256 mismatch')
    return {'sha256': digest, 'bytes': path.stat().st_size}


def build_record(build, target, source=None, library=None):
    compilers = list((build / 'CMakeFiles').glob('*/CMakeCXXCompiler.cmake'))
    require(len(compilers) == 1, 'ambiguous compiler record')
    compiler = compilers[0].read_text(encoding='utf-8')
    version = re.search(r'set\(CMAKE_CXX_COMPILER_VERSION "([0-9.]+)"\)', compiler)
    require(version is not None and 'set(CMAKE_CXX_COMPILER_ID "MSVC")' in compiler,
            'unsupported compiler record')
    prefix = 'upstream/' if target == 'CFHDCodecStatic' else ''
    records = list(build.glob(prefix + target + '.dir/Release/*.tlog/CL.command.1.tlog'))
    require(len(records) == 1, 'ambiguous compile record')
    commands = records[0].read_text(encoding='utf-16')
    # Fixed tokens only: never emit command lines or paths from retained logs.
    candidates = ('/O2', '/Ob2', '/MD', '/MT', '/GL', '/Zi', '/std:c++17',
                  '/arch:AVX2', '/arch:AVX512')
    flags = [f for f in candidates if re.search(re.escape(f) + r'(?=\s|$)', commands)]
    link = records[0].with_name('link.command.1.tlog')
    if not link.is_file():
        link = records[0].with_name('Lib.command.1.tlog')
    require(link.is_file(), 'missing link/archive record')
    if source is not None:
        require(str(source.resolve()).replace('/', '\\').casefold() in
                commands.replace('/', '\\').casefold(), 'compile source record differs')
    if library is not None:
        linked = re.findall(r'"([^"]+)"', link.read_text(encoding='utf-16'))
        # Compare Windows record spellings lexically on every host: fold case,
        # separators and parent components on both sides. Resolving only the
        # expected path can rewrite aliases/junctions absent from retained logs.
        # Full-path equality remains required; artifact bytes are bound separately.
        require(ntpath.normcase(ntpath.normpath(str(library))) in
                {ntpath.normcase(ntpath.normpath(p)) for p in linked},
                'link input record differs')
    return {'evidence_class': 'retained_local_records_not_attestation',
            'compiler_id': 'MSVC', 'compiler_version': version.group(1),
            'selected_compile_flags': flags,
            'compiler_record_sha256': file_sha(compilers[0]),
            'compile_record_sha256': file_sha(records[0]),
            'link_or_archive_record_sha256': file_sha(link)}


def compare_packet(actual, original):
    a, b = parse_packet(actual), parse_packet(original)
    require(a['normalized_sha256'] == b['normalized_sha256'], 'replay packet divergence')
    return {'bytes': len(actual), 'raw_equal': actual == original,
            'normalized_sha256': a['normalized_sha256'], 'normalized_equal': True}


def replay(binary, root, sessions, destination, state):
    observations = []
    for session in sessions:
        profile = session['profile']
        frames = session['frames'] if state else session['frames'][:1]
        inputs = [safe_file(root, f['input']) for f in frames]
        for frame, path in zip(frames, inputs):
            bound_file(path, frame['input_sha256'])
        out = destination / profile
        result = subprocess.run([str(binary), str({'low': 1, 'medium': 2, 'high': 3,
                                 'filmscan1': 4}[profile]), str(out), *map(str, inputs)],
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=180)
        require(result.returncode == 0, 'capture replay failed')
        trace = (out / 'trace.tsv').read_text().splitlines()
        require(len(trace) == len(frames), 'replay trace count mismatch')
        for index, frame in enumerate(frames):
            packet = (out / ('frame_%d.cfhd' % index)).read_bytes()
            original = safe_file(root, frame['sdk_packet']).read_bytes()
            bound_file(safe_file(root, frame['sdk_packet']), frame['sdk_sha256'])
            row = dict(profile=profile, frame=index, **compare_packet(packet, original))
            fields = trace[index].split('\t')
            require(len(fields) == (6 if state else 3), 'invalid replay trace')
            require(fields[-1] == 'frame_%d.cfhd' % index and
                    int(fields[-3]) == index and int(fields[-2]) == len(packet),
                    'replay trace order/length mismatch')
            if state:
                values = list(map(int, fields[:3]))
                expected = [frame[k] for k in ('sdk_overbitrate_after_encode',
                            'sdk_rate_limiter_after_encode', 'sdk_lastgopbitcount_after_encode')]
                require(values == expected and values[2] == len(packet) * 8,
                        'replay SDK state differs')
                row['sdk_state'] = values
            observations.append(row)
    return observations


def run(golden, historical, library, replay_dir=None):
    roots = {g: golden / WINDOWS / g for g in INDEXES if g != 'linux-sdk-gaps'}
    indexes = {g: verify_index(root, *INDEXES[g]) for g, root in roots.items()}
    manifests = {g: json.loads((root / 'manifest.json').read_text()) for g, root in roots.items()}
    lib_hashes = {m['sdk_static_library_sha256'] for m in manifests.values()}
    require(len(lib_hashes) == 1, 'conflicting library bindings')
    result = {'scope': 'retained Windows artifacts and bounded replay; no conformance claim',
              'indexed_files_verified': sum(map(len, indexes.values())),
              'sdk_library': bound_file(library, lib_hashes.pop()), 'captures': {},
              'limitations': [
                  'Retained records are not a historical build or execution attestation.',
                  'Archive-to-pristine-source reproduction remains NOT_ESTABLISHED.',
                  'Historical generator seed and actual playback cadence remain NOT_MEASURED.',
                  '30 fps is the inspected SDK quantizer fallback, not measured playback.',
                  'Replay covers four first frames and three seven-frame state sequences only.',
                  'No independent pixel decoder, GPU or product qualification.']}
    specs = [('api', 'group-a-sequences', 'sdk_packet_capture', historical / 'build', historical),
             ('state', 'group-c-sdk-state', 'sdk_capture_state',
              historical / 'instrumented/build', historical / 'instrumented')]
    binaries = {}
    for label, group, target, build, source_root in specs:
        source = source_root / (target + '.cpp')
        corpus_source = roots[group] / 'provenance' / source.name
        binary = build / 'Release' / (target + '.exe')
        row = {'binary': bound_file(binary, manifests[group]['capture_binary_sha256']),
               'source': bound_file(source, file_sha(corpus_source)),
               'cmake_recipe': bound_file(source_root / 'CMakeLists.txt',
                                        file_sha(roots[group] / 'provenance/CMakeLists.txt')),
               'build_record': build_record(build, target, source, library)}
        if label == 'api':
            require(manifests['group-b-hysteresis']['capture_binary_sha256'] ==
                    row['binary']['sha256'], 'A/B capture binding mismatch')
        result['captures'][label] = row
        binaries[label] = binary
    result['sdk_build_record'] = build_record(library.parents[2], 'CFHDCodecStatic')
    result['replay'] = {'status': 'NOT_RUN'}
    if replay_dir is not None:
        require(golden.resolve() != replay_dir.resolve() and
                golden.resolve() not in replay_dir.resolve().parents, 'replay inside corpus')
        replay_dir.mkdir(parents=True, exist_ok=False)
        rows = {}
        for label, group in [('api', 'group-a-sequences'), ('state', 'group-c-sdk-state')]:
            rows[label] = replay(binaries[label], roots[group], manifests[group]['sessions'],
                                 replay_dir / label, label == 'state')
        result['replay'] = {'status': 'PASS', 'observations': rows,
                            'frames': sum(map(len, rows.values()))}
    return result


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--golden-root', default=os.environ.get('HYPERCINE_GOLDEN_DIR'))
    p.add_argument('--historical-root', required=True)
    p.add_argument('--sdk-library', required=True)
    p.add_argument('--replay-dir', help='new private scratch directory, outside corpus')
    p.add_argument('--output', required=True)
    a = p.parse_args()
    try:
        require(bool(a.golden_root), 'missing golden root')
        golden, output = Path(a.golden_root).resolve(), Path(a.output).resolve()
        require(output != golden and golden not in output.parents and not output.exists(),
                'unsafe or existing report destination')
        report = run(golden, Path(a.historical_root).resolve(), Path(a.sdk_library).resolve(),
                     Path(a.replay_dir).resolve() if a.replay_dir else None)
        with output.open('x', encoding='utf-8') as stream:
            json.dump(report, stream, indent=2, sort_keys=True)
            stream.write('\n')
        print('PASS retained artifact bindings; replay=' + report['replay']['status'])
        return 0
    except (OSError, ValueError, KeyError, TypeError, struct.error, subprocess.SubprocessError):
        print('FAIL provenance audit; no report written', file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
