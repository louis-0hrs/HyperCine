# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
"""Record one bounded decoded-pixel capture of the committed Metal FilmScan3 packet.

This is the finite measurement named in issue #381 after the compatibility
matrix became available: the `hypercine-metal-filmscan3-packets` row carried a
committed 1920x1080 packet but no decoded-pixel capture, so it could only be
`NOT_OBSERVABLE`.

What this tool actually does:

1. Binds the committed packet by byte length and SHA-256 from its fixture
   manifest; the packet itself is never moved, renamed or rewritten.
2. Wraps that packet in the already-reviewed single-frame AVI container and
   decodes exactly one frame to `yuv422p10le` with a decoder whose executable
   SHA-256 and version token are recorded, never the build banner or a path.
3. Repeats the decode in independent processes, including explicit thread
   settings, and requires byte-identical output before it reports a capture.
4. Binds the capture by length, sample count, digest and an explicit plane
   layout: widths, heights, byte strides and offsets for 4:2:2 10-bit planar
   storage, plus the declared bit depth, sample endianness, range and the fact
   that chroma siting is not asserted by this packet.
5. Executes two real negative controls: a one-sample change must be detected
   with the exact expected metrics, and a forged metrics record that claims
   identity for differing buffers must be rejected.
6. Compares the capture against the already-committed macOS decode digest for
   the same packet and frame.

What this is **not**: a compatibility verdict. There is no committed encoder
input (`macos-filmscan3/flat.input-p10`, SHA-256 `760ea8d6…`) and no committed
macOS capture bytes in this repository, so no sample-level difference against a
source pattern can be recomputed here. The matrix row therefore stays
`NOT_OBSERVABLE`; reproducing the recorded macOS digest proves decoder agreement
on these exact packet bytes, which is a narrower statement.

`--verify` is hash-only and needs no decoder: it re-checks the committed
evidence file against the fixture manifest and the retained macOS evidence, so
an edited claim cannot drift away from the artifacts it names.

Only the Python standard library is required.
"""
import argparse
import array
import hashlib
import json
import math
import os
import platform
from pathlib import Path
import struct
import shutil
import sys
import tempfile

from compat_process import run_bounded

SCHEMA = 'hypercine.metal-packet-capture/2'
LEGACY_SCHEMA = 'hypercine.metal-packet-capture/1'
ROOT = Path(__file__).resolve().parents[1]
PACKET_ARTIFACT = 'macos-filmscan3/flat.cfhd'
MANIFEST_ARTIFACT = 'tests/fixtures/macos-filmscan3/manifest.json'
MACOS_EVIDENCE_ARTIFACT = 'docs/validation/metal-filmscan3-ffmpeg.json'
WIDTH, HEIGHT = 1920, 1080
FRAME_INDEX = 0
PIXEL_FORMAT = 'yuv422p10le'
BIT_DEPTH = 10
PEAK = 1023
DECODE_TIMEOUT = 60.0
CAPTURE_BYTES = WIDTH * HEIGHT * 4
SAMPLE_COUNT = WIDTH * HEIGHT * 2
# Independent one-frame decodes with explicit input-decoder settings. This is
# configuration coverage, not multiframe concurrency stress. Filter threads are
# a separate global option, never a decoder-worker setting.
DECODE_VARIANTS = (
    ('default', ()),
    ('single-thread', ('-threads', '1')),
    ('four-thread', ('-threads', '4')),
    ('single-filter-thread', ('-filter_threads', '1')),
)


def require(condition, message):
    if not condition:
        raise ValueError('metal capture: ' + message)


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def is_digest(value):
    return (type(value) is str and len(value) == 64 and
            all(char in '0123456789abcdef' for char in value))


