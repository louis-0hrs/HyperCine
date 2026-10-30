# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
"""Bounded #120 independent-decoder experiment; never qualifies a pixel oracle.

Only the historical synthetic Phase2C 1080p packet/source pair is accepted.
Exit 77 means successful observations with the oracle still BLOCKED; 1 is a
strict experiment/control failure. Failed controls are retained in the report.
Raw tool diagnostics and private paths stay private.
"""
import argparse
from collections import Counter
import hashlib
import json
import math
import os
from pathlib import Path
import re
import struct
import subprocess
import sys

from validate_standalone import avi
from decoder_differential.compare import BGRA_ARMS, flip_vertical, unpack, unpack_bgra

WIDTH, HEIGHT, REPEATS = 1920, 1080, 5
PACKET_SHA = '2b9a28dec5b015bf76a1ba3bce5a458167bd07bd061dbb59a97c00eece4fec52'
SOURCE_SHA = '45c166c3d6a3710d620ff5d738980224edf53cf9ce1bceb87267e474294afff1'
PIXELS = WIDTH * HEIGHT * 4


class CandidateUnavailable(Exception):
    pass


def require(condition, reason):
    if not condition:
        raise ValueError(reason)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def bound(path, length, digest):
    require(path.stat().st_size == length, 'bound file length mismatch')
    data = path.read_bytes()
    require(sha(data) == digest, 'bound file SHA mismatch')
    return data


def command(args):
    return subprocess.run(list(map(str, args)), stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE, timeout=30, check=False)


def tool_identity(path, name):
    if not path.is_file():
        raise CandidateUnavailable()
    try:
        result = command([path, '-version'])
    except (OSError, subprocess.SubprocessError):
        raise CandidateUnavailable() from None
    if result.returncode != 0:
        raise CandidateUnavailable()
    match = re.match(name + r' version ([A-Za-z0-9_.+\-]+)(?:\s|$)',
                     result.stdout.decode('utf-8', errors='replace'))
    if match is None:
        raise CandidateUnavailable()
    return {'version': match.group(1), 'sha256': sha(path.read_bytes())}


def metrics(source, decoded):
    require(len(source) == len(decoded) == PIXELS, 'BGRA adapter length mismatch')
    histogram = Counter(a - b for i, (a, b) in enumerate(zip(source, decoded)) if i % 4 != 3)
    count = sum(histogram.values())
    mse = sum(delta * delta * n for delta, n in histogram.items()) / count
    return {'rgb_psnr_db': 10 * math.log10(255 * 255 / mse) if mse else None,
            'rgb_exact': mse == 0, 'rgb_max_abs': max(map(abs, histogram)),
            'alpha_equal': source[3::4] == decoded[3::4]}


def stable(hashes):
    require(len(hashes) == REPEATS, 'missing repeat observation')
    require(len(set(hashes)) == 1, 'independent decoder is not repeatable')
    return {'repeats': REPEATS, 'distinct': 1, 'output_sha256': hashes[0]}


def mutation_result(status, raw, baseline):
    if status:
        return 'DECODER_REJECTED'
    unpack(raw, WIDTH, HEIGHT)
    require(raw != baseline, 'quant mutation was not discriminated')
    return 'PIXELS_DIFFER'


