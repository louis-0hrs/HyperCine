# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
"""Build or verify the bidirectional compatibility qualification matrix.

This is an evidence recorder, not a codec test. It has two jobs:

1. Inventory the already-committed reference artifacts without moving,
   renaming or rewriting them, and bind every role to byte length and SHA-256.
2. Emit a qualification matrix whose verdicts are mechanically derived from
   measured comparisons, so a row cannot claim identity while carrying
   differing pixels, and a feature without a measured case cannot appear as a
   pass.

Only the Python standard library is required. `--ffmpeg`/`--ffprobe` add live
decoder measurements; without them the matrix records the committed captures
and marks the direction NOT_OBSERVABLE.

Exit 0 means the matrix is internally consistent and its artifact digests
match, not that any decoder is globally correct.
"""
import argparse
import hashlib
import json
import math
import platform
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile

# One source of truth for the admitted domain of this first finite slice
# (issue #381: "existing admitted YUV422 p10 progressive GOP1 cases").
ADMITTED_SCOPE = {
    'pixel_format': 'yuv422p10le',
    'bit_depth': 10,
    'geometry': '128x128',
    'gop': 1,
    'scan': 'progressive',
    'chroma_siting': 'not asserted (no independent capture)',
    'range': 'studio (64-940)',
}
SCOPE_EXEMPT = ('chroma_siting', 'range')

DIRECTIONS = (
    'legacy-gopro-packet-to-hypercine',
    'legacy-gopro-packet-to-external-decoder',
    'legacy-gopro-packet-to-gopro-sdk-decoder',
    'hypercine-packet-to-gopro-sdk-decoder',
    'hypercine-packet-to-external-decoder',
    'hypercine-packet-to-hypercine-decoder',
)

STATUS_IDENTITY = 'QUALIFIED_IDENTITY'
STATUS_DIFFERENCE = 'MEASURED_DIFFERENCE'
STATUS_NOT_OBSERVABLE = 'NOT_OBSERVABLE'
STATUSES = (STATUS_IDENTITY, STATUS_DIFFERENCE, STATUS_NOT_OBSERVABLE)

VERDICT_IDENTICAL = 'IDENTICAL'
VERDICT_DIFFERENT = 'DIFFERENT'
VERDICTS = (VERDICT_IDENTICAL, VERDICT_DIFFERENT)

# 'committed_artifacts' compares two files that are both in the repository, so
# no decoder binary has to be trusted. 'local_decoder' is a measurement made on
# this host and therefore must name the exact executable it used.
BASIS_COMMITTED = 'committed_artifacts'
BASIS_LOCAL = 'local_decoder'
BASES = (BASIS_COMMITTED, BASIS_LOCAL)

COMPARISON_KEYS = ('basis', 'decoder', 'capture', 'against', 'metrics', 'verdict')
METRIC_KEYS = ('psnr_db', 'max_error', 'mismatches', 'samples')
DECODER_KEYS = ('id', 'version', 'binary_sha256')

MAX_ARTIFACT_BYTES = 128 * 1024 * 1024
MAX_MANIFEST_BYTES = 1024 * 1024
FRAME_BYTES = 65536
SAMPLE_COUNT = FRAME_BYTES // 2
HEX64 = re.compile(r'^[0-9a-f]{64}$')
GIT_ID = re.compile(r'^[0-9a-f]{40}([0-9a-f]{24})?$')


def require(ok, reason):
    """Fail closed with a public reason; never leak a private path."""
    if not ok:
        raise ValueError(reason)


def sha256_bytes(payload):
    return hashlib.sha256(payload).hexdigest()


def _relative_parts(relative):
    require(isinstance(relative, str) and relative, 'unsafe artifact name')
    require('\\' not in relative, 'unsafe artifact separator')
    parts = relative.split('/')
    require(all(part not in ('', '.', '..') for part in parts),
            'unsafe artifact component')
    return parts


