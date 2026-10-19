#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
"""Read-only inventory or seal/verify NEW SDK exports under the private binding.

No SDK or HyperCine decoder invocation; hashes authenticate files, not producers.
Never emits the binding, arbitrary manifest contents, or OS exception messages.
Exit 2 is a completed inventory with the external-reference gate still BLOCKED.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import sys

SDK = '11574d0295771edccadd17af14af74a539f924c7'
SAFE_ID = re.compile(r'[A-Za-z0-9_. /-]+\Z')


def safe_id(value):
    if (not isinstance(value, str) or not SAFE_ID.fullmatch(value) or
            '\\' in value or ':' in value or value.startswith('/') or
            any(p in ('', '.', '..') for p in value.split('/'))):
        raise ValueError('unsafe relative artifact ID')
    return value


def bound_root():
    value = os.environ.get('HYPERCINE_GOLDEN_DIR')
    if not value:
        raise ValueError('HYPERCINE_GOLDEN_DIR is not bound')
    root = Path(value).resolve(strict=True)
    if not root.is_dir():
        raise ValueError('bound corpus is not a directory')
    return root


def under(root, relative):
    relative = safe_id(relative)
    path = root.joinpath(*PurePosixPath(relative).parts)
    path.resolve().relative_to(root.resolve())
    if path.is_symlink():
        raise ValueError('symlink artifact rejected')
    return path


def digest(path):
    hash_value = hashlib.sha256()
    length = 0
    with path.open('rb') as stream:
        while True:
            block = stream.read(1024 * 1024)
            if not block:
                break
            length += len(block)
            hash_value.update(block)
    return {'byte_length': length, 'sha256': hash_value.hexdigest()}


def write_new(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open('x', encoding='utf-8', newline='\n') as stream:
        json.dump(value, stream, indent=2, sort_keys=True, allow_nan=False)
        stream.write('\n')


def inventory(root):
    manifests = []
    extensions = {}
    total_bytes = 0
    candidates = []
    unparsed = []
    # Keep DirEntry's cached Windows enumeration metadata: a remote stat and
    # resolve for every file unnecessarily multiply SMB round trips.
    pending = [root]
    while pending:
        directory = pending.pop()
        with os.scandir(directory) as scan:
            entries = sorted(scan, key=lambda e: e.name)
        for entry in entries:
            if entry.is_symlink() or getattr(entry, 'is_junction', lambda: False)():
                raise ValueError('linked corpus entry rejected')
            if entry.is_dir(follow_symlinks=False):
                pending.append(Path(entry.path))
                continue
            if not entry.is_file(follow_symlinks=False):
                raise ValueError('nonregular corpus file rejected')
            path = Path(entry.path)
            relative = path.relative_to(root).as_posix()
            safe_id(relative)
            size = entry.stat(follow_symlinks=False).st_size
            total_bytes += size
            ext = path.suffix.lower() or '(none)'
            extensions[ext] = extensions.get(ext, 0) + 1
            if ext != '.json':
                continue
            data = path.read_bytes()
            record = {'id': relative, 'byte_length': len(data),
                      'sha256': hashlib.sha256(data).hexdigest()}
            try:
                parsed = json.loads(data)
                # Discovery only: a text match must never qualify a reference.
                possible = bool(re.search(r'FIELDPLUS|2FRAME|GOP.?2|"gop(?:_size)?"\s*:\s*2',
                                          data.decode('utf-8'), re.I))
                record['gop2_candidate'] = possible
                record['contains_artifact_index'] = isinstance(parsed, dict) and 'artifacts' in parsed
                if possible:
                    candidates.append(relative)
            except (ValueError, UnicodeError):
                record['parse_status'] = 'INVALID_JSON'
                unparsed.append(relative)
            manifests.append(record)
    required = []
    for w, h in ((1920, 1080), (3840, 2160)):
        for frame in (0, 1):
            for stage in ('source', 'original_sdk_packet', 'decoded_BGRA', 'decoded_YU64',
                          'temporal_inverse', 'wavelet_LL', 'pre_inverse_coefficients', 'signed_planar'):
                required.append({'geometry': [w, h], 'frame_index': frame, 'stage': stage,
                                 'status': 'MISSING', 'reason': 'NO_QUALIFIED_ORIGINAL_GOP2_REFERENCE'})
    return {'schema': 'hypercine.gop2.inventory/1', 'issue': 189,
            'binding': 'HYPERCINE_GOLDEN_DIR', 'release_gate': 'BLOCKED',
            'scope': 'Existing loose-file metadata inventory; archives not opened; no SDK execution',
            'file_count': sum(extensions.values()), 'total_byte_length': total_bytes,
            'extension_counts': extensions, 'manifests': manifests,
            'gop2_manifest_candidates': candidates, 'unparsed_manifests': unparsed,
            'required': required,
            'limitations': ['Manifest discovery is not producer authentication.',
                            'Unindexed bytes and hash-only reports cannot qualify an original SDK oracle.',
                            'Existing GOP1 and RGB/RGBA/Bayer corpora are not GOP2 goldens.']}


def seal(root, binary, source):
    observed_path = root / 'observations.json'
    observation = json.loads(observed_path.read_text(encoding='utf-8'))
    if observation.get('schema') != 'hypercine.gop2.observations/1' or observation.get('sdk_revision') != SDK:
        raise ValueError('unknown exporter/SDK contract')
    geometry = (observation['width'], observation['height'])
    if geometry not in ((1920, 1080), (3840, 2160)):
        raise ValueError('unsupported geometry')
    controls = observation['observations']
    trusted_control = (observation.get('stable') is True and observation.get('source_quality_pass') is True
                       and len(controls) == 6)
    for control in controls:
        trusted_control = trusted_control and (control.get('api_status') == 0 and
            control.get('allowed_cpus') == 1 and control.get('entropy_pool') in (0, 1) and
            control.get('worker_pool') in (0, 1) and control.get('same_instance_repeats', 0) >= 3 and
            control.get('progressive') == 1 and control.get('precision') == 10 and control.get('num_frames') == 2)
        if control['format'] == 'YU64':
            trusted_control = trusted_control and all(p > 35 for p in control['source_psnr_db'])
    frame_lengths = geometry[0] * geometry[1] * 4
    for fmt in ('YU64', 'BGRA'):
        for f in (0, 1):
            records = [digest(root / f'{fmt}_call{r}_frame{f}.raw') for r in range(3)]
            trusted_control = trusted_control and len({v['sha256'] for v in records}) == 1
            trusted_control = trusted_control and all(v['byte_length'] == frame_lengths for v in records)
        trusted_control = trusted_control and (digest(root / f'{fmt}_call0_frame0.raw')['sha256'] !=
                                              digest(root / f'{fmt}_call0_frame1.raw')['sha256'])
    sources = [digest(root / f'source_frame{f}.yuy2') for f in (0, 1)]
    if (any(s['byte_length'] != geometry[0] * geometry[1] * 2 for s in sources) or
            sources[0]['sha256'] == sources[1]['sha256']):
        raise ValueError('source storage/asymmetry mismatch')
    packet = digest(root / 'original_sdk.cfhd')
    binary_digest, source_digest = digest(binary), digest(source)
    library_digest = digest(binary.parents[2] / 'upstream/Release/CFHDCodecStatic.lib')
    status = 'AVAILABLE' if trusted_control else 'UNSTABLE_REFERENCE'
    stages = {s['id']: s for s in observation['stages']}
    originals = sorted(p for p in root.rglob('*') if p.is_file())
    artifacts = []
    for path in originals:
        relative = safe_id(path.relative_to(root).as_posix())
        measured = digest(path)
        producer = 'original_sdk_decoder'
        inputs = [packet['sha256']]
        artifact_status = status
        if path.name.startswith('source_frame'):
            producer = 'versioned_test_source_generator'
            inputs = []
            artifact_status = 'AVAILABLE'
        elif path.suffix == '.cfhd':
            producer = 'original_sdk_encoder'
            inputs = [s['sha256'] for s in sources]
            artifact_status = 'AVAILABLE'
        elif path.name == 'observations.json':
            producer = 'versioned_test_observer'
        sidecar = {'schema': 'hypercine.gop2.artifact/1', 'id': relative, **measured,
                   'status': artifact_status, 'producer': producer,
                   'input_sha256': inputs, 'sdk_revision': SDK,
                   'geometry': list(geometry), 'qualification': 'bounded_single_cpu_control',
                   'exporter_binary': binary_digest, 'exporter_source': source_digest,
                   'sdk_static_library': library_digest}
        if path.suffix == '.i16le':
            stage = stages[path.stem]
            if measured['byte_length'] != stage['pitch'] * stage['height']:
                raise ValueError('stage length mismatch')
            sidecar['stage'] = stage
            sidecar['representation'] = 'signed_int16_little_endian_compact_rows'
            sidecar['capture_point'] = 'resident_buffer_after_original_SDK_full_inverse'
        elif path.suffix == '.raw':
            sidecar['frame_index'] = int(path.stem[-1])
            sidecar['format'] = path.name.split('_')[0]
            sidecar['pitch'] = observation['output_pitch']
            sidecar['orientation'] = 'bottom_up' if sidecar['format'] == 'BGRA' else 'top_down'
        elif path.suffix == '.yuy2':
            sidecar['frame_index'] = int(path.stem[-1])
            sidecar['format'] = 'YUY2'
            sidecar['pitch'] = observation['source_pitch']
        write_new(path.with_name(path.name + '.json'), sidecar)
        with path.with_name(path.name + '.sha256').open('x', encoding='ascii') as stream:
            stream.write(measured['sha256'] + '  ' + path.name + '\n')
        artifacts.append(sidecar)
    manifest = {'schema': 'hypercine.gop2.manifest/1', 'issue': 189,
                'sdk_revision': SDK, 'geometry': list(geometry), 'reference_status': status,
                'observations_sha256': digest(observed_path)['sha256'],
                'orientation_contract': {'source_YUY2': 'top_down', 'decoded_YU64': 'top_down',
                                         'decoded_BGRA': 'bottom_up'},
                'observation_v1_note': 'orientation in observations/1 names the source/YU64 orientation; BGRA uses bottom-up storage',
                'artifacts': artifacts, 'release_gate': 'BLOCKED',
                'required_missing': ['candidate_187_exact_head_comparison', 'pre_inverse_entropy_coefficients',
                                     'final_signed_planar_oracle', 'parsed_chunk_topology'],
                'limitations': ['Bounded control only; no default-threading qualification.',
                                'Post-inverse resident bands are not pre-inverse entropy goldens.',
                                'No intermediate stage is synthesized from HyperCine output.']}
    write_new(root / 'manifest.json', manifest)
    with (root / 'manifest.json.sha256').open('x', encoding='ascii') as stream:
        stream.write(digest(root / 'manifest.json')['sha256'] + '  manifest.json\n')
    return manifest


def verify(root):
    manifest = json.loads((root / 'manifest.json').read_text(encoding='utf-8'))
    if manifest.get('schema') != 'hypercine.gop2.manifest/1':
        raise ValueError('unknown manifest')
    for artifact in manifest['artifacts']:
        path = under(root, artifact['id'])
        if digest(path) != {k: artifact[k] for k in ('sha256', 'byte_length')}:
            raise ValueError('artifact digest/length mismatch')
        sidecar = json.loads(path.with_name(path.name + '.json').read_text(encoding='utf-8'))
        if sidecar != artifact:
            raise ValueError('artifact sidecar mismatch')
        expected = artifact['sha256'] + '  ' + path.name + '\n'
        if path.with_name(path.name + '.sha256').read_text(encoding='ascii') != expected:
            raise ValueError('SHA sidecar mismatch')
    expected = digest(root / 'manifest.json')['sha256'] + '  manifest.json\n'
    if (root / 'manifest.json.sha256').read_text(encoding='ascii') != expected:
        raise ValueError('manifest digest mismatch')
    return {'verified_artifacts': len(manifest['artifacts']), 'reference_status': manifest['reference_status'],
            'release_gate': 'BLOCKED'}


def validate_comparison_shape(report):
    expected = set()
    for frame in (0, 1):
        expected.update((frame, label) for label in ('BGRA', 'YU64_clipped'))
        for channel in range(3):
            for band in range(17):
                if (frame == 0 and 11 <= band <= 13) or (frame == 1 and 14 <= band <= 16):
                    continue
                expected.add((frame, f'ch{channel}_resident_band{band}'))
            for stage in ('wavelet4_ll', 'temporal_low', 'temporal_high', f'frame{frame}_base_ll'):
                expected.add((frame, f'ch{channel}_{stage}'))
    comparisons = report.get('comparisons', [])
    actual = {(item['frame_index'], item['id']) for item in comparisons}
    if actual != expected or len(comparisons) != len(expected) or report.get('compared') != len(expected):
        raise ValueError('comparison matrix incomplete or duplicated')


def seal_comparison(root, references, binary, source):
    reference_result = verify(references)
    if reference_result['reference_status'] != 'AVAILABLE':
        raise ValueError('untrustworthy original reference')
    report_path = root / 'comparison.json'
    report = json.loads(report_path.read_text(encoding='utf-8'))
    validate_comparison_shape(report)
    if (report.get('schema') != 'hypercine.gop2.comparison/1' or
            report.get('candidate_sha') != '7ed4a82ed97598454c2680bd264dedf259e6e7dc' or
            report.get('compared') != 112 or len(report.get('comparisons', [])) != 112):
        raise ValueError('incomplete candidate comparison')
    result = {'schema': 'hypercine.gop2.comparison_index/1', 'issue': 189,
              'candidate_sha': report['candidate_sha'], 'geometry': [report['width'], report['height']],
              'release_gate': 'BLOCKED', 'reference_status': 'AVAILABLE',
              'comparison_report': digest(report_path), 'binary': digest(binary), 'source': digest(source),
              'decoder_library': digest(binary.parents[1] / 'src/Release/hypercine_decoder.lib'),
              'wavelet_library': digest(binary.parents[1] / 'src/Release/hypercine_wavelet.lib'),
              'input_packet': digest(references / 'original_sdk.cfhd'),
              'reference_manifest': digest(references / 'manifest.json'),
              'topology': report['topology'], 'not_materialized_per_channel': report['not_materialized_per_channel'],
              'required_missing': report['missing'], 'comparisons': []}
    mismatched = 0
    for comparison in report['comparisons']:
        candidate_path = under(root, comparison['artifact'])
        identifier = comparison['id']
        if identifier in ('BGRA', 'YU64_clipped'):
            fmt = 'BGRA' if identifier == 'BGRA' else 'YU64'
            reference_id = f'{fmt}_call0_frame{comparison["frame_index"]}.raw'
        else:
            reference_id = f'stages/{identifier}.i16le'
        candidate_digest = digest(candidate_path)
        expected_digest = digest(under(references, reference_id))
        equal = candidate_digest == expected_digest
        if equal != comparison['equal'] or candidate_digest['byte_length'] != comparison['actual_bytes']:
            raise ValueError('comparison evidence inconsistent')
        mismatched += not equal
        item = {**comparison, 'actual': candidate_digest, 'expected': expected_digest,
                'reference_id': reference_id, 'producer': 'hypercine_candidate_decoder',
                'input_sha256': result['input_packet']['sha256'],
                'reference_qualification': 'bounded_original_sdk_single_cpu_control'}
        write_new(candidate_path.with_name(candidate_path.name + '.json'), item)
        with candidate_path.with_name(candidate_path.name + '.sha256').open('x', encoding='ascii') as stream:
            stream.write(candidate_digest['sha256'] + '  ' + candidate_path.name + '\n')
        result['comparisons'].append(item)
    if mismatched != report['mismatched']:
        raise ValueError('mismatch total inconsistent')
    result['compared'], result['mismatched'] = 112, mismatched
    write_new(root / 'comparison_index.json', result)
    with (root / 'comparison_index.json.sha256').open('x', encoding='ascii') as stream:
        stream.write(digest(root / 'comparison_index.json')['sha256'] + '  comparison_index.json\n')
    return result


def verify_comparison(root, references):
    verify(references)
    index_path = root / 'comparison_index.json'
    index = json.loads(index_path.read_text(encoding='utf-8'))
    validate_comparison_shape(index)
    if (index.get('schema') != 'hypercine.gop2.comparison_index/1' or
            index.get('compared') != 112 or len(index.get('comparisons', [])) != 112):
        raise ValueError('incomplete comparison index')
    expected = digest(index_path)['sha256'] + '  comparison_index.json\n'
    if (root / 'comparison_index.json.sha256').read_text(encoding='ascii') != expected:
        raise ValueError('comparison index digest mismatch')
    if digest(root / 'comparison.json') != index['comparison_report']:
        raise ValueError('comparison report digest mismatch')
    if digest(references / 'original_sdk.cfhd') != index['input_packet']:
        raise ValueError('comparison input mismatch')
    for item in index['comparisons']:
        path = under(root, item['artifact'])
        if digest(path) != item['actual'] or digest(under(references, item['reference_id'])) != item['expected']:
            raise ValueError('comparison artifact digest mismatch')
        if json.loads(path.with_name(path.name + '.json').read_text(encoding='utf-8')) != item:
            raise ValueError('comparison sidecar mismatch')
        if path.with_name(path.name + '.sha256').read_text(encoding='ascii') != item['actual']['sha256'] + '  ' + path.name + '\n':
            raise ValueError('comparison SHA sidecar mismatch')
    return {'verified_artifacts': 112, 'mismatched': index['mismatched'], 'release_gate': 'BLOCKED'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mode', choices=('inventory', 'seal', 'verify', 'seal-comparison', 'verify-comparison'))
    parser.add_argument('--relative', help='safe directory ID under the bound corpus')
    parser.add_argument('--out', type=Path, help='new sanitized report (never overwrite)')
    parser.add_argument('--binary', type=Path)
    parser.add_argument('--source', type=Path)
    parser.add_argument('--references', type=Path, help='already sealed original-SDK reference directory')
    args = parser.parse_args()
    try:
        root = bound_root()
        if args.mode == 'inventory':
            result = inventory(root)
        else:
            if not args.relative:
                raise ValueError('relative export directory required')
            root = under(root, args.relative)
            if args.mode == 'seal':
                result = seal(root, args.binary, args.source)
            elif args.mode == 'seal-comparison':
                result = seal_comparison(root, args.references, args.binary, args.source)
            elif args.mode == 'verify-comparison':
                result = verify_comparison(root, args.references)
            else:
                result = verify(root)
        if args.out:
            write_new(args.out, result)
        print(json.dumps({'mode': args.mode, 'release_gate': 'BLOCKED',
                          'status': result.get('reference_status', 'INVENTORIED'),
                          'file_count': result.get('file_count'),
                          'verified_artifacts': result.get('verified_artifacts')}))
        return 2
    except (OSError, ValueError, KeyError, TypeError):
        print('Reference operation failed; private diagnostics withheld; gate BLOCKED.', file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
