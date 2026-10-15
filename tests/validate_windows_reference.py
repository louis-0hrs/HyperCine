#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
"""SDK-free, fail-closed input gate and experimental CPU stage diagnostics.

Exit 0: input-only gate passed. Exit 1: invalid input/provenance/artifact.
Exit 2: diagnostic completed, SDK-compatible encoder parity remains blocked.
No Windows sample bytes are inferred from hashes. Only parsed whitelist metadata
values are normalized in copies after original file hash verification.
"""
import argparse
import hashlib
import json
from pathlib import Path
import platform
import struct
import subprocess
import tempfile
import sys
import reference_sample as sample

ROOT = Path(__file__).resolve().parents[1]
CORPUS = ROOT / 'tests/reference_corpus'
PATTERNS = ['FlatBlack', 'FlatMidGray', 'FlatWhite', 'HorizontalGradient',
            'VerticalGradient', 'Checkerboard', 'HardEdges', 'DeterministicNoise',
            'HighFrequencyPattern', 'LowDetailPhotographicSynthetic',
            'HighDetailPhotographicSynthetic']


def sha(data):
    return hashlib.sha256(data).hexdigest()


def verify_bytes(data, digest, length, label):
    if len(data) != length or sha(data) != digest:
        raise ValueError('SHA/length mismatch: ' + label)


def load_manifest(path):
    provenance = json.loads((CORPUS / 'import_provenance.json').read_text())
    generator = provenance['portable_generator']
    if sha((ROOT / generator['path']).read_bytes()) != generator['sha256']:
        raise ValueError('portable generator provenance mismatch')
    relative = path.resolve().relative_to(ROOT).as_posix()
    expected = next(f['sha256'] for f in provenance['files'] if f['path'] == relative)
    data = path.read_bytes()
    if sha(data) != expected:
        raise ValueError('imported manifest changed: ' + relative)
    manifest = json.loads(data)
    if manifest['schema_version'] != 1:
        raise ValueError('unsupported corpus schema')
    return manifest