def bound_artifact(root, relative, declared_bytes, declared_sha):
    """Read one committed artifact once, bounded, and bind bytes to digest."""
    path = root.joinpath(*_relative_parts(relative))
    require(path.is_file() and not path.is_symlink(), 'missing or linked artifact')
    require(type(declared_bytes) is int and 0 < declared_bytes <= MAX_ARTIFACT_BYTES,
            'invalid declared artifact length')
    with path.open('rb') as stream:
        payload = stream.read(declared_bytes + 1)
    require(len(payload) == declared_bytes, 'artifact length mismatch')
    require(sha256_bytes(payload) == declared_sha, 'artifact digest mismatch')
    return payload


def load_manifest(path):
    require(path.is_file() and not path.is_symlink(), 'missing fixture manifest')
    with path.open('rb') as stream:
        payload = stream.read(MAX_MANIFEST_BYTES + 1)
    require(len(payload) <= MAX_MANIFEST_BYTES, 'fixture manifest exceeds size limit')
    return json.loads(payload.decode('utf-8'))


def manifest_files(manifest):
    """Return (name, bytes, sha256) for either manifest shape."""
    records = []
    for entry in manifest.get('files', []):
        records.append((entry.get('file'), entry.get('bytes'), entry.get('sha256')))
    for entry in manifest.get('samples', []):
        records.append((entry.get('file'), entry.get('byte_length'), entry.get('sha256')))
    require(records, 'manifest declares no artifacts')
    for name, size, digest in records:
        require(isinstance(name, str) and name, 'manifest artifact without a name')
        require(isinstance(digest, str) and HEX64.match(digest),
                'manifest artifact without a SHA-256')
    return records


ROLE_SUFFIXES = (
    ('.input-p10', 'encoder_input_p10'),
    ('.sdk-p10', 'gopro_sdk_decoded_p10'),
    ('.sdk-bgra', 'gopro_sdk_decoded_bgra'),
    ('.p10', 'committed_ffmpeg_decoded_p10'),
    ('.bgra', 'committed_ffmpeg_decoded_bgra'),
    ('.cfhd', 'hypercine_packet'),
)


def classify(relative):
    """Map an artifact name to its evidence role; unknown names are refused."""
    name = relative.rsplit('/', 1)[-1]
    if name == 'control.cfhd':
        return 'legacy_gopro_packet'
    for suffix, role in ROLE_SUFFIXES:
        if name.endswith(suffix):
            return role
    raise ValueError('unclassified reference artifact')


def inventory(fixtures):
    """Hash-verify every committed fixture; return role records and payloads."""
    artifacts, payloads = {}, {}
    for group in ('standalone', 'windows-oracle', 'macos-filmscan3'):
        root = fixtures / group
        manifest = load_manifest(root / 'manifest.json')
        commit = manifest.get('encoder_commit') or manifest.get('base_commit')
        for name, size, digest in manifest_files(manifest):
            role = classify(name)
            payload = bound_artifact(root, name, size, digest)
            key = '%s/%s' % (group, name)
            artifacts[key] = {'group': group, 'file': name, 'role': role,
                              'bytes': len(payload), 'sha256': digest,
                              'source_commit': commit}
            payloads[key] = payload
    roles = {art['role'] for art in artifacts.values()}
    require(len(roles) >= 6, 'incomplete artifact role inventory')
    return artifacts, payloads


def required_sha(artifacts, key, role):
    record = artifacts.get(key)
    require(record is not None, 'artifact not inventoried: ' + role)
    require(record['role'] == role, 'artifact role mismatch: ' + role)
    return record['sha256']


def artifact_ref(artifacts, key, role):
    """Reference one inventoried artifact by name; the digest is bound."""
    return {'artifact': key, 'sha256': required_sha(artifacts, key, role)}


def local_ref(name, payload):
    return {'artifact': 'local/ffmpeg-cfhd/' + name,
            'sha256': sha256_bytes(payload)}


def recorded_decoder(identifier, version):
    """A capture recorded in the repository: version string, no binary claim."""
    require(identifier, 'decoder without an identifier')
    require(version and version != 'UNKNOWN_VERSION', 'decoder without a version')
    return {'id': identifier, 'version': version, 'binary_sha256': None}


