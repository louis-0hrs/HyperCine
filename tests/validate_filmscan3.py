#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
"""Test actual autonomous CPU FilmScan3 samples; no manifest tables enter encoder."""
import argparse
import array
import math
import struct
import hashlib
import json
from pathlib import Path
import platform
import os
import shutil
import subprocess
import sys
import tempfile
import reference_sample as sample
import validate_windows_reference as gate
from validate_standalone import avi


def compare_run(probe, encoder, manifest, corpus, directory, ffmpeg=None, inject=False, cpu_encoder=None):
    directory.mkdir(parents=True, exist_ok=True)
    inputs, metadata, expected = [], bytearray(), []
    run_name = 'tier-a-1080' if any('tier-a-1080/' in a['path'] for a in manifest['artifacts']) else 'sequence-1920x1080'
    for frame in manifest['frames']:
        if (frame['width'], frame['height']) != (1920, 1080):
            continue
        path = directory/('input_%04d.bgra' % frame['frame_index'])
        gate.checked_input(probe, frame, path)
        expected.append(frame); inputs.append(str(path))
        if inject:
            source = corpus/'frames'/run_name/('frame_%04d' % frame['frame_index'])/'sdk.cfhd'
            raw = source.read_bytes()
            gate.verify_bytes(raw, frame['sdk_sha256'], frame['sdk_byte_length'], 'SDK original before metadata injection')
            parsed = sample.parse_sample(raw)
            records = {v['tag']: raw[v['offset']:v['offset']+v['length']] for v in parsed['metadata_mask_ranges']}
            for key, size in [('TAG_CLIP_GUID', 16), ('TAG_ENCODE_DATE', 10), ('TAG_ENCODE_TIME', 8), ('TAG_TIMECODE', 11)]:
                metadata.extend(records[key][:size])
    arguments = [str(encoder)]
    if ffmpeg:
        arguments += ['--reference-yuv']
    if inject:
        path = directory/'variable-metadata.raw';path.write_bytes(metadata)
        arguments += ['--metadata-binary', str(path)]
    cpu_options = arguments[1:]
    output = directory/'samples';arguments += [str(output)] + inputs
    subprocess.run(arguments, check=True)
    trace = json.loads((output/'trace.json').read_text())['frames']
    cpu_output = directory/'cpu-samples'
    if cpu_encoder:
        # Identical input paths and explicit metadata; compare raw full packets,
        # independently from SDK-normalized hashes and the mandatory stage audit.
        subprocess.run([str(cpu_encoder)] + cpu_options + [str(cpu_output)] + inputs, check=True)
        if json.loads((cpu_output/'trace.json').read_text())['frames'] != trace:
            raise ValueError('same-input comparison encoder state/tables/30 bands differ')
    if len(trace) != len(expected):
        raise ValueError('missing encoder trace frames')
    results = []
    for frame, actual in zip(expected, trace):
        index = frame['frame_index']
        for key in ['frame_index', 'fs_rate_limiter', 'previous_sample_bytes', 'quant', 'scale']:
            if actual[key] != frame[key]:
                raise ValueError('frame %d autonomous state/table mismatch: %s; actual=%s expected=%s' % (index, key, actual[key], frame[key]))
        if actual['coefficient_fnv64'] != [b['sdk_coeff_fnv64'] for b in frame['bands']]:
            raise ValueError('frame %d production coefficient mismatch' % index)
        raw = (output/('frame_%04d.cfhd' % index)).read_bytes()
        if cpu_encoder:
            cpu_raw = (cpu_output/('frame_%04d.cfhd' % index)).read_bytes()
            if raw != cpu_raw:
                first = next((i for i, (a, b) in enumerate(zip(cpu_raw, raw)) if a != b), min(len(raw), len(cpu_raw)))
                raise ValueError('frame %d CPU/Metal full packet differs at byte %d' % (index, first))
        parsed = sample.parse_sample(raw)
        normalized = sample.normalized_sample(raw, parsed, frame['metadata_mask_ranges'])
        if len(raw) != frame['sdk_byte_length'] or actual['sample_bytes'] != len(raw):
            raise ValueError('frame %d actual encoded byte count mismatch' % index)
        row = {'frame_index': index, 'input_sha256': frame['input_sha256'],
               'fs_rate_limiter': actual['fs_rate_limiter'], 'previous_sample_bytes': actual['previous_sample_bytes'],
               'quant': actual['quant'], 'scale': actual['scale'], 'sample_bytes': len(raw),
               'raw_sha256': gate.sha(raw), 'normalized_sha256': gate.sha(normalized),
               'coefficient_hashes_equal': 30, 'framed_payload_hashes_equal': 30,
               'autonomous_state': 'PASS', 'full_normalized_sample': 'PASS'}
        if cpu_encoder:
            row['same_input_cpu_packet_sha256'] = gate.sha(cpu_raw)
            row['same_input_full_raw_packet_equal'] = True
        if corpus:
            original = (corpus/'frames'/run_name/('frame_%04d' % index)/'sdk.cfhd').read_bytes()
            gate.verify_bytes(original, frame['sdk_sha256'], frame['sdk_byte_length'], 'original SDK sample')
            reference, reference_normalized = sample.verify_sample(original, frame, 'sdk')
            if normalized != reference_normalized:
                first = next((i for i, (a, b) in enumerate(zip(normalized, reference_normalized)) if a != b), min(len(normalized), len(reference_normalized)))
                offset=first//4*4
                words=(struct.unpack_from('>hH', normalized, offset),struct.unpack_from('>hH', reference_normalized, offset)) if offset+4<=min(len(normalized),len(reference_normalized)) else None
                raise ValueError('frame %d normalized full-sample byte mismatch at %d; actual/expected tag words=%s' % (index, first, words))
            if any(a['framed'] != b['framed'] for a, b in zip(parsed['bands'], reference['bands'])):
                raise ValueError('frame %d actual framed band bytes differ' % index)
            row['actual_normalized_bytes_equal'] = True
            if inject:
                if raw != original:
                    raise ValueError('frame %d identical metadata but original sample bytes differ' % index)
                row['raw_sample_with_explicit_variable_metadata'] = 'PASS'
        sample.verify_sample(raw, frame, 'sdk')
        if ffmpeg and index in (0, 3, 6, 13):
            packet = output/('frame_%04d.avi' % index);packet.write_bytes(avi(raw, 1920, 1080))
            decoded = output/('frame_%04d.yuv' % index)
            subprocess.run([str(ffmpeg), '-v', 'error', '-xerror', '-y', '-i', str(packet), '-frames:v', '1', '-pix_fmt', 'yuv422p10le', '-f', 'rawvideo', str(decoded)], check=True)
            if decoded.stat().st_size != 1920*1080*4:
                raise ValueError('FFmpeg output storage mismatch')
            actual_pixels=array.array('H', decoded.read_bytes());reference_pixels=array.array('H', (output/('frame_%04d.reference.yuv' % index)).read_bytes())
            if sys.byteorder != 'little':
                actual_pixels.byteswap();reference_pixels.byteswap()
            if len(actual_pixels) != len(reference_pixels):
                raise ValueError('reference plane length mismatch')
            square=sum((int(a)-int(b))**2 for a,b in zip(actual_pixels, reference_pixels))
            psnr=10*math.log10(1023.0**2*len(actual_pixels)/square) if square else None
            if psnr is not None and psnr <= 35:
                raise ValueError('FS3 decoded p10 PSNR does not exceed 35 dB: '+str(psnr))
            row['external_ffmpeg'] = 'PASS';row['decoded_sha256'] = gate.sha(decoded.read_bytes())
            row['yuv_p10_psnr_db'] = psnr

        results.append(row)
    return {'profile': 'autonomous FilmScan3 CPU, BGRA8 -> YUV422 p10, GOP1',
            'tables_supplied_to_encoder': False, 'reference_headers_supplied_to_encoder': False,
            'variable_metadata_injected': inject, 'frames': results, 'status': 'PASS'}


