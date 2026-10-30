#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
"""Independent Windows #321 original-packet replay; no SDK state inference."""
import argparse
import json
import struct
import subprocess
import sys
import tempfile
from pathlib import Path
from p6_sdk_gap_gate import verified, trace_contract, packet_tables, FROZEN, sample
from p6_writer_parity import load_transfer_index, sha_file, require_pinned, first_diff

PINS = {
    'group-a-sequences': '62c6f4c9297d760f1f14bde73ad15f62ca254371fa8b05f55c7ddf68e1d2e0bc',
    'group-b-hysteresis': '72e7c314e0e28a9831d1e9270045642b28ade24aec9363f550c825a6a6fa5857',
    'group-c-sdk-state': 'a718243eb1f0d9efdd1c8fb030720eff7d2321d20d24fad5860b26bb8f91a4e5',
}
GAP_PIN = 'dfb7cace17a73b7bfd1871be545d07acf14692adaafda25907370edec5b6dffb'
PROFILES = ['low', 'medium', 'high', 'filmscan1']
LIMITS = dict(zip(PROFILES[:3], [110000000, 130000000, 150000000]))
STATE = [0, 2, 3, 2, 1, 1, 0]
STATE_SOURCE = 'original SDK CSampleEncoder::m_encoder->q read immediately after CFHD_EncodeSample; no state injection'


def inventory(root, pin):
    if sha_file(root/'TRANSFER_SHA256.tsv') != pin:
        raise ValueError('transfer index pin mismatch')
    index = load_transfer_index(root)
    for rel in index:
        verified(root, rel, index)
    return index