def bound_decoder(identifier, version, binary_sha):
    """A measurement made here: the exact executable must be named."""
    record = recorded_decoder(identifier, version)
    require(isinstance(binary_sha, str) and HEX64.match(binary_sha),
            'local decoder without a bound executable digest')
    record['binary_sha256'] = binary_sha
    return record


def comparison(basis, decoder, capture, against, metrics):
    require(basis in BASES, 'unknown evidence basis')
    expected = VERDICT_IDENTICAL if metrics['mismatches'] == 0 else VERDICT_DIFFERENT
    require(metrics['max_error'] == 0 if expected == VERDICT_IDENTICAL
            else metrics['max_error'] > 0, 'metrics contradict the error count')
    return {'basis': basis, 'decoder': decoder, 'capture': capture, 'against': against,
            'metrics': metrics, 'verdict': expected}


def metrics(mismatches, max_error, psnr_db, samples):
    return {'psnr_db': psnr_db, 'max_error': max_error,
            'mismatches': mismatches, 'samples': samples}


def metrics_from_result(result, samples):
    overall = result['overall']
    return metrics(overall['mismatches'], overall['max_error'],
                   overall['psnr_db'], SAMPLE_COUNT)


def derive_status(comparisons):
    """A row is only IDENTITY when every single comparison is bit-exact."""
    if not comparisons:
        return STATUS_NOT_OBSERVABLE
    if any(item['verdict'] == VERDICT_DIFFERENT for item in comparisons):
        return STATUS_DIFFERENCE
    return STATUS_IDENTITY


def diff_metrics(left, right, samples):
    """Sample-level comparison of two p10 buffers.

    p10 is 16-bit little-endian, so a byte diff would overstate both the count
    and the magnitude. Errors are measured in code values with the same 1023
    peak the reference tooling uses; no dither or rounding is applied.
    """
    require(len(left) == len(right), 'p10 buffer length differs')
    require(len(left) % 2 == 0, 'p10 buffer is not 16-bit aligned')
    count = len(left) // 2
    require(count == samples, 'unexpected p10 sample count')
    a = struct.unpack('<%dH' % count, left)
    b = struct.unpack('<%dH' % count, right)
    mismatches, maximum, total = 0, 0, 0
    for value, reference in zip(a, b):
        delta = abs(value - reference)
        if delta:
            mismatches += 1
            total += delta * delta
            if delta > maximum:
                maximum = delta
    psnr = None if total == 0 else round(
        10.0 * math.log10((1023.0 * 1023.0) / (total / count)), 6)
    return metrics(mismatches, maximum, psnr, samples)


def live_measurements(fixtures, ffmpeg, ffprobe):
    """Decode the committed packets with one named local FFmpeg build."""
    sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tests'))
    import compare_sdk_oracle as oracle

    with open(ffmpeg, 'rb') as stream:
        digest = hashlib.sha256()
        for block in iter(lambda: stream.read(1 << 20), b''):
            digest.update(block)
    binary_sha = digest.hexdigest()
    version = oracle.validated_ffmpeg_version(
        subprocess.check_output([ffmpeg, '-version'], text=True,
                                stderr=subprocess.DEVNULL).splitlines()[0])
    identity = bound_decoder('ffmpeg-cfhd', version, binary_sha)
    workspace = tempfile.mkdtemp(prefix='hypercine-compat-')
    results = {}
    try:
        for group, name in (('standalone', 'flat.cfhd'),
                            ('standalone', 'canonical.cfhd'),
                            ('standalone', 'gradient.cfhd'),
                            ('windows-oracle', 'control.cfhd')):
            packet = (fixtures / group / name).read_bytes()
            container = Path(workspace) / (name + '.avi')
            container.write_bytes(oracle.avi(packet))
            stream_row = json.loads(subprocess.check_output(
                [ffprobe, '-v', 'error', '-show_streams', '-of', 'json',
                 str(container)], stderr=subprocess.DEVNULL))['streams'][0]
            require((stream_row['codec_name'], stream_row['width'],
                     stream_row['height'], stream_row['pix_fmt']) ==
                    ('cfhd', 128, 128, 'yuv422p10le'),
                    'unexpected decoder stream geometry')
            decoded = Path(workspace) / (name + '.p10')
            subprocess.run([ffmpeg, '-v', 'error', '-xerror', '-n', '-i',
                            str(container), '-frames:v', '1', '-c:v', 'rawvideo',
                            '-pix_fmt', 'yuv422p10le', '-f', 'rawvideo',
                            str(decoded)], check=True, stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL)
            with decoded.open('rb') as source:
                payload = source.read(FRAME_BYTES + 1)
            require(len(payload) == FRAME_BYTES, 'invalid decoded frame size')
            results['%s/%s' % (group, name)] = (identity, payload)
    finally:
        shutil.rmtree(workspace, ignore_errors=True)
    return results


