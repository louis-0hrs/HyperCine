# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
"""Original six-pair S3 graph replay, not a CFHD frame-decoder qualification."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

import validate_decode_spatial_scalar as original

WORKERS = (1, 2, 4, 16)


def require(ok, message):
    if not ok:
        raise ValueError(message)


def execute(probe, case, files, output, workers, mode):
    stage = case['inputs'][0]['stage']
    result = subprocess.run([str(probe), '--replay', str(stage['width']), str(stage['height']),
        *[str(p) for p, _ in files[:4]], str(case['sdk_channel']), str(output), str(workers), mode],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=30)
    if mode == 'normal':
        require(result.returncode == 0 and b'S3_GRAPH_OUTPUT_ONLY drained=1' in result.stdout,
                'graph replay failed')
        actual = output / 'stage.i16le'
        require(original.digest(actual) == files[4][1], 'original stage hash/length mismatch')
        require(actual.read_bytes() == files[4][0].read_bytes(), 'original stage byte mismatch')
    else:
        require(result.returncode == 2 and
                b'S3_IDENTITY_REJECTED no_output_access=1 drained=1' in result.stdout,
                'identity control did not produce its required rejection')
        if mode == 'wrong-channel':
            channel = case['sdk_channel']
            foreign = 1 if channel == 0 else 3 - channel
            require(('source_channel=' + str(foreign)).encode() in result.stdout,
                    'wrong-channel control used an unexpected identity')
        require(not output.exists(), 'identity control published output')
    return result.returncode


def replay(root, probe):
    # Reuse the existing original-sidecar/source/packet identity verifier.
    # Authenticate all 30 files and 60 sidecars before ANY probe execution.
    pins = original.read_json(original.PINS)
    cases = original.originals(root, pins)
    rows = []
    with tempfile.TemporaryDirectory(prefix='hypercine-s3-original-') as temporary:
        work = Path(temporary)
        for n, (case, files) in enumerate(cases):
            for workers in WORKERS:
                row = {'packet_geometry': case['packet_geometry'], 'sdk_channel': case['sdk_channel'],
                       'workers': workers, 'expected_sha256': files[4][1]['sha256'],
                       'expected_bytes': files[4][1]['byte_length'],
                       'wrong_channel_source': 1 if case['sdk_channel'] == 0 else 3 - case['sdk_channel']}
                row['positive_exit'] = execute(probe, case, files, work / f'{n}-{workers}', workers, 'normal')
                row['identity_controls'] = {}
                for mode in ('wrong-band', 'wrong-channel'):
                    row['identity_controls'][mode] = execute(
                        probe, case, files, work / f'{n}-{workers}-{mode}', workers, mode)
                row['byte_exact'] = True
                rows.append(row)
        # Re-authenticate sidecars as well as raw originals after all executions.
        after = original.originals(root, pins)
        require([[m for _, m in f] for _, f in after] == [[m for _, m in f] for _, f in cases],
                'originals changed during graph replay')
    return {'copyright': 'Copyright (c) 2026 HyperCine Contributors. Dual-licensed MIT OR Apache-2.0.',
            'status': 'ORIGINAL_STAGE_S3_GRAPH_REPLAY_PASS', 'stage_only': True,
            'frame_decoder_qualified': False, 'graph_profile': 'Synthetic',
            'original_cases': 6, 'original_artifacts': 30, 'json_sha_sidecars': 60,
            'originals_unchanged': True, 'positive_comparisons': len(rows),
            'identity_rejections': len(rows) * 2, 'observations': rows,
            'pins_sha256': hashlib.sha256(original.PINS.read_bytes()).hexdigest(),
            'probe_sha256': hashlib.sha256(probe.read_bytes()).hexdigest()}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--probe', required=True, type=Path)
    p.add_argument('--golden-dir', type=Path)
    p.add_argument('--output', type=Path, help='optional new report outside the corpus; otherwise JSON on stdout')
    a = p.parse_args()
    try:
        bound = a.golden_dir or os.environ.get('HYPERCINE_GOLDEN_DIR')
        require(bool(bound), 'original corpus binding missing')
        root = Path(bound).resolve()
        output = a.output.resolve() if a.output else None
        if output is not None:
            require(output != root and root not in output.parents and not output.exists(),
                    'unsafe or existing report destination')
        report = replay(root, a.probe.resolve())
        if output is not None:
            with output.open('x', encoding='utf-8') as stream:
                json.dump(report, stream, indent=2, sort_keys=True)
                stream.write('\n')
        print(json.dumps(report, sort_keys=True))
        return 0
    except (OSError, ValueError, KeyError, TypeError, StopIteration, subprocess.SubprocessError):
        print('S3 original replay BLOCKED or MISMATCH; no pass report; details redacted', file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