def validate_sessions(manifest, group):
    state = group == 'group-c-sdk-state'
    upstream = 'sdk_upstream_commit' if state else 'upstream_commit'
    if manifest.get('schema') != 1 or manifest.get(upstream) != FROZEN:
        raise ValueError('incompatible Windows SDK provenance')
    profiles = PROFILES if group == 'group-a-sequences' else PROFILES[:3]+(['low'] if group == 'group-b-hysteresis' else [])
    sessions = manifest['sessions']
    if [s['profile'] for s in sessions] != profiles:
        raise ValueError('session profile count/order mismatch')
    count = 120 if group == 'group-a-sequences' else 7 if state else None
    for number, session in enumerate(sessions):
        frames = session['frames']
        if count is None:
            # Three 43-frame sweeps and the separate 16-frame Low calibration.
            expected = 16 if number == 3 else 43
            if session.get('sequence') != ('hold-calibration' if number == 3 else 'hold-ramp'):
                raise ValueError('group B sequence mismatch')
        else:
            expected = count
        trace_contract(frames, expected)
        if state:
            if session.get('state_source') != STATE_SOURCE or session.get('limit_bits_per_second') != LIMITS[session['profile']]:
                raise ValueError('missing directly observed SDK state provenance')
            if [f['sdk_overbitrate_after_encode'] for f in frames] != STATE:
                raise ValueError('direct SDK state trajectory mismatch')
            for i, f in enumerate(frames):
                previous = frames[i-1]['sdk_byte_length'] if i else None
                if (f['previous_sdk_sample_bytes'] != previous or
                    f['previous_bitrate_30fps'] != (previous*240 if i else None) or
                    f['sdk_lastgopbitcount_after_encode'] != f['sdk_byte_length']*8):
                    raise ValueError('direct SDK rate/size trajectory mismatch')
            limit = LIMITS[session['profile']]
            if not (frames[1]['previous_bitrate_30fps'] > limit and
                    frames[6]['previous_bitrate_30fps'] < limit*8//10):
                raise ValueError('missing actual SDK enter/leave rates')
            if not all(limit*8//10 <= frames[i]['previous_bitrate_30fps'] <= limit for i in (3, 4)):
                raise ValueError('missing consecutive actual 80-100 percent hold frames')
    return sessions


def source_inputs(root, index, gap_root, gap_index, frames):
    """Bind reused bytes to the independent Linux index; replay derived RGB bytes."""
    cache = {}
    def original(rel):
        if rel not in cache:
            cache[rel] = verified(gap_root, rel, gap_index).read_bytes()
        return cache[rel]
    for f in frames:
        path = verified(root, f['input'], index)
        require_pinned(path, f['input_byte_length'], f['input_sha256'], index, root, 'Windows input')
        rel = f['input']
        if rel.startswith('inputs/sequence-120f/'):
            expected = original(rel)
        elif rel.startswith('inputs/hold-ramp/amplitude_'):
            percent = int(Path(rel).stem.split('_')[1])
            if not 0 <= percent <= 100:
                raise ValueError('invalid amplitude')
            key = ('amplitude', percent)
            if key not in cache:
                data = bytearray(original('inputs/sequence-120f/frame_0006.bgra'))
                lut = bytes((v*percent+128*(100-percent))//100 for v in range(256))
                for channel in range(3):
                    data[channel::4] = data[channel::4].translate(lut)
                cache[key] = data
            expected = cache[key]
        else:
            raise ValueError('unqualified input provenance')
        if path.read_bytes() != expected:
            raise ValueError('Windows input derivation differs from pinned original')


def packet_contract(data, frame, code):
    q, s, tags, masks = packet_tables(data)
    bands = []
    for band in sample.parse_sample(data)['bands']:
        values = {}
        for offset in range(band['frame_start'], band['chunk_offset'], 4):
            tag, value = struct.unpack_from('>hH', data, offset)
            values[tag] = value
        bands.append(dict(channel=band['channel'], subband=band['subband'],
                          quant_53=values.get(53), scale_54=values.get(54)))
    if (bands != frame['packet_band_quant_scale'] or masks != frame['metadata_mask_ranges'] or
        struct.unpack_from('>hH', data, 84) != (-80, code) or frame['sdk_profile_tag_minus_80'] != code):
        raise ValueError('SDK packet tags/profile/masks differ from manifest')
    return q, s


def compare(root, index, session, encoder, directory, state):
    profile = session['profile']; frames = session['frames']; code = PROFILES.index(profile)+1
    packets = []
    for i, f in enumerate(frames):
        path = verified(root, f['sdk_packet'], index)
        require_pinned(path, f['sdk_byte_length'], f['sdk_sha256'], index, root, 'Windows SDK packet')
        data = path.read_bytes(); packet_contract(data, f, code); packets.append(data)
        if f['previous_sdk_sample_bytes'] != (frames[i-1]['sdk_byte_length'] if i else None):
            raise ValueError('SDK previous sample order mismatch')
    if state:
        trace = verified(root, profile+'/trace.tsv', index).read_text().splitlines()
        expected = ['%d	%d	%d	%d	%d	frame_%d.cfhd' % (
            f['sdk_overbitrate_after_encode'], f['sdk_rate_limiter_after_encode'],
            f['sdk_lastgopbitcount_after_encode'], i, f['sdk_byte_length'], i) for i, f in enumerate(frames)]
        if trace != expected:
            raise ValueError('direct SDK trace differs from manifest')
    if directory.exists():
        raise ValueError('writer output already exists')
    prefix = [sys.executable, str(encoder)] if encoder.suffix == '.py' else [str(encoder)]
    subprocess.run(prefix+['--coefficient-hash', 'off', '--quality', profile, str(directory)]+
                   [str(root/f['input']) for f in frames], check=True, capture_output=True, text=True)
    trace = json.loads((directory/'trace.json').read_text())['frames']; trace_contract(trace, len(frames))
    rows = []
    for i, (f, actual, sdk) in enumerate(zip(frames, trace, packets)):
        ours = (directory/('frame_%04d.cfhd'%i)).read_bytes()
        q, s = packet_tables(ours)[:2]; sdk_q, sdk_s = packet_tables(sdk)[:2]
        a = sample.normalized_sample(sdk, sample.parse_sample(sdk), f['metadata_mask_ranges'])
        b = sample.normalized_sample(ours, sample.parse_sample(ours), f['metadata_mask_ranges'])
        state_equal = (not state or
                       (actual['fs_overbitrate'] == f['sdk_overbitrate_after_encode'] and
                        actual['fs_rate_limiter'] == f['sdk_rate_limiter_after_encode'] and
                        actual['previous_sample_bytes'] == (f['previous_sdk_sample_bytes'] or 0)))
        rows.append(dict(frame_index=i, equal=a == b, tables_equal=q == sdk_q and s == sdk_s,
                         state_equal=state_equal, first_diff=first_diff(a, b),
                         sdk_bytes=len(sdk), writer_bytes=len(ours),
                         sdk_overbitrate=f.get('sdk_overbitrate_after_encode'),
                         writer_overbitrate=actual['fs_overbitrate'],
                         sdk_previous_sample_bytes=f['previous_sdk_sample_bytes'],
                         writer_previous_sample_bytes=actual['previous_sample_bytes']))
    return dict(status='PASS' if all(r['equal'] and r['tables_equal'] and r['state_equal'] for r in rows) else 'FAIL',
                matched=sum(r['equal'] for r in rows), requested=len(rows), frames=rows,
                hold_witness_frames=[3, 4] if state else None)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--corpus', type=Path, required=True)
    p.add_argument('--gap-corpus', type=Path, required=True)
    p.add_argument('--group', choices=list(PINS), required=True)
    p.add_argument('--encoder', type=Path, required=True)
    p.add_argument('--work', type=Path, required=True)
    p.add_argument('--report', type=Path, required=True)
    a = p.parse_args(); root = (a.corpus/a.group).resolve(); gap = a.gap_corpus.resolve()
    index = inventory(root, PINS[a.group]); gap_index = inventory(gap, GAP_PIN)
    manifest = json.loads(verified(root, 'manifest.json', index).read_text())
    sessions = validate_sessions(manifest, a.group)
    a.work.mkdir(parents=True, exist_ok=True)
    run = Path(tempfile.mkdtemp(prefix='run-', dir=str(a.work)))
    out = {}
    for session in sessions:
        source_inputs(root, index, gap, gap_index, session['frames'])
        profile = session['profile']
        key = profile+'/'+session.get('sequence', 'direct-state')
        out[key] = compare(root, index, session, a.encoder.resolve(), run/key, a.group == 'group-c-sdk-state')
        print(key, out[key]['status'], str(out[key]['matched'])+'/'+str(out[key]['requested']), flush=True)
    a.report.parent.mkdir(parents=True, exist_ok=True)
    a.report.write_text(json.dumps(out, indent=1)+chr(10))
    return int(any(r['status'] != 'PASS' for r in out.values()))

if __name__ == '__main__':
    raise SystemExit(main())