def plane_layout(width, height):
    """Tightly packed planar 4:2:2 with 16-bit storage: 4 bytes per pixel."""
    require(type(width) is int and type(height) is int, 'geometry must be integers')
    require(width > 0 and height > 0, 'geometry must be positive')
    require(width % 2 == 0, '4:2:2 requires an even width')
    chroma_width = width // 2
    luma = width * height * 2
    chroma = chroma_width * height * 2
    return [
        {'plane': 'y', 'width': width, 'height': height,
         'stride_bytes': width * 2, 'offset_bytes': 0, 'byte_length': luma},
        {'plane': 'u', 'width': chroma_width, 'height': height,
         'stride_bytes': chroma_width * 2, 'offset_bytes': luma,
         'byte_length': chroma},
        {'plane': 'v', 'width': chroma_width, 'height': height,
         'stride_bytes': chroma_width * 2, 'offset_bytes': luma + chroma,
         'byte_length': chroma},
    ]


def sample_view(payload):
    """Validate tightly packed p10 storage and return unsigned samples."""
    require(len(payload) % 4 == 0, 'p10 storage length mismatch')
    samples = array.array('H', payload)
    if sys.byteorder != 'little':
        samples.byteswap()
    require(all(value <= PEAK for value in samples), 'p10 sample outside 0..1023')
    return samples


def diff_metrics(left, right, samples=SAMPLE_COUNT):
    """Sample-level comparison in code values, never bytes (peak 1023)."""
    require(len(left) == len(right), 'p10 buffer length differs')
    require(len(left) % 2 == 0, 'p10 buffer is not 16-bit aligned')
    count = len(left) // 2
    require(count == samples, 'unexpected p10 sample count')
    a = sample_view(left)
    b = sample_view(right)
    mismatches, maximum, total = 0, 0, 0
    for value, reference in zip(a, b):
        delta = abs(value - reference)
        if delta:
            mismatches += 1
            total += delta * delta
            if delta > maximum:
                maximum = delta
    psnr = None if total == 0 else round(
        10.0 * math.log10((PEAK * PEAK) / (total / count)), 6)
    return {'mismatches': mismatches, 'max_error': maximum,
            'samples': count, 'psnr_db': psnr}


def one_sample_change(payload):
    """Deterministic changed capture: raise the first sample by one code value."""
    require(len(payload) == CAPTURE_BYTES, 'changed capture needs a full frame')
    changed = bytearray(payload)
    original = struct.unpack('<H', changed[0:2])[0]
    require(original < PEAK, 'cannot raise a saturated first sample')
    changed[0:2] = struct.pack('<H', original + 1)
    return bytes(changed)


def require_metrics(left, right, record):
    """Recompute a metrics record and refuse it when the claim does not hold.

    A record is accepted only when every field matches the measured values for
    these two buffers: a forged all-zero identity claim for differing pixels is
    rejected instead of being published.
    """
    actual = diff_metrics(left, right, SAMPLE_COUNT)
    require(type(record) is dict and set(record) == set(actual),
            'malformed metrics record')
    for field in ('mismatches', 'max_error', 'samples'):
        require(type(record[field]) is int and not isinstance(record[field], bool),
                'malformed metric value')
        require(record[field] == actual[field], 'metrics record contradicts the buffers')
    require(record['psnr_db'] == actual['psnr_db'],
            'metrics record contradicts the buffers')
    return actual


def _decoder_version(line):
    import re
    require(type(line) is str, 'invalid decoder version output')
    match = re.match(r'^ffmpeg version (n?[0-9]{1,3}(?:[.][0-9]{1,3}){1,3}'
                     r'(?:[-+][A-Za-z0-9_.+-]{1,32})?)(?=\s|$)', line)
    require(match is not None, 'invalid decoder version output')
    return 'ffmpeg version ' + match.group(1)


def decoder_identity(ffmpeg):
    """Bind the exact executable, not a build banner."""
    digest = hashlib.sha256()
    with open(ffmpeg, 'rb') as stream:
        for block in iter(lambda: stream.read(1 << 20), b''):
            digest.update(block)
    version = _decoder_version(
        run_bounded([ffmpeg, '-version'], stdout_limit=65536).decode('utf-8').splitlines()[0])
    return {'id': 'ffmpeg-cfhd', 'version': version,
            'binary_sha256': digest.hexdigest()}