def compiler_info():
    configured = os.environ.get('HYPERCINE_TEST_COMPILER')
    if configured:
        return configured
    compiler = shutil.which('c++')
    return (subprocess.check_output([compiler, '--version'], text=True).strip()
            if compiler else 'NOT RECORDED: run through configured CTest')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--probe', required=True, type=Path)
    parser.add_argument('--encoder', required=True, type=Path)
    parser.add_argument('--cpu-encoder', type=Path, help='Also compare every full raw packet using exactly the same input bytes and metadata')
    parser.add_argument('--corpus-root', type=Path)
    parser.add_argument('--sequence', action='store_true')
    parser.add_argument('--ffmpeg', type=Path)
    parser.add_argument('--inject-metadata', action='store_true')
    parser.add_argument('--output-dir', type=Path)
    parser.add_argument('--report', type=Path)
    args = parser.parse_args()
    if args.inject_metadata and not args.corpus_root:
        parser.error('explicit metadata injection requires verified actual originals')
    name = 'phase3a_1080p_120_manifest.json' if args.sequence else 'manifest.json'
    manifest = gate.load_manifest(gate.CORPUS/name)
    if args.corpus_root:
        inventory = gate.inspect_inventory(manifest, args.corpus_root)
        if inventory['status'] != 'PASS':
            raise ValueError('required real binary corpus incomplete')
    else:
        inventory = {'status': 'PENDING', 'meaning': 'manifest hashes only; no actual Windows bytes supplied'}
    with tempfile.TemporaryDirectory(prefix='hypercine-fs3-production-') as tmp:
        directory = args.output_dir or Path(tmp)
        result = compare_run(args.probe, args.encoder, manifest, args.corpus_root, directory, args.ffmpeg, args.inject_metadata, args.cpu_encoder)
    result.update({'copyright': 'Copyright (c) 2026 HyperCine Contributors.', 'license': 'MIT OR Apache-2.0',
                   'manifest': name, 'inventory': inventory, 'machine': platform.platform(), 'architecture': platform.machine(),
                   'encoder_sha256': gate.sha(args.encoder.read_bytes()),
                   'compiler': compiler_info(),
                   'cmake': subprocess.check_output(['cmake', '--version'], text=True).splitlines()[0],
                   'sources': [{'path': name, 'sha256': gate.sha((gate.ROOT/name).read_bytes())}for name in
                               ['include/hypercine/filmscan3.h','src/cpu/filmscan3_frontend.cpp','src/core/filmscan3_encoder.cpp',
                                'src/cpu/wavelet_cpu.cpp','src/cpu/standalone_frontend.cpp','src/core/standalone_encoder.cpp',
                                'src/core/standalone_codebook.h','tools/hypercine_encode_filmscan3.cpp','tests/validate_filmscan3.py']],
                   'command': [sys.executable]+sys.argv,
                   'windows_gopro_decoder': 'PENDING: actual one-worker Windows test required',
                   'unquantized_windows_intermediates': 'PENDING: Windows stage dumps not supplied'})
    if args.cpu_encoder:
        for name in ['src/core/filmscan3_backend.h', 'src/core/filmscan3_backend.cpp',
                     'src/metal/filmscan3.metal', 'src/metal/filmscan3_metal.mm',
                     'tests/metal_audit.h']:
            result['sources'].append({'path': name, 'sha256': gate.sha((gate.ROOT/name).read_bytes())})
        result['same_input_cpu_encoder_sha256'] = gate.sha(args.cpu_encoder.read_bytes())
        result['same_input_full_raw_packets_equal'] = len(result['frames'])
        result['profile'] = 'autonomous FilmScan3 with same-input CPU/Metal packet comparison'
    if args.report:
        args.report.parent.mkdir(parents=True, exist_ok=True);args.report.write_text(json.dumps(result, indent=2)+'\n')
    print(json.dumps({'frames': len(result['frames']), 'autonomous_tables_and_state': 'PASS',
                      'full_normalized_samples': 'PASS', 'actual_windows_bytes': inventory['status'],
                      'explicit_metadata_raw_identity': args.inject_metadata}))


if __name__ == '__main__':
    try:
        main()
    except (ValueError, KeyError, OSError, subprocess.CalledProcessError) as error:
        print('FAIL:', error);raise SystemExit(1)