def _packet_rows(artifacts, payloads, pattern, live):
    packet = 'standalone/%s.cfhd' % pattern
    source_key = 'standalone/%s.input-p10' % pattern
    committed_key = 'standalone/%s.p10' % pattern
    sdk_key = 'windows-oracle/%s.sdk-p10' % pattern
    comparisons = []
    source = artifact_ref(artifacts, source_key, 'encoder_input_p10')
    committed = artifact_ref(artifacts, committed_key,
                             'committed_ffmpeg_decoded_p10')
    sdk_p10 = artifact_ref(artifacts, sdk_key, 'gopro_sdk_decoded_p10')
    # Both sides live in the repository, so this needs no trusted binary.
    comparisons.append(comparison(
        BASIS_COMMITTED, recorded_decoder('ffmpeg-cfhd', 'n8.0 140fd653'),
        committed, source,
        diff_metrics(payloads[committed_key], payloads[source_key], SAMPLE_COUNT)))
    comparisons.append(comparison(
        BASIS_COMMITTED, recorded_decoder('gopro-sdk-decoder', 'pristine TAG_CPU_MAX=1'),
        sdk_p10, source,
        diff_metrics(payloads[sdk_key], payloads[source_key], SAMPLE_COUNT)))
    live_record = (live or {}).get('standalone/' + pattern + '.cfhd')
    if live_record is not None:
        identity, payload = live_record
        captured = local_ref(pattern + '.p10', payload)
        comparisons.append(comparison(
            BASIS_LOCAL, identity, captured, committed,
            diff_metrics(payload, payloads[committed_key], SAMPLE_COUNT)))
        comparisons.append(comparison(
            BASIS_LOCAL, identity, captured, source,
            diff_metrics(payload, payloads[source_key], SAMPLE_COUNT)))
        comparisons.append(comparison(
            BASIS_LOCAL, identity, captured, sdk_p10,
            diff_metrics(payload, payloads[sdk_key], SAMPLE_COUNT)))
    return {
        'id': 'hypercine-packet-' + pattern,
        'packet': {'artifact': packet,
                   'sha256': required_sha(artifacts, packet, 'hypercine_packet'),
                   'role': 'hypercine_packet',
                   'producer': 'HyperCine standalone CPU encoder',
                   'source_commit': artifacts[packet]['source_commit']},
        'direction': 'hypercine-packet-to-external-decoder',
        'scope': dict(ADMITTED_SCOPE),
        'comparisons': comparisons,
        'status': derive_status(comparisons),
        'limitations': ['Synthetic 128x128 pattern. A match bounds this packet, '
                        'this geometry and these decoder builds only.'],
    }