def decode_command(ffmpeg, container, extra):
    # FFmpeg scopes codec options to the next input/output file. In particular,
    # -threads after -i would configure rawvideo encoding instead of CFHD decode.
    return ([ffmpeg, '-v', 'error', '-xerror'] + list(extra) +
            ['-i', str(container), '-frames:v', '1', '-c:v', 'rawvideo',
             '-threads:v', '1', '-pix_fmt', PIXEL_FORMAT, '-f', 'rawvideo', 'pipe:1'])


def recorded_commands():
    """Portable command templates, with no executable or temporary host paths."""
    return [{'variant': name, 'argv': decode_command('ffmpeg', 'frame.avi', extra)}
            for name, extra in DECODE_VARIANTS]


def _decode(ffmpeg, container, extra):
    payload = run_bounded(
        decode_command(ffmpeg, container, extra),
        timeout=DECODE_TIMEOUT, stdout_limit=CAPTURE_BYTES + 1)
    require(len(payload) == CAPTURE_BYTES, 'invalid decoded frame size')
    return payload


def _stream_identity(ffprobe, container):
    import json as _json
    row = _json.loads(run_bounded(
        [ffprobe, '-v', 'error', '-select_streams', 'v:0', '-show_streams',
         '-of', 'json', str(container)], stdout_limit=65536))['streams'][0]
    require((row.get('codec_name'), row.get('width'), row.get('height'),
             row.get('pix_fmt')) == ('cfhd', WIDTH, HEIGHT, PIXEL_FORMAT),
            'unexpected decoder stream geometry or pixel format')
    return {'codec_name': 'cfhd', 'width': WIDTH, 'height': HEIGHT,
            'pixel_format': PIXEL_FORMAT}


def decode_capture(packet, ffmpeg, ffprobe):
    """Decode one frame under every registered variant, requiring identity."""
    sys.path.insert(0, str(ROOT / 'tests'))
    from validate_standalone import avi

    container_dir = Path(tempfile.mkdtemp(prefix='hypercine-metal-capture-'))
    try:
        container = container_dir / 'frame.avi'
        container.write_bytes(avi(packet, WIDTH, HEIGHT))
        stream = _stream_identity(ffprobe, container)
        digests, lengths = [], []
        for _name, extra in DECODE_VARIANTS:
            payload = _decode(ffmpeg, container, extra)
            digests.append(sha256(payload))
            lengths.append(len(payload))
    finally:
        shutil.rmtree(container_dir, ignore_errors=True)
    require(len(set(digests)) == 1, 'independent decodes disagree')
    require(len(set(lengths)) == 1 and lengths[0] == CAPTURE_BYTES,
            'independent decode lengths disagree')
    return payload, {'runs': len(digests), 'variants': [name for name, _ in DECODE_VARIANTS],
                     'commands': recorded_commands(),
                     'distinct_digests': 1, 'identical': True,
                     'digest': digests[0]}, stream


def load_manifest(path):
    manifest = json.loads(Path(path).read_text(encoding='utf-8'))
    samples = [item for item in manifest.get('samples', [])
               if item.get('file') == Path(PACKET_ARTIFACT).name]
    require(len(samples) == 1, 'packet is not uniquely pinned by its manifest')
    sample = samples[0]
    require(is_digest(sample.get('sha256')), 'manifest packet digest is malformed')
    require(type(sample.get('byte_length')) is int, 'manifest packet length is malformed')
    require((sample.get('width'), sample.get('height')) == (WIDTH, HEIGHT),
            'manifest geometry is not the registered capture geometry')
    return sample


def macos_recorded_digest(path):
    """The retained macOS evidence for the same packet and frame index."""
    evidence = json.loads(Path(path).read_text(encoding='utf-8'))
    frames = [frame for frame in evidence.get('frames', [])
              if frame.get('frame_index') == FRAME_INDEX]
    require(len(frames) == 1, 'macOS evidence does not pin the frame uniquely')
    return frames[0]


