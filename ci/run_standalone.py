#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
"""Run the complete standalone gate; missing or skipped tests are failures."""
import argparse
import json
from pathlib import Path
import platform
import subprocess
import sys
import xml.etree.ElementTree as ET

REQUIRED = {
    # Fail closed if the C0 geometry arithmetic or recorded-report gate disappears.
    'extreme_geometry_c0_arithmetic', 'extreme_geometry_c0_report',
    # P0 RGB444 frame and RG48 importer must never disappear from standalone CI.
    'rgb444_frame_storage', 'rgb444_frame_rejection',
    'rgb444_rg48_import', 'rgb444_rg48_rejection',
    'rgb444_rg48_replay_contract', 'rgb444_rg48_allocation_failure',
    'rgb444_allocation_failure',
    'rgb444_packet_probe',
    'reference_decode_contract',
    'quality_state_unit', 'standalone_gate_guardrails',
    'optimized_standalone_acceptance',
    'filmscan3_backend_selection', 'entropy_bitexact',
    'encoder_backend_selection_contract', 'encoder_backend_session_packets',
    'wavelet_simd_exact', 'wavelet_simd_packets',
    'cpu_isa_selection', 'gop2_reference_inventory_integrity', 'gop2_original_stage_contract',
    'filmscan3_api', 'filmscan3_fixture_integrity',
    'filmscan3_geometry', 'filmscan3_geometry_external_decode',
    'filmscan3_geometry_validation', 'filmscan3_geometry_reference_p10',
    'filmscan3_4k_gate_contract',
    # #259: loss of the capability/budget/ISA gates must fail the CI inventory.
    'encoder_capability_contract', 'filmscan3_geometry_budget',
    'wavelet_isa_session_policy', 'wavelet_isa_parallel_sessions',
    'neon_wavelet_bitexact', 'neon_concurrency_contract',
    # SDK-free Python contracts shared with the regular test tree; they are
    # registered by hypercine_register_sdk_free_contracts(), so their absence
    # from this inventory would let a dropped registration stay green.
    'test_reference_release_inventory', 'test_compare_sdk_oracle',
    'test_rca159_witness_contract', 'test_rca160_all_channel_contract',
    'test_rca160_ledger_audit',
    'qualification_report_contract', 'cuda_output_dir_platform_contract',
    'windows_reference_process_contract',
    'standalone_oracle_fixtures', 'windows_reference_inputs',
    'windows_reference_guardrails', 'windows_reference_parser',
    'windows_reference_sdk_coefficients', 'filmscan3_autonomous_manifest',
    'filmscan3_sequence_manifest', 'filmscan3_external_decode',
    'standalone_external_decode', 'standalone_windows_oracle_replay',
} | {'standalone_' + stage for stage in
     ('color', 'wavelet', 'quantization', 'entropy', 'sample', 'geometry')}
REQUIRED |= {'standalone_external_' + size for size in
             ('256x256', '1920x1080', '128x24', '384x40', '4096x24', '128x4096', '2048x1024')}

REQUIRED |= {'scalar_inverse_arithmetic', 'scalar_inverse_rejection', 'scalar_inverse_vectors',
             'scalar_inverse_replay_contract', 'original_descale_review_contract'} | {'scalar_inverse_graph_workers_' + str(w) for w in (1, 2, 4, 16)}


def cmake_enabled(value):
    # Match if(<variable>): false constants are case-insensitive, but the
    # -NOTFOUND suffix is case-sensitive. Cache BOOL values need not be ON/OFF.
    return (value.upper() not in {'', '0', 'OFF', 'NO', 'FALSE', 'N',
                                 'IGNORE', 'NOTFOUND'}
            and not value.endswith('-NOTFOUND'))


def required_tests(cache):
    required = set(REQUIRED)
    if cache.get('HYPERCINE_REFERENCE_CORPUS_ROOT'):
        required |= {'windows_reference_binaries', 'filmscan3_actual_tier_a'}
    if cache.get('HYPERCINE_FS3_SEQUENCE_ROOT'):
        required.add('filmscan3_actual_sequence')
    if cmake_enabled(cache.get('HYPERCINE_ENABLE_METAL', '')):
        required |= {'filmscan3_metal_required', 'filmscan3_metal_4k_ffmpeg_gate',
                     'filmscan3_metal_4k_encoder', 'filmscan3_metal_multisession',
                     'filmscan3_metal_geometry_matrix', 'encoder_metal_session_packets'}
        for gate in ('autonomous_manifest', 'sequence_manifest', 'external_decode',
                     'actual_tier_a', 'actual_sequence'):
            if 'filmscan3_' + gate in required:
                required.add('filmscan3_metal_' + gate)
    return required


def validate_architecture(cache, system, machine):
    # The interpreter architecture alone says nothing about the CMake target.
    # Require one explicit native Apple target; universal builds are a separate gate.
    if system == 'Darwin' and cache.get('CMAKE_OSX_ARCHITECTURES') != machine:
        raise ValueError('CMake Apple target architecture differs from the declared job')