def _control_row(artifacts, payloads, live):
    packet = 'windows-oracle/control.cfhd'
    source = artifact_ref(artifacts, 'standalone/canonical.input-p10',
                          'encoder_input_p10')
    sdk_p10 = artifact_ref(artifacts, 'windows-oracle/control.sdk-p10',
                          'gopro_sdk_decoded_p10')
    comparisons = [comparison(
        BASIS_COMMITTED, recorded_decoder('gopro-sdk-decoder',
                                          'pristine TAG_CPU_MAX=1'),
        sdk_p10, source,
        diff_metrics(payloads['windows-oracle/control.sdk-p10'],
                     payloads['standalone/canonical.input-p10'], SAMPLE_COUNT))]
    live_record = (live or {}).get('windows-oracle/control.cfhd')
    if live_record is not None:
        identity, payload = live_record
        comparisons.append(comparison(
            BASIS_LOCAL, identity, local_ref('control.p10', payload), sdk_p10,
            diff_metrics(payload, payloads['windows-oracle/control.sdk-p10'],
                         SAMPLE_COUNT)))
    return {
        'id': 'legacy-gopro-packet-control',
        'packet': {'artifact': packet,
                   'sha256': required_sha(artifacts, packet, 'legacy_gopro_packet'),
                   'role': 'legacy_gopro_packet',
                   'producer': 'legacy GoPro encoder (control packet)',
                   'source_commit': None},
        'direction': 'legacy-gopro-packet-to-gopro-sdk-decoder',
        'scope': dict(ADMITTED_SCOPE),
        'comparisons': comparisons,
        'status': derive_status(comparisons),
        'limitations': ['One frozen control packet. It is not a corpus and does '
                        'not qualify other GoPro bitstreams or other decoders.'],
    }


def _metal_row(artifacts):
    """Metal-written packets have no decoded-pixel capture in this repository."""
    return {
        'id': 'hypercine-metal-filmscan3-packets',
        'packet': {'artifact': 'macos-filmscan3/flat.cfhd',
                   'sha256': required_sha(artifacts, 'macos-filmscan3/flat.cfhd',
                                          'hypercine_packet'),
                   'role': 'hypercine_packet',
                   'producer': 'HyperCine Metal FilmScan3 encoder (macOS)',
                   'source_commit': artifacts[
                       'macos-filmscan3/flat.cfhd']['source_commit']},
        'direction': 'hypercine-packet-to-external-decoder',
        'scope': dict(ADMITTED_SCOPE, geometry='1920x1080'),
        'comparisons': [],
        'status': STATUS_NOT_OBSERVABLE,
        'limitations': ['Packets are committed but no decoded-pixel capture and '
                        'no independent pixel expectation exist in Git, so no '
                        'compatibility verdict is possible here.'],
    }


def backlog_rows():
    """Declared-unsupported features: enumerated, never a pass."""
    reason = 'no measured case in this repository'
    return [
        {'feature': 'RGB(A) source or 4:4:4 encoded family', 'reason': reason,
         'tracked_by': '#381, #259'},
        {'feature': 'RGB48 / B64A high-bit-depth input', 'reason': reason,
         'tracked_by': '#381'},
        {'feature': 'Bayer (BYR2/BYR4) source family', 'reason': reason,
         'tracked_by': '#381'},
        {'feature': 'Interlaced scan', 'reason': reason, 'tracked_by': '#381'},
        {'feature': 'Temporal GOP > 1', 'reason': reason,
         'tracked_by': '#381, tests/reference_gop2'},
        {'feature': 'Stereo / multi-eye temporal layout', 'reason': reason,
         'tracked_by': '#381'},
        {'feature': 'Active metadata beyond masked volatile tags',
         'reason': reason, 'tracked_by': '#381'},
        {'feature': 'GoPro encoder byte identity',
         'reason': 'not a product objective since #382', 'tracked_by': '#382'},
    ]


def build_matrix(artifacts, payloads, live=None):
    rows = [_packet_rows(artifacts, payloads, pattern, live)
            for pattern in ('flat', 'canonical', 'gradient')]
    rows.append(_control_row(artifacts, payloads, live))
    rows.append(_metal_row(artifacts))
    context = None
    if live:
        context = {'host': platform.platform(),
                   'command': 'tools/compat_qualification.py --ffmpeg FFMPEG '
                              '--ffprobe FFPROBE --out tests/compatibility/matrix.json'}
    return {'schema': 'hypercine.compat-matrix/1',
            'generated_by': 'tools/compat_qualification.py',
            'measurement_context': context,
            'admitted_scope': dict(ADMITTED_SCOPE),
            'rows': rows, 'backlog': backlog_rows()}


