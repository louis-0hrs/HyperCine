#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
"""Pin and independently verify Phase-B original stages and candidate bytes.

Only the two existing packets are used. Success exits 2 because owner review
remains required. Any absent/invalid/changed evidence exits 1, never a pass.
"""
import argparse
import json
from pathlib import Path
import sys
import gop2_reference_inventory as base

CANDIDATE = '7ed4a82ed97598454c2680bd264dedf259e6e7dc'


def identical(left, right):
    # JSON identities must not accept Python's False == 0 or 1920 == 1920.0.
    return json.dumps(left, sort_keys=True) == json.dumps(right, sort_keys=True)


def validate_observations(report):
    if report.get('schema') != 'hypercine.gop2.original_stages/1' or report.get('observer_version') != 2:
        raise ValueError('unknown original stage observer')
    w, h = report['width'], report['height']
    if type(w) is not int or type(h) is not int or (w, h) not in ((1920, 1080), (3840, 2160)):
        raise ValueError('unsupported geometry')
    required = {'neutrality_frame_checks': 12, 'neutrality_resident_checks': 198,
                'alternate_original_path_checks': 18, 'fresh_decoders_per_format': 3,
                'same_instance_calls': 3, 'stage_observations_per_band_or_row': 9,
                'cpu_affinity_count': 1, 'allowed_cpus': 1, 'entropy_pool': 0, 'worker_pool': 1,
                'sample_reencoded': False, 'stability': 'PASS', 'instrumentation_neutrality': 'PASS',
                'sdk_revision': base.SDK, 'stage_output_format': 'YU64',
                'matrix': 'BT709', 'range': 'video',
                'signed_plane_channel_order': ['Y', 'V', 'U'],
                'signed_plane_orientation': 'top_down',
                'signed_plane_storage': 'signed_int16_little_endian_before_p10_clipping_and_YU64_packing',
                'native_signed16_saturation_retained': True,
                'signed_path': 'alternate_original_SDK_spatial_signed_path_with_default_output_parity'}
    if any(not identical(report.get(k), v) for k, v in required.items()):
        raise ValueError('original SDK neutrality/stability/control incomplete')
    bands = report['pre_inverse_bands']
    planes = report['signed_planes']
    if len(bands) != 51 or {(b['channel'], b['logical_band']) for b in bands} != {
            (c, b) for c in range(3) for b in range(17)}:
        raise ValueError('pre-inverse original matrix incomplete or duplicated')
    if len(planes) != 6 or {(p['frame_index'], p['channel']) for p in planes} != {
            (f, c) for f in (0, 1) for c in range(3)}:
        raise ValueError('signed original matrix incomplete or duplicated')
    for stage in bands + planes:
        base.safe_id(stage['id'])
        fields = ('width', 'height', 'pitch', 'channel')
        if any(type(stage.get(k)) is not int for k in fields):
            raise ValueError('stage numeric identity type invalid')
        if (stage['width'] <= 0 or stage['height'] <= 0 or stage['pitch'] != stage['width'] * 2 or
                stage.get('original_pitch', stage['pitch']) < stage['pitch']):
            raise ValueError('stage dimensions/pitch invalid')
    for plane in planes:
        if plane.get('capture_point') != 'TransformInverseSpatialToV210_before_ConvertYUVStripPlanarToV210':
            raise ValueError('signed-plane capture boundary invalid')
        if type(plane.get('frame_index')) is not int or plane['id'] != f'signed_frame{plane["frame_index"]}_ch{plane["channel"]}.i16le':
            raise ValueError('signed-plane identity invalid')
        if (plane['width'], plane['height']) != (w if plane['channel'] == 0 else w//2, h):
            raise ValueError('signed-plane geometry invalid')
    for band in bands:
        if band.get('capture_point') != 'DecodeSampleSubband_after_entropy_before_inverse_submission':
            raise ValueError('pre-inverse capture boundary invalid')
        if type(band.get('logical_band')) is not int or band['id'] != f'pre_ch{band["channel"]}_band{band["logical_band"]}.i16le':
            raise ValueError('pre-inverse band identity invalid')
    return {s['id']: s for s in bands + planes}


def validate_comparison(report):
    if report.get('schema') != 'hypercine.gop2.stage_comparison/1' or report.get('candidate_sha') != CANDIDATE:
        raise ValueError('unknown stage candidate')
    expected = set()
    for f in (0, 1):
        for c in range(3):
            expected.add((f, f'signed_frame{f}_ch{c}.i16le'))
            for b in range(17):
                if (f == 0 and 11 <= b <= 13) or (f == 1 and b >= 14):
                    continue
                expected.add((f, f'pre_ch{c}_band{b}.i16le'))
    records = report['comparisons']
    if type(report.get('compared')) is not int or any(type(r.get('frame_index')) is not int for r in records):
        raise ValueError('comparison identity types invalid')
    if (len(records) != 90 or report.get('compared') != 90 or
            {(r['frame_index'], r['reference_id']) for r in records} != expected):
        raise ValueError('new-stage comparison matrix incomplete or duplicated')
    if (report.get('pre_inverse_band_comparisons'), report.get('signed_plane_comparisons'),
            report.get('unique_pre_inverse_band_oracles')) != (84, 6, 51):
        raise ValueError('new-stage totals invalid')


def validate_binding(index, observations, comparison, reference_manifest):
    validate_observations(observations)
    validate_comparison(comparison)
    geometry = [observations['width'], observations['height']]
    if (index.get('candidate_sha') != CANDIDATE or not identical(index.get('geometry'), geometry) or
            not identical(reference_manifest.get('geometry'), geometry) or
            not identical([comparison.get('width'), comparison.get('height')], geometry) or
            not identical(index.get('observations'), observations)):
        raise ValueError('stage evidence producer/geometry/observation binding differs')
    if (type(index.get('mismatched')) is not int or type(comparison.get('mismatched')) is not int or
            index['mismatched'] != comparison['mismatched']):
        raise ValueError('stage report/index totals differ')
    expected = validate_observations(observations)
    for item in index['original_stages']:
        stage = expected.get(item['id'])
        if (stage is None or type(item.get('byte_length')) is not int or
                item['byte_length'] != stage['pitch'] * stage['height']):
            raise ValueError('original stage byte length differs from declared rows')
        representation = {'representation': 'signed_int16_little_endian_compact_rows',
                          'sdk_channel_order': ['Y', 'V', 'U'], 'orientation': 'top_down',
                          'matrix': 'BT709', 'range': 'video', 'output_format_context': 'YU64',
                          'signed_path_if_applicable': observations['signed_path']}
        if any(not identical(item.get(key), value) for key, value in representation.items()):
            raise ValueError('original stage representation differs')
        if (not identical(item.get('stage'), expected.get(item['id'])) or
                not identical(item.get('input_packet'), index['input_packet']) or
                item.get('producer') != 'original_sdk_private_observer_overlay_v2' or item.get('status') != 'AVAILABLE'):
            raise ValueError('original stage provenance differs')
    report_items = {(r['frame_index'], r['reference_id']): r for r in comparison['comparisons']}
    for item in index['comparisons']:
        stage = expected.get(item['reference_id'])
        if stage is None:
            raise ValueError('comparison references unknown stage')
        required_bytes = stage['pitch'] * stage['height']
        for field in ('actual', 'expected'):
            if (type(item[field].get('byte_length')) is not int or
                    item[field]['byte_length'] != required_bytes):
                raise ValueError('comparison byte length differs from reference rows')
        original = report_items.get((item['frame_index'], item['reference_id']))
        if original is None or any(not identical(item.get(k), value) for k, value in original.items()):
            raise ValueError('comparison report/index fields disagree')
        if (type(item.get('equal')) is not bool or item['equal'] != (item['actual'] == item['expected']) or
                item.get('actual_bytes') != item['actual']['byte_length'] or
                item.get('expected_bytes') != item['expected']['byte_length'] or
                item.get('width') != expected[item['reference_id']]['width']):
            raise ValueError('stage comparison equality/storage claims inconsistent')


def sha_sidecar(path, digest):
    with path.with_name(path.name + '.sha256').open('x', encoding='ascii') as stream:
        stream.write(digest + '  ' + path.name + '\n')


def check_sha_sidecar(path, digest):
    if path.with_name(path.name + '.sha256').read_text(encoding='ascii') != digest + '  ' + path.name + '\n':
        raise ValueError('SHA sidecar differs')


def seal(args):
    if base.verify(args.reference)['reference_status'] != 'AVAILABLE':
        raise ValueError('original Phase-A reference unqualified')
    observations = json.loads((args.stages / 'stage_observations.json').read_text())
    metadata = validate_observations(observations)
    comparison = json.loads((args.candidate / 'stage_comparison.json').read_text())
    validate_comparison(comparison)
    if (comparison['width'], comparison['height']) != (observations['width'], observations['height']):
        raise ValueError('cross-geometry comparison')
    observer = json.loads(args.observer.read_text())
    if (observer.get('version') != 2 or observer.get('sdk_revision') != base.SDK or
            observer.get('tracked_upstream_modified') is not False):
        raise ValueError('private SDK provenance invalid')
    for source in observer['sources']:
        path = base.under(args.observer.parent, source['id'])
        if base.digest(path) != {'sha256': source['instrumented_sha256'],
                                 'byte_length': source['instrumented_byte_length']}:
            raise ValueError('compiled private SDK source changed')
    toolchain = {'extractor_binary': base.digest(args.binary), 'extractor_source': base.digest(args.source),
                 'phase_a_helper_source': base.digest(args.source.parent / 'gop2_reference_export.cpp'),
                 'pristine_sdk_archive': base.digest(args.source.parent.parent / 'build/upstream/Release/CFHDCodecStatic.lib'),
                 'observer_index': base.digest(args.observer), 'observer': observer,
                 'comparison_binary': base.digest(args.candidate_binary),
                 'comparison_source': base.digest(args.candidate_source),
                 'candidate_decoder_archive': base.digest(args.candidate_binary.parents[1] / 'src/Release/hypercine_decoder.lib'),
                 'candidate_wavelet_archive': base.digest(args.candidate_binary.parents[1] / 'src/Release/hypercine_wavelet.lib')}
    packet = base.digest(args.reference / 'original_sdk.cfhd')
    originals = []
    # Validate every byte and comparison before writing any final metadata.
    original_measurements = {}
    for identifier, stage in metadata.items():
        path = base.under(args.stages, identifier)
        measured = base.digest(path)
        if measured['byte_length'] != stage['pitch'] * stage['height']:
            raise ValueError('original stage length invalid')
        original_measurements[identifier] = measured
    measured_comparisons = []
    mismatches = 0
    for record in comparison['comparisons']:
        path = base.under(args.candidate, record['artifact'])
        actual = base.digest(path)
        expected = original_measurements[record['reference_id']]
        equal = actual == expected
        if (record['equal'] != equal or record['actual_bytes'] != actual['byte_length'] or
                record['expected_bytes'] != expected['byte_length']):
            raise ValueError('comparison claims do not match file hashes/lengths')
        mismatches += not equal
        measured_comparisons.append({**record, 'actual': actual, 'expected': expected})
    if mismatches != comparison['mismatched']:
        raise ValueError('comparison mismatch total invalid')
    for identifier, stage in metadata.items():
        item = {'schema': 'hypercine.gop2.original_stage_artifact/1', 'id': identifier,
                **original_measurements[identifier], 'status': 'AVAILABLE',
                'producer': 'original_sdk_private_observer_overlay_v2', 'input_packet': packet,
                'stage': stage, 'representation': 'signed_int16_little_endian_compact_rows',
                'matrix': 'BT709', 'range': 'video', 'output_format_context': 'YU64',
                'sdk_channel_order': ['Y', 'V', 'U'], 'orientation': 'top_down',
                'qualification': 'bounded_single_cpu_control_on_pinned_packet',
                'signed_path_if_applicable': observations['signed_path']}
        path = base.under(args.stages, identifier)
        base.write_new(path.with_name(path.name + '.json'), item)
        sha_sidecar(path, item['sha256'])
        originals.append(item)
    for item in measured_comparisons:
        path = base.under(args.candidate, item['artifact'])
        base.write_new(path.with_name(path.name + '.json'), item)
        sha_sidecar(path, item['actual']['sha256'])
    sha_sidecar(args.stages / 'stage_observations.json', base.digest(args.stages / 'stage_observations.json')['sha256'])
    sha_sidecar(args.candidate / 'stage_comparison.json', base.digest(args.candidate / 'stage_comparison.json')['sha256'])
    result = {'schema': 'hypercine.gop2.phaseb_index/1', 'license': 'MIT OR Apache-2.0',
              'issue': 189, 'geometry': [observations['width'], observations['height']],
              'candidate_sha': CANDIDATE, 'input_packet': packet,
              'phase_a_manifest': base.digest(args.reference / 'manifest.json'),
              'observations_file': base.digest(args.stages / 'stage_observations.json'),
              'comparison_file': base.digest(args.candidate / 'stage_comparison.json'),
              'observations': observations, 'toolchain': toolchain,
              'original_stages': originals, 'comparisons': measured_comparisons,
              'compared': 90, 'mismatched': mismatches,
              'pre_inverse_entropy_oracle': 'AVAILABLE', 'unclipped_signed_planes': 'AVAILABLE',
              'reference_result': 'PASS' if mismatches == 0 else 'MISMATCH',
              'release_gate': 'OWNER_REVIEW_REQUIRED' if mismatches == 0 else 'BLOCKED',
              'limitations': ['Two pinned generated-source SDK packets only; no default-pool/race-free claim.',
                              'Signed planes use the identified alternate frozen SDK spatial path with exact default-output controls.',
                              'Native signed16 transform saturation is retained; no p10/output packing clip is applied to these planes.',
                              'Phase-A originals are unchanged; their old MISSING fields are historical.']}
    base.write_new(args.stages / 'phaseb_index.json', result)
    sha_sidecar(args.stages / 'phaseb_index.json', base.digest(args.stages / 'phaseb_index.json')['sha256'])
    if args.out:
        base.write_new(args.out, result)
    return result


def verify(args):
    if base.verify(args.reference)['reference_status'] != 'AVAILABLE':
        raise ValueError('Phase-A reference untrustworthy')
    path = args.stages / 'phaseb_index.json'
    check_sha_sidecar(path, base.digest(path)['sha256'])
    index = json.loads(path.read_text())
    if index.get('schema') != 'hypercine.gop2.phaseb_index/1':
        raise ValueError('unknown Phase-B index')
    observations = json.loads((args.stages / 'stage_observations.json').read_text())
    report = json.loads((args.candidate / 'stage_comparison.json').read_text())
    reference_manifest = json.loads((args.reference / 'manifest.json').read_text())
    validate_binding(index, observations, report, reference_manifest)
    if (base.digest(args.reference / 'original_sdk.cfhd') != index['input_packet'] or
            base.digest(args.reference / 'manifest.json') != index['phase_a_manifest'] or
            base.digest(args.stages / 'stage_observations.json') != index['observations_file'] or
            base.digest(args.candidate / 'stage_comparison.json') != index['comparison_file']):
        raise ValueError('Phase-B input/observation identity differs')
    if len(index['original_stages']) != 57 or len(index['comparisons']) != 90:
        raise ValueError('Phase-B index matrix incomplete')
    if {s['id'] for s in index['original_stages']} != set(validate_observations(index['observations'])):
        raise ValueError('Phase-B original index matrix incomplete or duplicated')
    report = json.loads((args.candidate / 'stage_comparison.json').read_text())
    validate_comparison({**report, 'comparisons': index['comparisons']})
    check_sha_sidecar(args.stages / 'stage_observations.json', index['observations_file']['sha256'])
    check_sha_sidecar(args.candidate / 'stage_comparison.json', index['comparison_file']['sha256'])
    mismatches = 0
    for item in index['original_stages']:
        artifact = base.under(args.stages, item['id'])
        if base.digest(artifact) != {k: item[k] for k in ('sha256', 'byte_length')}:
            raise ValueError('original stage bytes changed')
        if json.loads(artifact.with_name(artifact.name + '.json').read_text()) != item:
            raise ValueError('original stage sidecar changed')
        check_sha_sidecar(artifact, item['sha256'])
    for item in index['comparisons']:
        actual_path = base.under(args.candidate, item['artifact'])
        expected_path = base.under(args.stages, item['reference_id'])
        actual, expected = base.digest(actual_path), base.digest(expected_path)
        if actual != item['actual'] or expected != item['expected'] or (actual == expected) != item['equal']:
            raise ValueError('candidate/new-reference bytes changed')
        if json.loads(actual_path.with_name(actual_path.name + '.json').read_text()) != item:
            raise ValueError('comparison sidecar changed')
        check_sha_sidecar(actual_path, item['actual']['sha256'])
        mismatches += actual != expected
    if mismatches != index['mismatched']:
        raise ValueError('verified mismatch total invalid')
    expected_result = 'PASS' if mismatches == 0 else 'MISMATCH'
    expected_gate = 'OWNER_REVIEW_REQUIRED' if mismatches == 0 else 'BLOCKED'
    if index.get('reference_result') != expected_result or index.get('release_gate') != expected_gate:
        raise ValueError('stage gate claims inconsistent')
    return index


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mode', choices=('seal', 'verify'))
    for field in ('stages', 'reference', 'candidate'):
        parser.add_argument('--' + field, type=Path, required=True)
    for field in ('observer', 'binary', 'source', 'candidate-binary', 'candidate-source', 'out'):
        parser.add_argument('--' + field, type=Path)
    args = parser.parse_args()
    try:
        if args.mode == 'seal' and any(getattr(args, k) is None for k in (
                'observer', 'binary', 'source', 'candidate_binary', 'candidate_source')):
            raise ValueError('sealing provenance required')
        result = seal(args) if args.mode == 'seal' else verify(args)
        print(json.dumps({'status': result['reference_result'], 'original_stages': 57,
                          'compared': 90, 'mismatched': result['mismatched'], 'release_gate': result['release_gate']}))
        return 2
    except (OSError, ValueError, KeyError, TypeError):
        print('Original-stage evidence invalid or incomplete; gate BLOCKED.', file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