def invalidate_results(build):
    # A failed preflight must never leave an earlier PASS available to upload.
    for name in ('standalone-gate.json', 'standalone-results.xml', 'registered-tests.json'):
        (build / name).unlink(missing_ok=True)


def registered_tests(document, required=None):
    names = [test['name'] for test in document['tests']]
    missing = (REQUIRED if required is None else required) - set(names)
    if missing or len(names) != len(set(names)):
        raise ValueError('missing/duplicate CTest registrations: ' + str(sorted(missing)))
    return set(names)


def completed_tests(path, registered):
    root = ET.parse(path).getroot()
    cases = list(root.iter('testcase'))
    names = [case.attrib['name'] for case in cases]
    if len(names) != len(set(names)) or set(names) != registered:
        raise ValueError('executed test inventory differs from registration')
    for case in cases:
        if (case.find('skipped') is not None or case.find('failure') is not None
                or case.find('error') is not None or case.get('status') != 'run'):
            raise ValueError('CTest did not pass: ' + case.attrib['name'])
    return len(cases)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, default=Path('build'))
    parser.add_argument('--system', choices=['Darwin', 'Windows', 'Linux'], required=True)
    parser.add_argument('--machine', choices=['arm64', 'x86_64'], required=True)
    args = parser.parse_args()
    build = args.build_dir.resolve()
    invalidate_results(build)
    machine = {'aarch64': 'arm64', 'amd64': 'x86_64'}.get(platform.machine().lower(), platform.machine().lower())
    print(json.dumps({'system': platform.system(), 'platform': platform.platform(),
                      'architecture': machine, 'python': sys.version}), flush=True)
    if (platform.system(), machine) != (args.system, args.machine):
        raise ValueError('runner OS/architecture does not match the declared job')
    cache = {}
    for line in (build / 'CMakeCache.txt').read_text().splitlines():
        if line and not line.startswith(('#', '//')) and '=' in line:
            key, value = line.split('=', 1)
            cache[key.split(':')[0]] = value
    for key, expected in {'HYPERCINE_BUILD_STANDALONE': 'ON', 'HYPERCINE_BUILD_UPSTREAM': 'OFF',
                          'HYPERCINE_ENABLE_CUDA': 'OFF', 'HYPERCINE_BUILD_TESTS': 'ON',
                          'HYPERCINE_REQUIRE_EXTERNAL_DECODE': 'ON'}.items():
        if cache.get(key) != expected:
            raise ValueError('invalid standalone gate configuration: ' + key)
    validate_architecture(cache, args.system, args.machine)
    commands = json.loads((build / 'compile_commands.json').read_text())
    if not commands:
        raise ValueError('empty compile database')
    for command in commands:
        if any(part in json.dumps(command).replace('\\\\', '/').replace('\\', '/')
               for part in ('/upstream/', '/src/cuda/')):
            raise ValueError('standalone build depends on upstream/CUDA')
    for key in ('HYPERCINE_FFMPEG_EXECUTABLE', 'HYPERCINE_FFPROBE_EXECUTABLE'):
        subprocess.run([cache[key], '-version'], check=True)
    subprocess.run(['cmake', '--version'], check=True)
    subprocess.run(['git', 'diff', '--exit-code', 'HEAD', '--', 'upstream/'], check=True)
    if subprocess.check_output(['git', 'status', '--porcelain', '--', 'upstream/'], text=True).strip():
        raise ValueError('upstream working tree is modified')
    ctest = ['ctest', '--test-dir', str(build), '-C', 'Release']
    document = json.loads(subprocess.check_output(ctest + ['--show-only=json-v1'], text=True))
    registered = registered_tests(document, required_tests(cache))
    if cache.get('HYPERCINE_SCALAR_ORACLE_ROOT') and 'scalar_inverse_original_six_pairs' not in registered:
        raise ValueError('requested original scalar replay not registered')
    (build / 'registered-tests.json').write_text(json.dumps(document, indent=2) + '\n')
    print('Registered full standalone tests:', len(registered), flush=True)
    junit = build / 'standalone-results.xml'
    if junit.exists():
        junit.unlink()  # Never accept a stale result from an earlier run.
    subprocess.run(ctest + ['--output-on-failure', '--no-tests=error', '--output-junit', str(junit)], check=True)
    count = completed_tests(junit, registered)
    result = {'status': 'PASS', 'registered_tests': len(registered), 'passed_tests': count,
              'system': platform.system(), 'architecture': machine,
              'scalar_original_stage_replay': ('SIX_PAIRS_EQUAL' if 'scalar_inverse_original_six_pairs' in registered else 'NOT_RUN'),
              'windows_binaries': 'NOT REQUIRED: versioned manifests/fixtures; optional NAS corpus is separate'}
    (build / 'standalone-gate.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result), flush=True)


if __name__ == '__main__':
    try:
        main()
    except (ValueError, OSError, KeyError, subprocess.CalledProcessError) as error:
        print('FAIL:', error, file=sys.stderr)
        raise SystemExit(1)