def validate_matrix(matrix, fixtures=None):
    require(matrix.get('schema') == 'hypercine.compat-matrix/1',
            'unsupported matrix schema')
    context = matrix.get('measurement_context')
    require(context is None or (isinstance(context, dict) and
                                set(context) == {'host', 'command'}),
            'malformed measurement context')
    rows = matrix.get('rows')
    require(isinstance(rows, list) and rows, 'matrix has no rows')
    seen = set()
    for row in rows:
        require(set(row) == {'id', 'packet', 'direction', 'scope', 'comparisons',
                             'status', 'limitations'}, 'malformed matrix row')
        require(row['id'] and row['id'] not in seen, 'duplicate matrix row id')
        seen.add(row['id'])
        require(row['direction'] in DIRECTIONS, 'unknown compatibility direction')
        require(row['status'] in STATUSES, 'unknown qualification status')
        require(isinstance(row['scope'], dict), 'row without a scope')
        _validate_comparisons(row)
        _validate_status_scope(row)
        if fixtures is not None:
            _reverify_packet(row, fixtures)
    _validate_backlog(matrix.get('backlog'))
    return matrix


def _validate_comparisons(row):
    comparisons = row['comparisons']
    require(isinstance(comparisons, list), 'comparisons must be a list')
    for item in comparisons:
        require(set(item) == set(COMPARISON_KEYS), 'malformed comparison record')
        require(item['basis'] in BASES, 'unknown evidence basis')
        require(item['verdict'] in VERDICTS, 'unknown comparison verdict')
        decoder = item['decoder']
        require(set(decoder) == set(DECODER_KEYS), 'malformed decoder record')
        require(isinstance(decoder['id'], str) and decoder['id'],
                'comparison decoder without an id')
        require(isinstance(decoder['version'], str) and decoder['version'],
                'comparison decoder without a version')
        bound = decoder['binary_sha256']
        if item['basis'] == BASIS_LOCAL:
            require(isinstance(bound, str) and HEX64.match(bound),
                    'local measurement without a bound decoder executable')
        else:
            require(bound is None,
                    'committed-artifact comparison must not claim a binary digest')
        for field in ('capture', 'against'):
            reference = item[field]
            require(set(reference) == {'artifact', 'sha256'},
                    'malformed artifact reference')
            require(isinstance(reference['artifact'], str) and reference['artifact'],
                    'artifact reference without a name')
            require(HEX64.match(reference['sha256']),
                    'artifact reference without a bound digest')
        require(item['capture']['artifact'] != item['against']['artifact'],
                'comparison compares an artifact with itself')
        metrics = item['metrics']
        require(set(metrics) == set(METRIC_KEYS), 'malformed metrics record')
        for field in ('max_error', 'mismatches', 'samples'):
            require(type(metrics[field]) is int and metrics[field] >= 0,
                    'invalid metric value: ' + field)
        require(metrics['samples'] > 0, 'comparison without samples')
        if item['verdict'] == VERDICT_IDENTICAL:
            require(metrics['mismatches'] == 0 and metrics['max_error'] == 0,
                    'IDENTICAL verdict contradicts the measured error')
        else:
            require(metrics['mismatches'] > 0 and metrics['max_error'] > 0,
                    'DIFFERENT verdict without a measured difference')
    require(row['status'] == derive_status(comparisons),
            'row status contradicts its comparisons')


def _validate_status_scope(row):
    """An identity claim needs a bound capture and the admitted domain."""
    if row['status'] != STATUS_IDENTITY:
        return
    require(row['comparisons'], 'identity claim without a comparison')
    scope = row['scope']
    for field, expected in ADMITTED_SCOPE.items():
        if field in SCOPE_EXEMPT:
            continue
        require(scope.get(field) == expected,
                'identity claim outside the admitted scope: ' + field)