def build_evidence(packet_path, ffmpeg, ffprobe, manifest_path, macos_path):
    packet = Path(packet_path).read_bytes()
    manifest = load_manifest(manifest_path)
    require(len(packet) == manifest['byte_length'], 'packet length contradicts its manifest')
    require(sha256(packet) == manifest['sha256'], 'packet digest contradicts its manifest')

    identity = decoder_identity(ffmpeg)
    capture, stability, stream = decode_capture(packet, ffmpeg, ffprobe)

    macos = macos_recorded_digest(macos_path)
    require(macos.get('raw_sha256') == manifest['sha256'],
            'retained macOS evidence belongs to another packet')
    require(is_digest(macos.get('decoded_sha256')), 'macOS evidence has no decode digest')
    # The record is specifically decoder agreement, not a difference report.
    # Never publish match=False as a successfully measured agreement.
    require(stability['digest'] == macos['decoded_sha256'],
            'decoded pixels disagree with retained macOS evidence')

    changed = one_sample_change(capture)
    changed_metrics = diff_metrics(changed, capture)
    require((changed_metrics['mismatches'], changed_metrics['max_error']) == (1, 1),
            'changed capture was not detected with the expected metrics')
    forged = {'mismatches': 0, 'max_error': 0, 'samples': SAMPLE_COUNT, 'psnr_db': None}
    rejected = False
    try:
        require_metrics(changed, capture, forged)
    except ValueError:
        rejected = True
    require(rejected, 'forged metrics record was not rejected')

    return {
        'schema': SCHEMA,
        'issue': '#381',
        'question': ('Decode the committed 1920x1080 Metal FilmScan3 packet to '
                     '10-bit planar pixels on Linux and bind the result to the '
                     'retained macOS decode digest.'),
        'packet': {'artifact': PACKET_ARTIFACT, 'sha256': manifest['sha256'],
                   'byte_length': manifest['byte_length'],
                   'producer': 'HyperCine Metal FilmScan3 encoder (macOS)',
                   'frame_index': FRAME_INDEX},
        'decoder': dict(identity, host=platform.system() + ' ' + platform.machine()),
        'container': {'kind': 'single-frame AVI wrapper of the raw CFHD packet',
                      'codec': stream['codec_name'],
                      'width': stream['width'], 'height': stream['height'],
                      'pixel_format': stream['pixel_format']},
        'capture': {'sha256': stability['digest'], 'byte_length': CAPTURE_BYTES,
                    'samples': SAMPLE_COUNT, 'pixel_format': PIXEL_FORMAT,
                    'bit_depth': BIT_DEPTH},
        'interpretation': {
            'storage': '16-bit little-endian per sample, tightly packed planes',
            'sample_domain': '0..1023 code values, peak 1023 for error metrics',
            'range': 'studio (64-940)',
            'chroma_siting': 'not asserted by this packet',
            'planes': plane_layout(WIDTH, HEIGHT),
        },
        'stability': stability,
        'compared': {
            'against': 'retained macOS external-FFmpeg decode of the same packet and frame',
            'source': MACOS_EVIDENCE_ARTIFACT,
            'frame_index': FRAME_INDEX,
            'macos_decoded_sha256': macos['decoded_sha256'],
            'match': stability['digest'] == macos['decoded_sha256'],
        },
        'negatives': {
            'changed_capture': {'applied': 'first sample raised by one code value',
                                'mismatches': changed_metrics['mismatches'],
                                'max_error': changed_metrics['max_error'],
                                'detected': True},
            'forged_metrics': {'claim': 'identity (0 mismatches, 0 maximum error)',
                               'rejected': rejected},
        },
        'limits': [
            'One packet, one frame, one decoder executable and one host.',
            'Requested input-decoder thread settings are exercised on one frame; '
            'this does not establish multiframe concurrency stress coverage.',
            'No committed encoder input and no committed macOS capture bytes, so no '
            'sample-level difference against a source pattern is recomputed here.',
            'Decoder agreement on these bytes is not a HyperCine decoder, GoPro '
            'product, SDK or SMPTE VC-5 conformance result.',
        ],
    }