def experiment(packet, source, ffmpeg, ffprobe, work):
    # Bind both originals and tools before writing the plan or starting decodes.
    data = bound(packet, 900396, PACKET_SHA)
    pixels = bound(source, PIXELS, SOURCE_SHA)
    tools = {'ffmpeg': tool_identity(ffmpeg, 'ffmpeg'),
             'ffprobe': tool_identity(ffprobe, 'ffprobe')}
    # The mutation is fixed by parsed tag identity, not selected from outcomes.
    sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
    from verify_windows_sdk_corpus import parse_packet
    parsed = parse_packet(data)
    at, _, quant = next(tag for tag in parsed['tags'] if tag[1] == 53 and tag[2] > 0)
    mutation = {'tag': 53, 'value_byte_offset': at + 3, 'xor': 1, 'original_quant': quant}
    plan = {'schema': 1, 'packet_sha256': PACKET_SHA, 'source_sha256': SOURCE_SHA,
            'width': WIDTH, 'height': HEIGHT, 'repeats_per_arm': REPEATS,
            'tools': tools, 'mutation': mutation,
            'arms': ['native_yuv422p10le'] + [name for name, _ in BGRA_ARMS[:2]],
            'orientation_hypotheses': ['source_rows_as_stored', 'source_rows_reversed'],
            'qualification_policy': 'observation only; oracle remains unqualified',
            'harness_sha256': sha(Path(__file__).read_bytes()),
            'controls': {'quant_mutation': 'reject or emit different valid pixels',
                         'zero_initial_tag': 'require nonzero decoder exit; retain acceptance as failure'},
            'avi_cadence': 'adapter synthetic 25 fps; NOT original playback measurement'}
    work.mkdir(parents=True, exist_ok=False)
    (work / 'plan.json').write_text(json.dumps(plan, indent=2, sort_keys=True) + '\n')
    wrapped = work / 'original.avi'
    wrapped.write_bytes(avi(data, WIDTH, HEIGHT))
    probe = command([ffprobe, '-v', 'error', '-select_streams', 'v:0', '-show_entries',
                     'stream=codec_name,width,height,pix_fmt,color_range,color_space,chroma_location',
                     '-of', 'json', wrapped])
    require(probe.returncode == 0, 'probe command failed')
    streams = json.loads(probe.stdout).get('streams', [])
    require(len(streams) == 1, 'unexpected stream inventory')
    stream = streams[0]
    require(tuple(stream.get(k) for k in ('codec_name', 'width', 'height', 'pix_fmt')) ==
            ('cfhd', WIDTH, HEIGHT, 'yuv422p10le'), 'stream adapter mismatch')

    def decode(container, label, fmt, filters=None):
        output = work / (label + '.raw')
        args = [ffmpeg, '-v', 'error', '-xerror', '-nostdin', '-threads', '1',
                '-i', container, '-frames:v', '1']
        if filters:
            args += ['-vf', filters]
        result = command(args + ['-pix_fmt', fmt, '-f', 'rawvideo', output])
        return result.returncode, output

    report = {'plan': plan, 'stream': stream, 'arms': {}, 'controls': {},
              'oracle_qualified': False, 'release_gate': 'BLOCKED',
              'state': 'BLOCKED_ORACLE_NOT_QUALIFIED'}
    native = None
    arms = [('native_yuv422p10le', None)] + list(BGRA_ARMS[:2])
    for name, filters in arms:
        hashes, first = [], None
        for repeat in range(REPEATS):
            status, output = decode(wrapped, name + '-' + str(repeat),
                                    'bgra' if filters else 'yuv422p10le', filters)
            require(status == 0, 'positive decoder command failed')
            raw = output.read_bytes()
            (unpack_bgra if filters else unpack)(raw, WIDTH, HEIGHT)
            hashes.append(sha(raw))
            if first is None:
                first = raw
        row = stable(hashes)
        if filters:
            row['source_rows_as_stored'] = metrics(pixels, first)
            row['source_rows_reversed'] = metrics(flip_vertical(pixels, WIDTH, HEIGHT), first)
        else:
            native = first
        report['arms'][name] = row

    mutant = bytearray(data)
    mutant[mutation['value_byte_offset']] ^= mutation['xor']
    mutated = work / 'quant-mutation.avi'
    mutated.write_bytes(avi(bytes(mutant), WIDTH, HEIGHT))
    status, output = decode(mutated, 'quant-mutation', 'yuv422p10le')
    # Adapter/I/O exceptions cannot masquerade as decoder rejection.
    outcome = mutation_result(status, output.read_bytes() if status == 0 else None, native)
    report['controls']['quant_mutation'] = outcome
    report['quant_mutation_evidence'] = {
        'decoder_exit': status, 'mutated_packet_sha256': sha(bytes(mutant)),
        'output_sha256': sha(output.read_bytes()) if status == 0 else None}
    # A separate malformed-header control must yield actual nonzero decoder exit.
    bad = work / 'invalid-signature.avi'
    bad.write_bytes(avi(b'\0\0\0\0' + data[4:], WIDTH, HEIGHT))
    status, output = decode(bad, 'invalid-signature', 'yuv422p10le')
    report['controls']['zero_initial_tag'] = 'DECODER_REJECTED' if status else 'ACCEPTED'
    report['zero_initial_tag_decoder_exit'] = status
    if status == 0:
        raw = output.read_bytes()
        unpack(raw, WIDTH, HEIGHT)
        report['zero_initial_tag_output_equal_original'] = raw == native
        report['state'] = 'CONTROL_NOT_DISCRIMINATED'
    require(sha(packet.read_bytes()) == PACKET_SHA and sha(source.read_bytes()) == SOURCE_SHA,
            'original input changed during experiment')
    return report


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for flag in ('packet', 'source', 'ffmpeg', 'ffprobe', 'work', 'output'):
        p.add_argument('--' + flag, required=True, type=Path)
    a = p.parse_args()
    try:
        # Never put generated files into the private immutable corpus.
        golden = os.environ.get('HYPERCINE_GOLDEN_DIR')
        destinations = [a.work.resolve(), a.output.resolve()]
        require(not a.output.exists() and not a.work.exists(), 'destination exists')
        for destination in destinations:
            for input_path in (a.packet.resolve(), a.source.resolve()):
                require(destination != input_path and input_path.parent not in destination.parents,
                        'destination overlaps immutable input directory')
            if golden:
                g = Path(golden).resolve()
                require(destination != g and g not in destination.parents, 'destination inside corpus')
        report = experiment(a.packet, a.source, a.ffmpeg, a.ffprobe, a.work)
        with a.output.open('x', encoding='utf-8') as stream:
            json.dump(report, stream, indent=2, sort_keys=True)
            stream.write('\n')
        code = observation_exit(report)
        print('OBSERVATIONS_COMPLETE state=' + report['state'] +
              ' oracle_qualified=false release_gate=BLOCKED exit=' + str(code))
        return code
    except CandidateUnavailable:
        print('BLOCKED_CANDIDATE_UNAVAILABLE oracle_qualified=false exit=77')
        return 77
    except (OSError, ValueError, KeyError, StopIteration, struct.error, subprocess.SubprocessError):
        print('STRICT_EXPERIMENT_FAILURE (no qualification report)', file=sys.stderr)
        return 1


def observation_exit(report):
    require(report['oracle_qualified'] is False and report['release_gate'] == 'BLOCKED',
            'observation cannot qualify a release oracle')
    require(report['state'] in ('CONTROL_NOT_DISCRIMINATED', 'BLOCKED_ORACLE_NOT_QUALIFIED'),
            'unknown observation state')
    return 1 if report['state'] == 'CONTROL_NOT_DISCRIMINATED' else 77


if __name__ == '__main__':
    sys.exit(main())