def validate_frame(frame):
    w, h = frame['width'], frame['height']
    if (w, h, frame['pitch']) != (1920, 1080, 7680):
        raise ValueError('only the validated 1080p oracle is supported')
    if frame['input_byte_length'] != w*h*4:
        raise ValueError('input storage mismatch')
    if len(frame['quant']) != 36 or len(frame['scale']) != 36 or len(frame['bands']) != 30:
        raise ValueError('invalid quant/scale/band count')
    for i, b in enumerate(frame['bands']):
        channel, sub = divmod(i, 10)
        wavelet = 2 if sub <= 3 else (1 if sub <= 6 else 0)
        band = 0 if sub == 0 else ((sub-1) % 3)+1
        if (b['channel'], b['subband'], b['wavelet'], b['band']) != (channel, sub, wavelet, band):
            raise ValueError('invalid band ordering')
        qindex = channel*12 + wavelet*4 + band
        if (b['quant'], b['scale']) != (frame['quant'][qindex], frame['scale'][qindex]):
            raise ValueError('band table mismatch')
        if not 1 <= b['quant'] <= 32768:
            raise ValueError('invalid divisor')
        if (b['width'], b['height']) != ((w if channel == 0 else w//2)//(2**(wavelet+1)), h//(2**(wavelet+1))):
            raise ValueError('band geometry mismatch')
        if b['sdk_coeff_fnv64'] != b['cuda_coeff_fnv64'] or b['sdk_payload_sha256'] != b['cuda_payload_sha256']:
            raise ValueError('SDK/CUDA oracle disagrees')
    if not frame['normalized_equal'] or frame['sdk_normalized_sha256'] != frame['cuda_normalized_sha256']:
        raise ValueError('SDK/CUDA normalized oracle disagrees')


def checked_input(probe, frame, path):
    validate_frame(frame)
    subprocess.run([str(probe), 'generate', str(frame['width']), str(frame['height']),
                    str(frame['frame_index']), str(PATTERNS.index(frame['pattern'])), str(path)], check=True)
    verify_bytes(path.read_bytes(), frame['input_sha256'], frame['input_byte_length'], 'generated BGRA')


def verify_artifacts(manifest, frame, root):
    """Verify original and separately normalized exports without modifying either."""
    suffix = '/frame_%04d/' % frame['frame_index']
    artifacts = [a for a in manifest['artifacts'] if suffix in a['path'] and
                 ('tier-a-1080/' in a['path'] or 'sequence-1920x1080/' in a['path'])]
    if not {'sdk.cfhd', 'cuda.cfhd'}.issubset({Path(a['path']).name for a in artifacts}):
        raise ValueError('manifest has no matching binary artifacts')
    missing = []
    for artifact in artifacts:
        path = (root / artifact['path']).resolve()
        if root.resolve() not in path.parents:
            raise ValueError('artifact escapes corpus root')
        if not path.is_file():
            missing.append(artifact['path'])
            continue
        verify_bytes(path.read_bytes(), artifact['sha256'], artifact['byte_length'], artifact['path'])
    return {'status': 'BLOCKED' if missing else 'PASS', 'missing': missing,
            'meaning': 'file SHA verification only; no Mac encoder parity claim'}


def first_i16_difference(expected, actual, width):
    """Exact signed diagnostic for future provenance-checked intermediate dumps."""
    if len(expected) % 2 or len(actual) % 2 or width < 1:
        raise ValueError('invalid signed plane storage')
    for offset in range(0, min(len(expected), len(actual)), 2):
        a, b = struct.unpack_from('<h', expected, offset)[0], struct.unpack_from('<h', actual, offset)[0]
        if a != b:
            index = offset//2
            return {'x': index % width, 'y': index//width, 'expected': a, 'actual': b}
    return None if len(expected) == len(actual) else {'byte_length': [len(expected), len(actual)]}


def diagnose(probe, frame, input_path, directory, reference=None, sdk_replay=False):
    # Recheck before entering the codec, even if caller already generated it.
    verify_bytes(input_path.read_bytes(), frame['input_sha256'], frame['input_byte_length'], 'pre-codec BGRA')
    directory.mkdir(parents=True, exist_ok=True)
    plan = directory / 'quant-plan.txt'
    plan.write_text(' '.join(str(b['quant']) for b in frame['bands']))
    subprocess.run([str(probe), 'diagnose-sdk-replay' if sdk_replay else 'diagnose', str(frame['width']), str(frame['height']),
                    str(input_path), str(directory), str(plan)], check=True)
    bands = []
    for b in frame['bands']:
        prefix = directory / ('c%d-s%d' % (b['channel'], b['subband']))
        coeff = prefix.with_suffix('.fnv64').read_text()
        entropy = prefix.with_suffix('.payload').read_bytes()
        payload = sha(sample.reframe_band(reference['bands'][len(bands)], entropy)) if reference else None
        raw = reference['bands'][len(bands)] if reference else None
        difference = None
        if raw and raw['lowpass']:
            p = raw['payload']
            expected = b''.join(p[i:i+2][::-1] for i in range(0, len(p), 2))
            difference = first_i16_difference(expected, prefix.with_suffix('.i16le').read_bytes(), b['width'])
        byte_diff = None
        if raw:
            common = min(len(raw['payload']), len(entropy))
            byte_diff = next((i for i in range(common) if raw['payload'][i] != entropy[i]), None)
            if byte_diff is None and len(raw['payload']) != len(entropy):
                byte_diff = common
        bands.append({'channel': b['channel'], 'subband': b['subband'],
                      'quant': b['quant'], 'scale_reference_only': b['scale'],
                      'coefficient_fnv64': coeff, 'payload_sha256': payload,
                      'payload_scope': 'reference-header diagnostic reframe' if raw else 'PENDING: actual reference headers required',
                      'entropy_sha256': sha(entropy),
                      'first_signed_difference': difference,
                      'first_entropy_byte_difference': byte_diff,
                      'reference_sample_byte_offset': raw['offset']+byte_diff if raw and byte_diff is not None else None,
                      'expected_coeff_fnv64': b['sdk_coeff_fnv64'],
                      'expected_payload_sha256': b['sdk_payload_sha256'],
                      'coefficient_hash_equal': coeff == b['sdk_coeff_fnv64'],
                      'payload_equal': payload == b['sdk_payload_sha256'] if payload is not None else None})
    first = next((b for b in bands if not b['coefficient_hash_equal'] or b['payload_equal'] is False), None)
    return {'status': 'FAIL' if first else 'PASS',
            'scope': 'frozen-table SDK arithmetic replay; reference-header diagnostic wrapping; no stateful encoder' if sdk_replay else 'current Q2 frontend/wavelet with exported divisors; diagnostic only',
            'coeff_matches': sum(b['coefficient_hash_equal'] for b in bands),
            'payload_matches': sum(b['payload_equal'] is True for b in bands) if reference else None, 'bands': bands,
            'first_observed_difference': first,
            'earliest_causal_stage': 'PENDING: Windows converted/filter/coeff dumps unavailable',
            'coordinate': 'exact signed LL comparisons available' if reference else 'PENDING: reference binary required'}



def inspect_inventory(manifest, root):
    verified, missing = 0, []
    for artifact in manifest['artifacts']:
        path = (root / artifact['path']).resolve()
        if root.resolve() not in path.parents:
            raise ValueError('artifact escapes corpus root')
        if not path.is_file():
            missing.append(artifact['path'])
            continue
        verify_bytes(path.read_bytes(), artifact['sha256'], artifact['byte_length'], artifact['path'])
        verified += 1
    result = {'status': 'BLOCKED' if missing else 'PASS', 'files_verified': verified,
              'files_expected': len(manifest['artifacts']), 'files_missing': len(missing),
              'missing_examples': missing[:3], 'parsed_frames': []}
    if missing:
        return result
    for frame in manifest['frames']:
        if (frame['width'], frame['height']) != (1920, 1080):
            continue  # File integrity for 128px is not an encoder oracle.
        directory = 'tier-a-1080' if any('tier-a-1080/' in a['path'] for a in manifest['artifacts']) else 'sequence-1920x1080'
        base = root / 'frames' / directory / ('frame_%04d' % frame['frame_index'])
        sdk, sdk_normalized = sample.verify_sample((base/'sdk.cfhd').read_bytes(), frame, 'sdk')
        cuda, cuda_normalized = sample.verify_sample((base/'cuda.cfhd').read_bytes(), frame, 'cuda')
        if sdk_normalized != cuda_normalized:
            raise ValueError('actual SDK/CUDA normalized bytes differ')
        if any(a['framed'] != b['framed'] for a, b in zip(sdk['bands'], cuda['bands'])):
            raise ValueError('actual SDK/CUDA framed band bytes differ')
        result['parsed_frames'].append({'frame_index': frame['frame_index'],
                                       'framed_band_pairs_equal': 30,
                                       'normalized_sample_bytes_equal': True,
                                       'll_coefficient_hashes_checked_per_encoder': 3})
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--probe', type=Path, required=True)
    parser.add_argument('--inputs-only', action='store_true')
    parser.add_argument('--sdk-replay', action='store_true', help='replay documented CG709/default midpoint arithmetic with frozen exported divisors')
    parser.add_argument('--output', type=Path)
    parser.add_argument('--corpus-root', type=Path)
    parser.add_argument('--stage-dir', type=Path, help='retain intermediate dumps outside Git')
    parser.add_argument('--frames', type=int, nargs='+', default=[0, 3, 6, 13])
    args = parser.parse_args()
    report = {'copyright': 'Copyright (c) 2026 HyperCine Contributors.',
              'license': 'MIT OR Apache-2.0', 'schema_version': 1, 'machine': platform.platform(), 'architecture': platform.machine(),
              'probe_sha256': sha(args.probe.read_bytes()),
              'compiler': subprocess.check_output(['c++', '--version'], text=True).strip(),
              'cmake': subprocess.check_output(['cmake', '--version'], text=True).splitlines()[0],
              'command': [sys.executable] + sys.argv,
              'source_files': [{'path': name, 'sha256': sha((ROOT/name).read_bytes())} for name in
                               ['CMakeLists.txt', 'tests/reference_frame_generator.h',
                                'tests/validate_windows_reference.py', 'tests/reference_sample.py', 'tools/hypercine_reference_probe.cpp',
                                'src/cpu/standalone_frontend.cpp', 'src/core/standalone_encoder.cpp',
                                'src/cpu/wavelet_cpu.cpp']],
              'import_provenance': json.loads((CORPUS / 'import_provenance.json').read_text()),
              'sdk_compatible_profile': 'PENDING: full FS3 quantization/rate-control/framing port',
              'dynamic_state_evolution': 'PENDING: exported state is recorded, not reproduced',
              'windows_binary_parity': 'BLOCKED', 'production_gate': 'BLOCKED', 'runs': []}
    for name in ['manifest.json', 'phase3a_1080p_120_manifest.json']:
        manifest = load_manifest(CORPUS / name)
        run = {'manifest': name, 'profile': manifest['profile'], 'frames': []}
        if args.corpus_root and not args.inputs_only:
            run['binary_inventory'] = inspect_inventory(manifest, args.corpus_root)
        report['runs'].append(run)
        with tempfile.TemporaryDirectory(prefix='hypercine-reference-') as tmp:
            work = Path(tmp)
            for frame in manifest['frames']:
                if frame['width'] != 1920 or frame['height'] != 1080:
                    continue
                input_path = work / 'input.bgra'
                checked_input(args.probe, frame, input_path)
                row = {'frame_index': frame['frame_index'], 'frame_order': frame['frame_order'],
                       'pattern': frame['pattern'], 'input_sha256': frame['input_sha256'], 'input': 'PASS',
                       'state_reference_only': {k: frame[k] for k in ['fs_rate_limiter', 'previous_sample_bytes', 'quant', 'scale']}}
                run['frames'].append(row)
                if not args.inputs_only and name == 'manifest.json' and frame['frame_index'] in args.frames:
                    row['windows_artifacts'] = (verify_artifacts(manifest, frame, args.corpus_root) if args.corpus_root else
                                               {'status': 'BLOCKED', 'reason': 'Windows binaries not supplied'})
                    directory = (args.stage_dir / ('frame_%04d' % frame['frame_index'])) if args.stage_dir else work
                    reference = None
                    if args.corpus_root and row['windows_artifacts']['status'] == 'PASS':
                        original = (args.corpus_root / ('frames/tier-a-1080/frame_%04d/sdk.cfhd' % frame['frame_index'])).read_bytes()
                        reference, _ = sample.verify_sample(original, frame, 'sdk')
                    row['diagnostic'] = diagnose(args.probe, frame, input_path, directory, reference, args.sdk_replay)
        run['input_gate'] = 'PASS'
        run['input_frames_verified'] = len(run['frames'])
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(report, indent=2)+'\n')
    print(json.dumps({'input_frames_verified': [r['input_frames_verified'] for r in report['runs']],
                      'input_gate': 'PASS', 'production_gate': 'BLOCKED'}))
    return 0 if args.inputs_only else 2


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (ValueError, KeyError, StopIteration, OSError, subprocess.CalledProcessError) as error:
        print('FAIL:', error)
        raise SystemExit(1)
