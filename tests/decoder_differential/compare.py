#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
"""Bounded, standard-library-only decoder audit. Delta = GoPro minus FFmpeg."""
import argparse
from array import array
from collections import Counter
import hashlib
import json
import math
import operator
from pathlib import Path
import shutil
import struct
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from validate_standalone import avi, chunk

# Above this plane size only overall, border/interior and parity groups are
# emitted. The per-sample code-bin/spatial grouping loop is not bounded enough
# for 4K/8K planes; omitting it is reported, never silently inferred.
FULL_DETAIL_SAMPLES = 8_000_000
SEQUENCE_INDICES = (0, 17, 34, 51, 68, 85, 102, 119)
BGRA_ARMS = (
    ('bt709_limited_neighbor',
     'scale=in_color_matrix=bt709:in_range=limited:out_range=full:flags=neighbor,format=bgra'),
    ('bt709_full_neighbor',
     'scale=in_color_matrix=bt709:in_range=full:out_range=full:flags=neighbor,format=bgra'),
    ('bt709_limited_bilinear',
     'scale=in_color_matrix=bt709:in_range=limited:out_range=full,format=bgra'),
)
RANGE_ARMS = (
    ('full_to_full', 'scale=in_range=full:out_range=full,format=yuv422p10le'),
    ('limited_to_full', 'scale=in_range=limited:out_range=full,format=yuv422p10le'),
)


def digest(raw):
    return hashlib.sha256(raw).hexdigest()


def unpack(raw, width, height):
    if width <= 0 or width % 2 or height <= 0 or width > 7680 or height > 4320:
        raise ValueError('Invalid bounded YUV422 geometry')
    if len(raw) != width * height * 4:
        raise ValueError('Wrong p10 byte length (tight top-down planes required)')
    values = array('H')
    values.frombytes(raw)
    if sys.byteorder != 'little':
        values.byteswap()
    if max(values) > 1023:
        raise ValueError('Nonzero upper six bits in p10 container')
    return values


def unpack_bgra(raw, width, height):
    if width <= 0 or height <= 0 or width > 7680 or height > 4320:
        raise ValueError('Invalid bounded BGRA geometry')
    if len(raw) != width * height * 4:
        raise ValueError('Wrong BGRA byte length (tight top-down rows required)')
    return memoryview(raw)


def summarize(hist, peak=1023):
    count = sum(hist.values())
    if not count:
        return dict(samples=0, mismatches=0, max_abs=0, mean_abs=None)
    absolute = Counter()
    for delta, n in hist.items():
        absolute[abs(delta)] += n
    cumulative = 0
    for error in sorted(absolute):
        cumulative += absolute[error]
        if cumulative >= math.ceil(count * .95):
            p95 = error
            break
    squares = sum(d*d*n for d, n in hist.items())
    return dict(samples=count, mismatches=count-hist.get(0, 0),
                mismatch_fraction=(count-hist.get(0, 0))/count,
                max_abs=max(absolute), mean_abs=sum(d*n for d, n in absolute.items())/count,
                p95_abs=p95, bias=sum(d*n for d, n in hist.items())/count,
                rmse=math.sqrt(squares/count),
                psnr_db=10*math.log10(peak**2*count/squares) if squares else None,
                signed_histogram={str(d): hist[d] for d in sorted(set(hist) | set(range(-3, 4)))})


def _histograms(a, b):
    return Counter(map(operator.sub, a, b))


