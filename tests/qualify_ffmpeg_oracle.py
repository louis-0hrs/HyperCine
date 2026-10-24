#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE.
"""Fail-closed qualification of an external FFmpeg build as a candidate oracle.

Qualification is bound to the three committed 128x128 standalone fixtures and
to an explicit list of measured FFmpeg builds. It proves four things on this
host: the inputs and pinned reference bytes match their manifest, fresh
processes of the candidate decoder agree, their output is byte-identical to the
pinned reference, and the comparison still rejects a different packet and a
mutated packet. It does not qualify any other packet, format, resolution or
GPU path, and it never promotes a release gate by itself.

Exit codes: 0 qualified, 1 control failed, 77 blocked (tool absent or build
not on the measured list).
"""
import argparse
import hashlib
import json
import re
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile

from validate_standalone import avi

WIDTH = 128
HEIGHT = 128
OUTPUT_BYTES = WIDTH * HEIGHT * 4  # yuv422p10le: Y + Cb + Cr, 16-bit LE
PEAK = 1023
PATTERNS = ('flat', 'gradient', 'canonical')
# Builds that reproduced the pinned n8.0 reference bytes on the measured host.
# Extend only after re-running this harness and reviewing the new evidence.
QUALIFIED_BUILD_PREFIXES = ('n8.0', '9.0.2')
# Pre-registered discriminator control: flip one entropy-payload byte of the
# canonical packet. The offset is fixed here, not chosen per run.
MUTATION_OFFSET = 500
MUTATION_XOR = 0xFF

EXIT_QUALIFIED = 0
EXIT_FAILED = 1
EXIT_BLOCKED = 77


class Blocked(Exception):
    """The candidate oracle cannot be evaluated on this host."""


class Failed(Exception):
    """A bound input or control did not behave as the contract requires."""


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def load_manifest(fixtures):
    path = Path(fixtures) / 'manifest.json'
    try:
        manifest = json.loads(path.read_text())
    except (OSError, ValueError) as error:
        raise Failed('cannot read fixture manifest: {}'.format(error))
    declared = {entry['file']: entry for entry in manifest.get('files', [])}
    if not declared:
        raise Failed('fixture manifest declares no files')
    return declared


def load_bound(fixtures, declared, name):
    entry = declared.get(name)
    if entry is None:
        raise Failed('fixture {} is not declared in the manifest'.format(name))
    try:
        data = (Path(fixtures) / name).read_bytes()
    except OSError as error:
        raise Failed('cannot read fixture {}: {}'.format(name, error))
    if len(data) != entry['bytes']:
        raise Failed('{} is {} bytes, manifest binds {}'.format(
            name, len(data), entry['bytes']))
    digest = sha256(data)
    if digest != entry['sha256']:
        raise Failed('{} sha256 {} does not match manifest {}'.format(
            name, digest, entry['sha256']))
    return data


def find_tool(explicit, name):
    if explicit:
        path = Path(explicit)
        if not path.is_file():
            raise Blocked('{} not found at the given path'.format(name))
        return str(path)
    found = shutil.which(name)
    if not found:
        raise Blocked('{} is not on PATH'.format(name))
    return found


def build_token(ffmpeg):
    try:
        banner = subprocess.check_output(
            [ffmpeg, '-version'], stderr=subprocess.STDOUT, text=True)
    except (OSError, subprocess.CalledProcessError) as error:
        raise Blocked('cannot run the candidate decoder: {}'.format(error))
    first = banner.splitlines()[0] if banner.splitlines() else ''
    match = re.match(r'ffmpeg version (\S+)', first)
    if not match:
        raise Failed('unrecognized decoder banner: {!r}'.format(first))
    return match.group(1)


def require_qualified_build(token):
    if not any(token.startswith(prefix) for prefix in QUALIFIED_BUILD_PREFIXES):
        raise Blocked('decoder build {} is not on the measured list ({})'.format(
            token, ', '.join(QUALIFIED_BUILD_PREFIXES)))
    return token