def _reverify_packet(row, fixtures):
    packet = row['packet']
    require(set(packet) == {'artifact', 'sha256', 'role', 'producer',
                            'source_commit'}, 'malformed packet record')
    require(packet['source_commit'] is None or
            GIT_ID.match(packet['source_commit']),
            'packet source commit is not a digest')
    group, name = packet['artifact'].split('/', 1)
    path = (fixtures / group).joinpath(*_relative_parts(name))
    require(path.is_file() and not path.is_symlink(), 'missing packet artifact')
    bound_artifact(fixtures / group, name, path.stat().st_size, packet['sha256'])


def _validate_backlog(backlog):
    require(isinstance(backlog, list) and backlog, 'backlog must be enumerated')
    for item in backlog:
        require(set(item) == {'feature', 'reason', 'tracked_by'},
                'backlog entry carries unexpected fields')
        require(item['feature'] and item['reason'], 'backlog entry without a reason')


def markdown(matrix):
    lines = ['| row | direction | decoder | basis | against | verdict | max err '
             '| mismatches | PSNR dB |',
             '| :-- | :-- | :-- | :-- | :-- | :-- | --: | --: | --: |']
    for row in matrix['rows']:
        if not row['comparisons']:
            lines.append('| %s | %s | n/a | n/a | n/a | %s | - | - | - |' % (
                row['id'], row['direction'], row['status']))
        for item in row['comparisons']:
            stats = item['metrics']
            psnr = ('exact' if item['verdict'] == VERDICT_IDENTICAL
                    else ('%.4f' % stats['psnr_db'] if stats['psnr_db'] is not None
                          else 'n/a'))
            lines.append('| %s | %s | %s %s | %s | %s | %s | %d | %d/%d | %s |' % (
                row['id'], row['direction'], item['decoder']['id'],
                item['decoder']['version'], item['basis'],
                item['against']['artifact'], item['verdict'], stats['max_error'],
                stats['mismatches'], stats['samples'], psnr))
    lines.append('')
    lines.append('| backlog feature | reason | tracked by |')
    lines.append('| :-- | :-- | :-- |')
    for item in matrix['backlog']:
        lines.append('| %s | %s | %s |' % (item['feature'], item['reason'],
                                           item['tracked_by']))
    return '\n'.join(lines)


def main(argv=None):
    parser = argparse.ArgumentParser(description='Compatibility qualification matrix')
    parser.add_argument('--fixtures', default=None,
                        help='fixture root (default: tests/fixtures)')
    parser.add_argument('--out', default=None, help='write the matrix JSON here')
    parser.add_argument('--verify', default=None,
                        help='verify an existing matrix against the fixtures')
    parser.add_argument('--markdown', action='store_true',
                        help='print the matrix as markdown tables')
    parser.add_argument('--ffmpeg', default=None, help='FFmpeg executable')
    parser.add_argument('--ffprobe', default=None, help='ffprobe executable')
    args = parser.parse_args(argv)

    root = Path(args.fixtures) if args.fixtures else \
        Path(__file__).resolve().parents[1] / 'tests' / 'fixtures'
    try:
        if args.verify:
            with open(args.verify, 'r', encoding='utf-8') as stream:
                matrix = json.load(stream)
            validate_matrix(matrix, root)
            print('matrix verified: %d rows, %d backlog entries' %
                  (len(matrix['rows']), len(matrix['backlog'])))
            return 0
        artifacts, payloads = inventory(root)
        live = None
        if args.ffmpeg:
            require(args.ffprobe, '--ffmpeg requires --ffprobe')
            live = live_measurements(root, args.ffmpeg, args.ffprobe)
        matrix = build_matrix(artifacts, payloads, live)
        validate_matrix(matrix)
        if args.out:
            with open(args.out, 'w', encoding='utf-8') as stream:
                json.dump(matrix, stream, indent=2, sort_keys=True)
                stream.write('\n')
        if args.markdown:
            print(markdown(matrix))
        elif not args.out:
            print(json.dumps(matrix, indent=2, sort_keys=True))
        return 0
    except (ValueError, KeyError, OSError) as error:
        print('compat qualification failed: %s' % error, file=sys.stderr)
        return 2


if __name__ == '__main__':
    sys.exit(main())