def validate_planes(planes):
    require(type(planes) is list and len(planes) == 3, 'plane list is malformed')
    expected = plane_layout(WIDTH, HEIGHT)
    require(planes == expected, 'plane layout contradicts the capture geometry')


def verify_evidence(evidence, packet_path=None, manifest_path=None, macos_path=None):
    """Hash-only re-check of a committed evidence file."""
    require(type(evidence) is dict and evidence.get('schema') in (SCHEMA, LEGACY_SCHEMA),
            'unsupported evidence schema')
    require(evidence.get('issue') == '#381', 'evidence names another issue')
    for field in ('packet', 'decoder', 'container', 'capture', 'interpretation',
                  'stability', 'compared', 'negatives', 'limits'):
        require(field in evidence, 'evidence is missing a required section')

    capture = evidence['capture']
    require(capture.get('byte_length') == CAPTURE_BYTES, 'capture length contradicts the geometry')
    require(capture.get('samples') == SAMPLE_COUNT, 'capture sample count contradicts the geometry')
    require((capture.get('pixel_format'), capture.get('bit_depth')) == (PIXEL_FORMAT, BIT_DEPTH),
            'capture format is not the registered one')
    require(is_digest(capture.get('sha256')), 'capture has no bound digest')

    container = evidence['container']
    require((container.get('codec'), container.get('width'), container.get('height'),
             container.get('pixel_format')) == ('cfhd', WIDTH, HEIGHT, PIXEL_FORMAT),
            'container geometry or codec label changed')
    interpretation = evidence['interpretation']
    validate_planes(interpretation.get('planes'))
    require(interpretation.get('range') == 'studio (64-940)', 'range interpretation changed')
    require('not asserted' in str(interpretation.get('chroma_siting')),
            'chroma siting must stay unasserted')

    stability = evidence['stability']
    require(stability.get('identical') is True and
            type(stability.get('runs')) is int and
            stability['runs'] == len(DECODE_VARIANTS) and
            stability.get('variants') == [name for name, _ in DECODE_VARIANTS],
            'capture was not proven stable across registered independent decodes')
    require(stability.get('distinct_digests') == 1, 'stability claims differing digests')
    require(stability.get('digest') == capture['sha256'], 'stability digest is not the capture digest')
    if evidence['schema'] == SCHEMA:
        require(stability.get('commands') == recorded_commands(),
                'decode command options do not bind the registered input/output roles')
    else:
        # Keep immutable v1 measurements verifiable as digest evidence only.
        # They did not record commands and placed -threads on the output encoder.
        require('commands' not in stability,
                'legacy evidence cannot claim corrected decoder command coverage')

    decoder = evidence['decoder']
    require(decoder.get('id') == 'ffmpeg-cfhd', 'decoder role mismatch')
    require(is_digest(decoder.get('binary_sha256')), 'decoder executable is not bound')
    require(isinstance(decoder.get('version'), str) and decoder['version'],
            'decoder version is not recorded')

    require(evidence['packet'].get('frame_index') == FRAME_INDEX, 'unexpected frame index')
    require(evidence['packet'].get('artifact') == PACKET_ARTIFACT,
            'packet provenance artifact changed')
    if packet_path is not None:
        packet = Path(packet_path).read_bytes()
        require(len(packet) == evidence['packet'].get('byte_length'),
                'packet length contradicts the evidence')
        require(sha256(packet) == evidence['packet'].get('sha256'),
                'packet digest contradicts the evidence')

    if manifest_path is not None:
        manifest = load_manifest(manifest_path)
        require(manifest['sha256'] == evidence['packet'].get('sha256'),
                'evidence packet is not the manifest packet')
        require(manifest['byte_length'] == evidence['packet'].get('byte_length'),
                'evidence packet length is not the manifest length')

    compared = evidence['compared']
    require(compared.get('source') == MACOS_EVIDENCE_ARTIFACT,
            'comparison source is not the retained macOS evidence artifact')
    require(compared.get('frame_index') == FRAME_INDEX, 'comparison names another frame')
    require(is_digest(compared.get('macos_decoded_sha256')),
            'comparison has no bound macOS digest')
    if macos_path is not None:
        macos = macos_recorded_digest(macos_path)
        require(macos.get('raw_sha256') == evidence['packet'].get('sha256'),
                'retained macOS evidence belongs to another packet')
        require(compared['macos_decoded_sha256'] == macos.get('decoded_sha256'),
                'comparison digest contradicts the retained macOS evidence')
        require(compared.get('match') is (macos['decoded_sha256'] == capture['sha256']),
                'match flag contradicts the retained macOS evidence')
    require(compared.get('match') is (compared['macos_decoded_sha256'] == capture['sha256']),
            'match flag contradicts the recorded digests')
    require(compared.get('match') is True,
            'decoded pixels disagree with retained macOS evidence')

    negatives = evidence['negatives']
    require(negatives.get('changed_capture', {}).get('detected') is True,
            'changed-capture negative was not recorded as detected')
    require((negatives['changed_capture'].get('mismatches'),
             negatives['changed_capture'].get('max_error')) == (1, 1),
            'changed-capture negative has unexpected metrics')
    require(negatives.get('forged_metrics', {}).get('rejected') is True,
            'forged-metrics negative was not recorded as rejected')

    require(isinstance(evidence.get('limits'), list) and evidence['limits'],
            'evidence carries no limits')
    return evidence