def validate_output(raw):
    """Enforce the normalization contract on one decoded image."""
    if len(raw) != OUTPUT_BYTES:
        raise Failed('decoded {} bytes, contract binds {}'.format(
            len(raw), OUTPUT_BYTES))
    samples = struct.unpack('<{}H'.format(OUTPUT_BYTES // 2), raw)
    if max(samples) > PEAK:
        raise Failed('decoded sample {} exceeds {} bit depth'.format(
            max(samples), PEAK))
    return samples


def probe_stream(ffprobe, container):
    probe = subprocess.run(
        [ffprobe, '-v', 'error', '-show_streams', '-of', 'json', str(container)],
        check=False, capture_output=True, text=True)
    if probe.returncode != 0:
        raise Failed('ffprobe rejected the container: {}'.format(
            probe.stderr.strip()))
    try:
        streams = json.loads(probe.stdout).get('streams', [])
    except ValueError as error:
        raise Failed('ffprobe emitted invalid JSON: {}'.format(error))
    if len(streams) != 1:
        raise Failed('expected one stream, found {}'.format(len(streams)))
    stream = streams[0]
    observed = (stream.get('codec_name'), stream.get('width'),
                stream.get('height'), stream.get('pix_fmt'))
    expected = ('cfhd', WIDTH, HEIGHT, 'yuv422p10le')
    if observed != expected:
        raise Failed('stream contract {} != {}'.format(observed, expected))
    return observed


def decode(ffmpeg, ffprobe, packet, workdir, label):
    container = workdir / (label + '.avi')
    container.write_bytes(avi(packet))
    probe_stream(ffprobe, container)
    output = workdir / (label + '.yuv')
    result = subprocess.run(
        [ffmpeg, '-v', 'error', '-xerror', '-y', '-i', str(container),
         '-frames:v', '1', '-c:v', 'rawvideo', '-pix_fmt', 'yuv422p10le',
         '-f', 'rawvideo', str(output)],
        check=False, capture_output=True, text=True)
    if result.returncode != 0:
        lines = result.stderr.strip().splitlines()
        raise Failed('decode of {} failed: {}'.format(
            label, lines[-1] if lines else 'no output'))
    try:
        raw = output.read_bytes()
    except OSError as error:
        raise Failed('decode of {} wrote no image: {}'.format(label, error))
    validate_output(raw)
    return raw


def self_check_rejects_corrupt_output():
    """The adapter must reject data outside its declared normalization."""
    try:
        validate_output(bytes(OUTPUT_BYTES - 1))
    except Failed:
        pass
    else:
        raise Failed('adapter accepted a truncated image')
    samples = [0] * (OUTPUT_BYTES // 2)
    samples[-1] = PEAK + 1
    try:
        validate_output(struct.pack('<{}H'.format(len(samples)), *samples))
    except Failed:
        pass
    else:
        raise Failed('adapter accepted an out-of-range sample')


def qualify(fixtures, ffmpeg, ffprobe, repeats):
    declared = load_manifest(fixtures)
    packets, references = {}, {}
    for pattern in PATTERNS:
        packets[pattern] = load_bound(fixtures, declared, pattern + '.cfhd')
        references[pattern] = load_bound(fixtures, declared, pattern + '.p10')
    self_check_rejects_corrupt_output()

    report = dict(state='QUALIFIED', packets=[], controls=[])
    with tempfile.TemporaryDirectory(prefix='hypercine-oracle-qual-') as temporary:
        workdir = Path(temporary)
        decoded = {}
        for pattern in PATTERNS:
            hashes = []
            for repeat in range(repeats):
                raw = decode(ffmpeg, ffprobe, packets[pattern], workdir,
                             '{}-{}'.format(pattern, repeat))
                hashes.append(sha256(raw))
                decoded.setdefault(pattern, raw)
            distinct = len(set(hashes))
            matches = decoded[pattern] == references[pattern]
            report['packets'].append(dict(
                packet=pattern, packet_sha256=sha256(packets[pattern]),
                reference_sha256=sha256(references[pattern]),
                output_sha256=sha256(decoded[pattern]), distinct=distinct,
                repeats=repeats, reference_match=matches))
            if distinct != 1:
                raise Failed('{} produced {} distinct outputs'.format(
                    pattern, distinct))
            if not matches:
                raise Failed('{} output differs from the pinned reference'.format(
                    pattern))

        # Control 1: the comparison must reject a different packet.
        cross = decoded['flat'] == references['gradient']
        report['controls'].append(dict(
            control='cross_packet', outcome='REJECTED' if not cross else 'ACCEPTED'))
        if cross:
            raise Failed('comparison accepted the wrong packet')

        # Control 2: a mutated packet must not qualify as the pinned reference.
        mutated = bytearray(packets['canonical'])
        if MUTATION_OFFSET >= len(mutated):
            raise Failed('mutation offset {} is outside the packet'.format(
                MUTATION_OFFSET))
        mutated[MUTATION_OFFSET] ^= MUTATION_XOR
        try:
            mutant = decode(ffmpeg, ffprobe, bytes(mutated), workdir, 'mutated')
        except Failed as error:
            report['controls'].append(dict(
                control='mutated_packet', outcome='DECODER_REJECTED',
                detail=str(error)))
        else:
            changed = mutant != references['canonical']
            report['controls'].append(dict(
                control='mutated_packet',
                outcome='PIXELS_DIFFER' if changed else 'ACCEPTED'))
            if not changed:
                raise Failed('mutated packet decoded to the pinned reference')
    return report


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--fixtures', default=str(
        Path(__file__).parent / 'fixtures' / 'standalone'))
    parser.add_argument('--ffmpeg')
    parser.add_argument('--ffprobe')
    parser.add_argument('--repeats', type=int, default=5)
    args = parser.parse_args()
    if args.repeats < 2:
        parser.error('--repeats must be at least 2 for a determinism claim')
    try:
        ffmpeg = find_tool(args.ffmpeg, 'ffmpeg')
        ffprobe = find_tool(args.ffprobe, 'ffprobe')
        token = require_qualified_build(build_token(ffmpeg))
        report = qualify(args.fixtures, ffmpeg, ffprobe, args.repeats)
    except Blocked as error:
        print('ORACLE_QUALIFICATION_BLOCKED reason={}'.format(error))
        return EXIT_BLOCKED
    except Failed as error:
        print('ORACLE_QUALIFICATION_FAILED reason={}'.format(error))
        return EXIT_FAILED
    report['decoder_build'] = token
    print('ORACLE_QUALIFICATION state=QUALIFIED decoder_build={} packets={} repeats={}'.format(
        token, len(report['packets']), args.repeats))
    for packet in report['packets']:
        print('ORACLE_QUALIFICATION packet={packet} packet_sha256={packet_sha256} '
              'reference_sha256={reference_sha256} output_sha256={output_sha256} '
              'distinct={distinct} reference_match={reference_match}'.format(**packet))
    for control in report['controls']:
        print('ORACLE_QUALIFICATION control={control} outcome={outcome}'.format(
            **{key: value for key, value in control.items() if key != 'detail'}))
    return EXIT_QUALIFIED


if __name__ == '__main__':
    sys.exit(main())