def compare_plane(a, b, width, height, margins, peak=1023):
    """Exhaustive per-sample statistics for one plane.

    Overall, border/interior and parity groups stay exhaustive at every size.
    Code-bin and 8x8 spatial groups are emitted only while the plane is below
    FULL_DETAIL_SAMPLES; the omission is recorded in the result.
    """
    mx, my = margins
    count = width * height
    detail = 'full' if count <= FULL_DETAIL_SAMPLES else 'basic'
    hist = _histograms(a, b)
    border, interior = Counter(), Counter()
    parity = {f'{x},{y}': Counter() for x in (0, 1) for y in (0, 1)}
    code = {k: Counter() for k in range((peak >> 6) + 1)} if detail == 'full' else None
    spatial = {k: Counter() for k in range(64)} if detail == 'full' else None
    sx = [x*8//width for x in range(width)] if detail == 'full' else None
    first = []
    for y in range(height):
        start = y*width
        ra, rb = a[start:start+width], b[start:start+width]
        parity[f'0,{y&1}'].update(map(operator.sub, ra[0::2], rb[0::2]))
        parity[f'1,{y&1}'].update(map(operator.sub, ra[1::2], rb[1::2]))
        if y < my or y >= height-my:
            border.update(map(operator.sub, ra, rb))
        else:
            border.update(map(operator.sub, ra[:mx], rb[:mx]))
            border.update(map(operator.sub, ra[width-mx:], rb[width-mx:]))
            interior.update(map(operator.sub, ra[mx:width-mx], rb[mx:width-mx]))
        if detail == 'full':
            gy = (y*8//height)*8
            for x in range(width):
                va, vb = ra[x], rb[x]
                delta = va-vb
                code[vb >> 6][delta] += 1
                spatial[gy+sx[x]][delta] += 1
                if delta and len(first) < 32:
                    first.append(dict(x=x, y=y, gopro=va, ffmpeg=vb, delta=delta))
        elif len(first) < 32 and bytes(ra) != bytes(rb):
            for x in range(width):
                va, vb = ra[x], rb[x]
                if va != vb:
                    first.append(dict(x=x, y=y, gopro=va, ffmpeg=vb, delta=va-vb))
                    if len(first) >= 32:
                        break
    groups = {'border': {'border': border, 'interior': interior}, 'parity': parity}
    if detail == 'full':
        groups['reference_code_bin'] = {str(k): v for k, v in code.items()}
        groups['spatial_8x8'] = {f'{k//8},{k%8}': v for k, v in spatial.items()}
    result = dict(**summarize(hist, peak), peak=peak, detail=detail,
                  first_32_differences=first,
                  groups={category: {key: summarize(h, peak) for key, h in sorted(bins.items())}
                          for category, bins in sorted(groups.items())})
    if detail != 'full':
        result['groups_omitted'] = ['reference_code_bin', 'spatial_8x8']
    return result


def compare(gopro, ffmpeg, width, height):
    a, b = unpack(gopro, width, height), unpack(ffmpeg, width, height)
    va, vb = memoryview(a), memoryview(b)
    offset, planes = 0, {}
    for name, pw, margins in [('Y', width, (8, 8)), ('Cb', width//2, (4, 8)), ('Cr', width//2, (4, 8))]:
        samples = pw*height
        planes[name] = compare_plane(va[offset:offset+samples], vb[offset:offset+samples],
                                     pw, height, margins)
        offset += samples
    return planes


def compare_bgra(gopro, ffmpeg, width, height):
    a, b = unpack_bgra(gopro, width, height), unpack_bgra(ffmpeg, width, height)
    # BGRA interleaved; each channel is compared as an 8-bit plane. Alpha is
    # included because it is a real output byte, not a decoder-parity claim.
    return {name: compare_plane(a[index::4], b[index::4], width, height, (8, 8), peak=255)
            for index, name in enumerate(('B', 'G', 'R', 'A'))}


def run(command, timeout=300):
    result = subprocess.run([str(x) for x in command], capture_output=True, timeout=timeout)
    if result.returncode:
        # Verbose tool diagnostics may contain local paths; keep them out of reports.
        raise RuntimeError(f'Decoder command failed: exit {result.returncode}')
    return result.stdout.decode('utf-8', errors='replace')


def manifest_root(corpus, quality, tier):
    legacy = corpus/quality/'manifest.json'
    if legacy.exists():
        return corpus/quality
    tiered = corpus/quality/tier/'manifest.json'
    if not tiered.exists():
        raise FileNotFoundError(f'No manifest for {quality}/{tier} in corpus')
    return corpus/quality/tier


def load_manifest(root):
    return json.loads((root/'manifest.json').read_text(encoding='utf-8'))


def verified_packet(root, manifest, frame, encoder='sdk'):
    relative = f"frames/{manifest['profile']['quality']}-sequence-{frame['width']}x{frame['height']}/frame_{frame['frame_index']:04d}/{encoder}.cfhd"
    entry = next(a for a in manifest['artifacts'] if a['path'] == relative)
    packet = (root/relative).read_bytes()
    if len(packet) != entry['byte_length'] or digest(packet) != entry['sha256'] or digest(packet) != frame[f'{encoder}_sha256']:
        raise ValueError('Archived packet does not match manifest')
    return packet


def ffmpeg_raw(args, wrapped, output, filters=None, frames=None, fps_mode=None, pix_fmt='yuv422p10le'):
    command = [args.ffmpeg, '-v', 'error', '-y', '-threads', '1', '-i', wrapped]
    if filters:
        command += ['-vf', filters]
    if fps_mode:
        command += ['-fps_mode', fps_mode]
    if frames:
        command += ['-frames:v', str(frames)]
    command += ['-pix_fmt', pix_fmt, '-f', 'rawvideo', output]
    return run(command)


def range_controls(wrapped, width, height, baseline_sha, work, args):
    arms = {}
    for name, filters in RANGE_ARMS:
        output = work/f'ffmpeg-range-{name}.p10'
        ffmpeg_raw(args, wrapped, output, filters=filters, frames=1)
        raw = output.read_bytes()
        unpack(raw, width, height)
        sha = digest(raw)
        arms[name] = dict(filters=filters, sha256=sha, equals_baseline=sha == baseline_sha)
    return dict(baseline_sha256=baseline_sha, arms=arms)


def _compact_plane(plane):
    """Overall conversion-arm statistics without the full grouping matrix."""
    result = {key: value for key, value in plane.items()
              if key not in ('groups', 'first_32_differences')}
    result['first_8_differences'] = plane.get('first_32_differences', [])[:8]
    result['groups_omitted'] = 'conversion-path arm'
    return result


def flip_vertical(raw, width, height, pixel_bytes=4):
    stride = width*pixel_bytes
    return b''.join(raw[y*stride:(y+1)*stride] for y in range(height-1, -1, -1))


def _orientation_sample_mean(a, b, width, height, rows=16):
    """Mean absolute byte difference over the first and last sampled rows."""
    stride = width*4
    sampled = min(rows, height)
    total = count = 0
    step = 16 if stride >= 64 else 4
    for y in list(range(sampled)) + list(range(height-sampled, height)):
        offset = y*stride
        for i in range(0, stride, step):
            total += abs(a[offset+i]-b[offset+i]) + abs(a[offset+i+1]-b[offset+i+1])
            count += 2
    return total/count


def align_bgra_orientation(sdk_raw, reference_raw, width, height):
    top_down = _orientation_sample_mean(sdk_raw, reference_raw, width, height)
    flipped = flip_vertical(sdk_raw, width, height)
    flipped_mean = _orientation_sample_mean(flipped, reference_raw, width, height)
    if flipped_mean < top_down:
        return flipped, dict(selected='flipped_to_top_down', top_down_sample_mean_abs=top_down,
                             flipped_sample_mean_abs=flipped_mean)
    return sdk_raw, dict(selected='top_down', top_down_sample_mean_abs=top_down,
                         flipped_sample_mean_abs=flipped_mean)


def bgra_controls(sample, width, height, work, args, repeats=2):
    output = work/'gopro.bgra'
    hashes = []
    for _ in range(repeats):
        run([args.sdk_dump, sample, output, width, height, 'bgra'])
        raw = output.read_bytes()
        unpack_bgra(raw, width, height)
        hashes.append(digest(raw))
    if len(set(hashes)) != 1:
        raise RuntimeError('GoPro BGRA output unstable across repeats')
    sdk_raw = output.read_bytes()
    arms, aligned, orientation = {}, None, None
    for name, filters in BGRA_ARMS:
        arm_hashes, planes = [], None
        for repeat in range(repeats):
            target = work/f'ffmpeg-bgra-{name}.bgra'
            ffmpeg_raw(args, work/'input.avi', target, filters=filters, frames=1, pix_fmt='bgra')
            raw = target.read_bytes()
            unpack_bgra(raw, width, height)
            arm_hashes.append(digest(raw))
            if repeat == 0:
                if aligned is None:
                    aligned, orientation = align_bgra_orientation(sdk_raw, raw, width, height)
                planes = {channel: _compact_plane(plane) for channel, plane in
                          compare_bgra(aligned, raw, width, height).items()}
        arms[name] = dict(filters=filters, stable=len(set(arm_hashes)) == 1,
                          repeat_hashes=arm_hashes, planes=planes)
    return dict(gopro_repeat_hashes=hashes, gopro_stable=True, orientation=orientation,
                interpretation='conversion-path comparison, not decoder parity', arms=arms)


def audit_case(packet, width, height, label, args, repeats=3, run_range=False, run_bgra=False):
    work = Path(args.work)/label
    work.mkdir(parents=True, exist_ok=True)
    sample = work/'input.cfhd'
    sample.write_bytes(packet)
    wrapped = work/'input.avi'
    wrapped.write_bytes(avi(packet, width, height))
    probe = json.loads(run([args.ffprobe, '-v', 'error', '-select_streams', 'v:0',
                           '-show_entries', 'stream=codec_name,pix_fmt,width,height,color_range,color_space,chroma_location',
                           '-of', 'json', wrapped]))['streams'][0]
    if (probe.get('codec_name'), probe.get('width'), probe.get('height'), probe.get('pix_fmt')) != ('cfhd', width, height, 'yuv422p10le'):
        raise ValueError('FFprobe source format/geometry differs from audit contract')
    hashes, first = [], None
    for repeat in range(repeats):
        sdk, ff = work/'gopro.p10', work/'ffmpeg.p10'
        run([args.sdk_dump, sample, sdk, width, height])
        ffmpeg_raw(args, wrapped, ff, frames=1)
        a, b = sdk.read_bytes(), ff.read_bytes()
        unpack(a, width, height); unpack(b, width, height)
        hashes.append(dict(gopro=digest(a), ffmpeg=digest(b)))
        if repeat == 0:
            first = compare(a, b, width, height)
    baseline = hashes[0]['ffmpeg']
    record = dict(label=label, width=width, height=height, input_sha256=digest(packet),
                  ffprobe=probe, repeat_hashes=hashes,
                  stable_gopro=len({h['gopro'] for h in hashes}) == 1,
                  stable_ffmpeg=len({h['ffmpeg'] for h in hashes}) == 1,
                  planes=first, cause='CAUSE_UNKNOWN')
    if run_range:
        record['range_controls'] = range_controls(wrapped, width, height, baseline, work, args)
    if run_bgra:
        record['bgra'] = bgra_controls(sample, width, height, work, args)
    return record


def avi_sequence(packets, width, height):
    frames = len(packets)
    largest = max(map(len, packets))
    avih = struct.pack('<14I', 40000, 0, 0, 0, frames, 0, 1, largest, width, height, 0, 0, 0, 0)
    strh = struct.pack('<4s4sIHH8I4h', b'vids', b'CFHD', 0, 0, 0, 0, 1, 25, 0, frames,
                       largest, 0xffffffff, 0, 0, 0, width, height)
    strf = struct.pack('<IiiHH4sIiiII', 40, width, height, 1, 24, b'CFHD', largest, 0, 0, 0, 0)
    hdrl = chunk(b'LIST', b'hdrl' + chunk(b'avih', avih) +
                 chunk(b'LIST', b'strl' + chunk(b'strh', strh) + chunk(b'strf', strf)))
    return chunk(b'RIFF', b'AVI ' + hdrl +
                 chunk(b'LIST', b'movi' + b''.join(chunk(b'00dc', p) for p in packets)))


def select_filter(indices):
    return "select='" + '+'.join(f'eq(n\\,{index})' for index in indices) + "',format=yuv422p10le"


def sequence_arm(corpus, quality, tier, indices, args, repeats=2):
    root = manifest_root(corpus, quality, tier)
    manifest = load_manifest(root)
    frames = sorted(manifest['frames'], key=lambda f: f['frame_order'])
    if [f['frame_index'] for f in frames] != list(range(len(frames))):
        raise ValueError('Sequence manifest is not contiguous')
    packets = [verified_packet(root, manifest, frame) for frame in frames]
    width, height = frames[0]['width'], frames[0]['height']
    if any((f['width'], f['height']) != (width, height) for f in frames):
        raise ValueError('Mixed geometry in sequence manifest')
    if any(i < 0 or i >= len(frames) for i in indices):
        raise ValueError('Sequence sample index outside manifest')
    work = Path(args.work)/'sequence'
    work.mkdir(parents=True, exist_ok=True)
    listing = work/'packets.txt'
    listing.write_text('\n'.join(str(root/f"frames/{manifest['profile']['quality']}-sequence-{width}x{height}/frame_{f['frame_index']:04d}/sdk.cfhd") for f in frames) + '\n', encoding='utf-8')
    wrapped = work/'sequence.avi'
    wrapped.write_bytes(avi_sequence(packets, width, height))
    keep = ','.join(map(str, indices))
    stateful, ffmpeg_hashes = [], []
    for repeat in range(repeats):
        out = work/f'stateful-{repeat}'
        if out.exists():
            for path in out.iterdir():
                path.unlink()
        out.mkdir(parents=True, exist_ok=True)
        run([args.sdk_dump, '--sequence', listing, out, width, height, 'yuv422p10le', keep])
        stateful.append({i: digest((out/f'frame_{i:04d}.p10').read_bytes()) for i in indices})
        target = work/f'ffmpeg-selected-{repeat}.p10'
        ffmpeg_raw(args, wrapped, target, filters=select_filter(indices), frames=len(indices),
                   fps_mode='passthrough')
        raw = target.read_bytes()
        frame_bytes = width*height*4
        if len(raw) != frame_bytes*len(indices):
            raise ValueError('FFmpeg selected-frame output has unexpected length')
        ffmpeg_hashes.append([digest(raw[n*frame_bytes:(n+1)*frame_bytes]) for n in range(len(indices))])
    records = []
    raw = (work/'ffmpeg-selected-0.p10').read_bytes()
    for position, index in enumerate(indices):
        independent = work/f'independent_{index:04d}.p10'
        # Independent per-packet control: same packet, fresh decoder instance.
        packet_path = root/f"frames/{manifest['profile']['quality']}-sequence-{width}x{height}/frame_{index:04d}/sdk.cfhd"
        run([args.sdk_dump, packet_path, independent, width, height])
        ffmpeg_frame = work/f'ffmpeg-selected-{position}.p10'
        frame_bytes = width*height*4
        ffmpeg_frame.write_bytes(raw[position*frame_bytes:(position+1)*frame_bytes])
        stateful_frame = work/'stateful-0'/f'frame_{index:04d}.p10'
        planes = compare(stateful_frame.read_bytes(), ffmpeg_frame.read_bytes(), width, height)
        records.append(dict(frame_index=index, pattern=frames[index]['pattern'],
                            input_sha256=digest(packets[index]),
                            stateful_sdk_sha256={str(r): stateful[r][index] for r in range(repeats)},
                            independent_sdk_sha256=digest(independent.read_bytes()),
                            ffmpeg_sha256={str(r): ffmpeg_hashes[r][position] for r in range(repeats)},
                            stateful_equals_independent=stateful[0][index] == digest(independent.read_bytes()),
                            stateful_stable=len({stateful[r][index] for r in range(repeats)}) == 1,
                            ffmpeg_stable=len({ffmpeg_hashes[r][position] for r in range(repeats)}) == 1,
                            planes=planes))
    return dict(quality=quality, tier=tier, width=width, height=height,
                decoded_frames=len(frames), sampled_indices=list(indices), repeats=repeats,
                corpus_manifest_sha256=digest((root/'manifest.json').read_bytes()),
                stateful=True, method='one decoder instance, PrepareToDecode on frame 0, DecodeSample per frame',
                frames=records)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--corpus', type=Path, required=True)
    p.add_argument('--sdk-dump', required=True)
    p.add_argument('--ffmpeg', required=True)
    p.add_argument('--ffprobe', required=True)
    p.add_argument('--work', required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--only', default=None, help='comma-separated label substrings')
    p.add_argument('--skip-sequence', action='store_true')
    p.add_argument('--sequence-indices', default=','.join(map(str, SEQUENCE_INDICES)))
    args = p.parse_args()
    # Windows CreateProcess does not resolve forward-slash relative executables.
    args.sdk_dump = str(Path(args.sdk_dump).resolve())
    for name in ('ffmpeg', 'ffprobe'):
        resolved = shutil.which(getattr(args, name)) or str(Path(getattr(args, name)).resolve())
        setattr(args, name, resolved)
    wanted = [x for x in (args.only or '').split(',') if x]
    def selected(label):
        return not wanted or any(x in label for x in wanted)

    records = []
    def add(record, **extra):
        if not selected(record['label']):
            return
        record.update(extra)
        records.append(record)
        print(record['label'], [record['planes'][plane]['max_abs'] for plane in ('Y', 'Cb', 'Cr')], flush=True)

    for quality in ('low', 'filmscan2', 'filmscan3'):
        root = manifest_root(args.corpus, quality, '1080p-16f')
        manifest = load_manifest(root)
        for index in (0, 3, 6):
            frame = next(f for f in manifest['frames'] if f['frame_index'] == index and f['width'] == 1920 and f['height'] == 1080)
            packet = verified_packet(root, manifest, frame)
            run_extra = dict(run_range=quality == 'low' and index == 0,
                             run_bgra=(quality, index) in {('low', 0), ('filmscan3', 3), ('filmscan3', 6)})
            add(audit_case(packet, 1920, 1080, f'{quality}-{index}', args, **run_extra),
                quality=quality, frame_index=index, pattern=frame['pattern'],
                corpus_manifest_sha256=digest((root/'manifest.json').read_bytes()))

    for label, quality in (('low-4k-flat', 'low'), ('filmscan3-4k-flat', 'filmscan3')):
        if not selected(label):
            continue
        root = manifest_root(args.corpus, quality, '3840x2160-1f')
        manifest = load_manifest(root)
        frame = manifest['frames'][0]
        add(audit_case(verified_packet(root, manifest, frame), 3840, 2160, label, args,
                       run_range=quality == 'low'),
            quality=quality, frame_index=frame['frame_index'], pattern=frame['pattern'],
            corpus_manifest_sha256=digest((root/'manifest.json').read_bytes()))

    for label, quality in (('low-8k-flat', 'low'), ('filmscan3-8k-flat', 'filmscan3')):
        if not selected(label):
            continue
        root = manifest_root(args.corpus, quality, '7680x4320-1f')
        manifest = load_manifest(root)
        frame = manifest['frames'][0]
        add(audit_case(verified_packet(root, manifest, frame), 7680, 4320, label, args,
                       run_range=quality == 'low'),
            quality=quality, frame_index=frame['frame_index'], pattern=frame['pattern'],
            corpus_manifest_sha256=digest((root/'manifest.json').read_bytes()))

    sequence = None
    if not args.skip_sequence and selected('sequence'):
        indices = [int(x) for x in args.sequence_indices.split(',') if x]
        sequence = sequence_arm(args.corpus, 'filmscan3', 'sequence-120f', indices, args)
        print('sequence', [f"{f['frame_index']}:{max(f['planes'][p]['max_abs'] for p in ('Y', 'Cb', 'Cr'))}"
                           for f in sequence['frames']], flush=True)

    fixtures = Path(__file__).resolve().parents[1]/'fixtures'
    if selected('historical'):
        for pattern in ('flat', 'gradient', 'canonical'):
            add(audit_case((fixtures/'standalone'/f'{pattern}.cfhd').read_bytes(),
                           128, 128, 'historical-'+pattern, args))
        add(audit_case((fixtures/'windows-oracle'/'control.cfhd').read_bytes(),
                       128, 128, 'historical-sdk-control', args))

    report = dict(copyright='Copyright (c) 2026 HyperCine Contributors.', license='MIT OR Apache-2.0',
                  schema_version=2, ffmpeg_version=run([args.ffmpeg, '-version']),
                  ffprobe_version=run([args.ffprobe, '-version']).splitlines()[0],
                  audit_source_sha256={name: digest((Path(__file__).parent/name).read_bytes())
                                       for name in ('compare.py', 'sdk_dump.cpp')},
                  sdk_baseline='11574d0295771edccadd17af14af74a539f924c7',
                  contract=dict(format='yuv422p10le', orientation='top-down', planes='Y,Cb,Cr',
                                storage='tight LE unsigned 16-bit, low 10 bits', delta='GoPro minus FFmpeg',
                                sdk='YU64 Y0/Cr/Y1/Cb >>6; PRCS=65535, CLSY=2, CLSR=1, CLSF=0, CPU_MAX=1',
                                ffmpeg='cfhd, -threads 1, no explicit range or matrix conversion in baseline',
                                psnr_exact='null means zero MSE',
                                spatial='8x8 normalized plane grid; not codec tile attribution',
                                detail_limit=f'planes above {FULL_DETAIL_SAMPLES} samples omit code-bin and spatial groups',
                                bgra='explicit scale filters; conversion-path comparison, not decoder parity',
                                sequence='single decoder instance across the archived 120-frame sequence'),
                  python_version=sys.version.split()[0],
                  corpus=dict(layout='quality/tier/manifest.json (legacy quality/manifest.json still accepted)',
                              inventory=[r['label'] for r in records], cases_compared=len(records),
                              sequence_frames_decoded=sequence['decoded_frames'] if sequence else 0,
                              sequence_frames_compared=len(sequence['frames']) if sequence else 0),
                  pending=['IDWT first differing stage', 'highpass coefficient attribution',
                           'FFmpeg version matrix beyond the measured executable', 'CPU_MAX alternative',
                           '8K full-detail code-bin/spatial groups'],
                  cases=records, sequence=sequence)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2, allow_nan=False)+'\n', encoding='utf-8')
    unstable = [r['label'] for r in records if not (r['stable_gopro'] and r['stable_ffmpeg'])]
    if sequence:
        unstable += [f"sequence-{f['frame_index']}" for f in sequence['frames']
                     if not (f['stateful_stable'] and f['ffmpeg_stable'])]
    if unstable:
        raise RuntimeError('Repeated decoder output unstable: ' + ', '.join(unstable))


if __name__ == '__main__':
    main()