def main(argv=None):
    parser = argparse.ArgumentParser(description='Record or verify one bounded Metal packet capture.')
    parser.add_argument('--verify', metavar='EVIDENCE')
    parser.add_argument('--packet', default=str(ROOT / 'tests/fixtures' / PACKET_ARTIFACT))
    parser.add_argument('--manifest', default=str(ROOT / MANIFEST_ARTIFACT))
    parser.add_argument('--macos-evidence', default=str(ROOT / MACOS_EVIDENCE_ARTIFACT))
    parser.add_argument('--ffmpeg')
    parser.add_argument('--ffprobe')
    parser.add_argument('--out')
    arguments = parser.parse_args(argv)
    try:
        if arguments.verify:
            evidence = json.loads(Path(arguments.verify).read_text(encoding='utf-8'))
            verify_evidence(evidence, arguments.packet, arguments.manifest,
                            arguments.macos_evidence)
            print(json.dumps({'schema': evidence['schema'], 'verified': True,
                              'input_thread_options_bound': evidence['schema'] == SCHEMA,
                              'capture_sha256': evidence['capture']['sha256'],
                              'match': evidence['compared']['match']}))
            return 0
        require(bool(arguments.ffmpeg and arguments.ffprobe),
                'live capture needs --ffmpeg and --ffprobe')
        evidence = build_evidence(arguments.packet, arguments.ffmpeg, arguments.ffprobe,
                                  arguments.manifest, arguments.macos_evidence)
        verify_evidence(evidence, arguments.packet, arguments.manifest,
                        arguments.macos_evidence)
        text = json.dumps(evidence, indent=2, sort_keys=True) + '\n'
        if arguments.out:
            Path(arguments.out).write_text(text, encoding='utf-8')
        print(json.dumps({'schema': SCHEMA,
                          'capture_sha256': evidence['capture']['sha256'],
                          'capture_byte_length': evidence['capture']['byte_length'],
                          'decoder_binary_sha256': evidence['decoder']['binary_sha256'],
                          'decoder_version': evidence['decoder']['version'],
                          'macos_match': evidence['compared']['match'],
                          'stable_runs': evidence['stability']['runs'],
                          'changed_capture_detected': True,
                          'forged_metrics_rejected': True}))
        return 0
    except (ValueError, OSError, KeyError, json.JSONDecodeError) as error:
        print('metal capture refused: ' + str(error).splitlines()[0][:200])
        return 2


if __name__ == '__main__':
    sys.exit(main())
