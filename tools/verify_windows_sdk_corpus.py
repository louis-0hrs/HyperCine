# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
"""Read-only issue-321 integrity/observation verifier; NOT a VC-5 validator.

No imports or execution of corpus-supplied code. Only Python standard library.
Exit 0 means integrity and declared structural assertions passed, not that
historical build provenance, playback cadence or input-to-packet causality did.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import struct
import subprocess
import sys

WINDOWS = 'golden/windows/qualities/2026-10-29-issue321-windows-sdk-reference'
LINUX = 'golden/windows/qualities/2026-10-29-sdk-gaps'
INDEXES = {
    'group-a-sequences': (606, '62c6f4c9297d760f1f14bde73ad15f62ca254371fa8b05f55c7ddf68e1d2e0bc'),
    'group-b-hysteresis': (177, '72e7c314e0e28a9831d1e9270045642b28ade24aec9363f550c825a6a6fa5857'),
    'group-c-sdk-state': (35, 'a718243eb1f0d9efdd1c8fb030720eff7d2321d20d24fad5860b26bb8f91a4e5'),
    'linux-sdk-gaps': (776, 'dfb7cace17a73b7bfd1871be545d07acf14692adaafda25907370edec5b6dffb'),
}
PROFILES = {'low': 1, 'medium': 2, 'high': 3, 'filmscan1': 4}
VARIABLE = {b'GUID': (16, 16, ord('G')), b'DATE': (1, 32, ord('c')),
            b'TIME': (1, 32, ord('c')), b'TIMC': (1, 32, ord('c'))}


def require(ok, reason):
    if not ok:
        raise ValueError(reason)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def file_sha(path):
    h = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(4 * 1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()


def safe_file(root, name):
    p = PurePosixPath(name)
    require(bool(name) and not p.is_absolute() and '\\' not in name and
            all(part not in ('', '.', '..') for part in name.split('/')),
            'unsafe relative file name')
    target = root.joinpath(*p.parts)
    require(not any(root.joinpath(*p.parts[:i]).is_symlink()
                    for i in range(1, len(p.parts) + 1)), 'symlink rejected')
    require(target.is_file(), 'missing required corpus file')
    return target


def verify_index(root, count=None, digest=None):
    index = safe_file(root, 'TRANSFER_SHA256.tsv')
    require(digest is None or file_sha(index) == digest, 'transfer index SHA-256 mismatch')
    rows = {}
    for line in index.read_text(encoding='utf-8').splitlines():
        fields = line.split('	')
        require(len(fields) == 3, 'malformed transfer row')
        name, size, expected = fields
        require(name not in rows and name != 'TRANSFER_SHA256.tsv', 'duplicate/self transfer entry')
        require(size.isdigit() and re.fullmatch('[0-9a-f]{64}', expected), 'invalid transfer value')
        path = safe_file(root, name)
        require(path.stat().st_size == int(size), 'indexed byte length mismatch: ' + name)
        require(file_sha(path) == expected, 'indexed SHA-256 mismatch: ' + name)
        rows[name] = (int(size), expected)
    actual = {p.relative_to(root).as_posix() for p in root.rglob('*') if p.is_file()}
    require(actual == set(rows) | {'TRANSFER_SHA256.tsv'}, 'unindexed or missing files')
    require(count is None or len(rows) == count, 'transfer file count mismatch')
    return rows


def parse_packet(data):
    """Walk sized chunks, never interpret entropy bytes as tags.

    Local bounded GOP1 dialect only. Header tag 69 and UFRM are independent
    packet order witnesses. SDK internal q fields are NOT serialized here.
    """
    require(len(data) >= 24 and len(data) % 4 == 0 and data[:4] == bytes((0, 1, 0, 9)),
            'unsupported sample signature/alignment')
    tags, metadata, bands, masks = [], [], [], []
    state = {'channel': 0, 'band': None, 'quant': None, 'scale': None}

    def pair(at):
        return struct.unpack_from('>hH', data, at)

    def walk(start, stop, depth=0):
        require(depth < 6, 'excessive packet nesting')
        at = start
        while at < stop:
            require(at + 4 <= stop, 'truncated tag')
            tag, value = pair(at)
            code = abs(tag)
            group = code & 0xff00
            if tag < 0 and group in (0x2000, 0x2100, 0x2200):
                end = at + 4 + (((code & 255) << 16) | value) * 4
                require(at + 4 <= end <= stop, 'chunk beyond parent')
                if group == 0x2000:
                    low = state['band'] == 0
                    require(end - at >= 12, 'short band')
                    width = state.get(27 if low else 49)
                    height = state.get(28 if low else 50)
                    require(width and height, 'missing band geometry')
                    require(not low or end-at-12 == width*height*2, 'lowpass storage size mismatch')
                    require(pair(at + 4) == ((4, 0x0f0f) if low else (55, 0)) and
                            pair(end - 4) == ((4, 0x1b4b) if low else (56, 0)),
                            'band delimiter mismatch')
                    bands.append({'channel': state['channel'], 'subband': state['band'],
                                  'quant_53': state['quant'], 'scale_54': state['scale'],
                                  'payload_sha256': sha(data[at+8:end-4]),
                                  'payload_bytes': end-at-12})
                else:
                    walk(at+4, end, depth+1)
                at = end
                continue
            if tag == -0x4002:
                end = at + 4 + value * 4
                require(end <= stop, 'metadata beyond parent')
                pos = at + 4
                while pos < end:
                    require(pos+8 <= end, 'short metadata record')
                    name = data[pos:pos+4]
                    size_type = struct.unpack_from('<I', data, pos+4)[0]
                    size = size_type & 0xffffff
                    padded = (size+3) & ~3
                    require(pos+8+padded <= end, 'metadata beyond chunk')
                    payload = data[pos+8:pos+8+size]
                    metadata.append((name.decode('ascii'), size_type >> 24, payload.hex()))
                    if name in VARIABLE:
                        lo, hi, typ = VARIABLE[name]
                        require(lo <= size <= hi and size_type >> 24 == typ,
                                'unexpected variable metadata')
                        masks.append((pos+8, padded))
                    pos += 8+padded
                at = end
                continue
            if tag == 2:
                require(value == 3 and at+16 <= stop, 'invalid channel index')
                at += 16
                continue
            tags.append((at, tag, value))
            if tag == 62:
                state['channel'] = value
            if tag in (25, 48):
                state.update(band=0 if tag == 25 else None, quant=None, scale=None)
            if tag == 51:
                state['band'] = value
            if tag in (27, 28, 49, 50):
                state[tag] = value
            if tag == 53:
                state['quant'] = value
            if tag == 54:
                state['scale'] = value
            at += 4
    walk(0, len(data))
    require([(b['channel'], b['subband']) for b in bands] ==
            [(c, b) for c in range(3) for b in range(10)], 'expected 30 ordered bands')
    normalized = bytearray(data)
    for start, size in masks:
        normalized[start:start+size] = bytes(size)
    return {'tags': tags, 'metadata': metadata, 'bands': bands,
            'normalized_sha256': sha(normalized), 'bytes': len(data)}


def observe_order(packet, index, profile):
    frame_numbers = [v for _, t, v in packet['tags'] if abs(t) == 69]
    unique = [int.from_bytes(bytes.fromhex(v), 'little') for n, typ, v in packet['metadata']
              if n == 'UFRM' and typ == ord('L') and len(v) == 8]
    require(frame_numbers == [index+1], 'packet frame-number order mismatch')
    require(unique == [index], 'packet UFRM order mismatch')
    require([v for _, t, v in packet['tags'] if t == -80] == [PROFILES[profile]],
            'packet profile mismatch')
    return {'frame_number_tag_69': frame_numbers[0], 'ufrm': unique[0]}


def checked_frame(root, rows, frame, packet_key):
    for key, prefix in [('input', 'input'), (packet_key, 'sdk')]:
        name = frame[key]
        require(name in rows, 'manifest references unindexed file')
        require(rows[name] == (frame[prefix+'_byte_length'], frame[prefix+'_sha256']),
                'manifest/index binding mismatch')
    data = safe_file(root, frame[packet_key]).read_bytes()
    return data, parse_packet(data)


def run(golden, repo):
    roots = {k: golden / WINDOWS / k for k in INDEXES if k != 'linux-sdk-gaps'}
    roots['linux-sdk-gaps'] = golden / LINUX
    indexes = {k: verify_index(root, *INDEXES[k]) for k, root in roots.items()}
    report = {'scope': 'integrity and independent bounded packet observations; not codec conformance',
              'indexes': {k: {'files_ok': len(v), 'index_sha256': file_sha(roots[k]/'TRANSFER_SHA256.tsv'),
                              'bytes_ok': sum(size for size, _ in v.values())}
                          for k, v in indexes.items()},
              'windows_sessions': [], 'state_trace': [], 'comparison': {}, 'differences': [], 'volatile_metadata_differences': [],
              'packet_frmr_records': [], 'provenance_verified': {}, 'frame_observations': [],
              'transfer_files': {k: {name: {'bytes': size, 'sha256': digest}
                                  for name, (size, digest) in rows.items()}
                                 for k, rows in indexes.items()},
              'limitations': [
                  'No FRMR in parsed metadata: actual playback framerate NOT_MEASURED.',
                  '30 fps bitrate below is a source-fallback scenario, not measured cadence.',
                  'overbitrate/FSratelimiter/lastgopbitcount come from indexed trace, NOT packets.',
                  'Trace/source consistency does not authenticate historic process execution.',
                  'Input bytes/order are bound by manifests; input-to-packet causality needs replay.',
                  'Windows static library and capture executables absent; hashes are assertions only.',
                  'Compiler, flags, link inputs, pristine source used by historic build are unproven.',
                  'Original generator seed, build timestamps and capture host are unproven.',
                  'Linux executable reproduction claim and generator hash cannot be checked here.',
              ]}
    observed = {}
    for group, root in roots.items():
        if group == 'linux-sdk-gaps':
            continue
        manifest = json.loads((root/'manifest.json').read_text())
        sessions = []
        signatures = [(s['profile'], s.get('sequence', 'state')) for s in manifest['sessions']]
        expected = ([(p, 'sequence-120f') for p in PROFILES] if group == 'group-a-sequences' else
                    ([(p, 'state') for p in ('low', 'medium', 'high')] if group == 'group-c-sdk-state' else
                     [(p, 'hold-ramp') for p in ('low', 'medium', 'high')]+[('low', 'hold-calibration')]))
        require(signatures == expected, 'missing/duplicated/reordered profile sessions')
        for session in manifest['sessions']:
            profile = session['profile']
            sequence = session.get('sequence', 'state')
            frames = session['frames']
            expected_count = 120 if group == 'group-a-sequences' else (
                7 if group == 'group-c-sdk-state' else (16 if sequence == 'hold-calibration' else 43))
            require(len(frames) == expected_count, 'session frame count mismatch')
            previous = None
            trace = None
            if group == 'group-c-sdk-state':
                trace = [line.split('	') for line in safe_file(root, profile+'/trace.tsv').read_text().splitlines()]
                require(len(trace) == len(frames), 'trace length mismatch')
            for index, frame in enumerate(frames):
                require(frame['frame_index'] == index, 'manifest order mismatch')
                noise = 'inputs/sequence-120f/frame_0006.bgra'
                flat = 'inputs/sequence-120f/frame_0000.bgra'
                if group == 'group-a-sequences':
                    expected_input = 'inputs/sequence-120f/frame_%04d.bgra' % index
                elif group == 'group-c-sdk-state':
                    amplitude = {'low': 13, 'medium': 15, 'high': 10}[profile]
                    expected_input = noise if index < 2 else (flat if index > 4 else
                        'inputs/hold-ramp/amplitude_%03d.bgra' % amplitude)
                elif sequence == 'hold-ramp':
                    expected_input = noise if index < 2 else (flat if index == 42 else
                        'inputs/hold-ramp/amplitude_%03d.bgra' % (100-5*((index-2)//2)))
                else:
                    expected_input = noise if index < 2 else (flat if index >= 14 else
                        'inputs/hold-ramp/amplitude_%03d.bgra' % (14-(index-2)//3))
                require(frame['input'] == expected_input, 'unexpected input sequence')
                data, packet = checked_frame(root, indexes[group], frame, 'sdk_packet')
                order = observe_order(packet, index, profile)
                require(frame['previous_sdk_sample_bytes'] == previous, 'previous packet length mismatch')
                quant = [{k: b[k] for k in ('channel', 'subband', 'quant_53', 'scale_54')}
                         for b in packet['bands']]
                require(quant == frame['packet_band_quant_scale'], 'manifest quant/scale mismatch')
                for name, typ, val in packet['metadata']:
                    if name == 'FRMR':
                        report['packet_frmr_records'].append({'group': group, 'profile': profile,
                                                              'frame': index, 'type': typ, 'hex': val})
                if trace is not None:
                    row = trace[index]
                    require(len(row) == 6, 'malformed state trace')
                    over, limiter, bits, ordinal, size = map(int, row[:5])
                    require((ordinal, size, row[5]) == (index, len(data), 'frame_%d.cfhd' % index),
                            'state trace order/size mismatch')
                    require(bits == len(data)*8, 'state lastgopbitcount differs from real packet')
                    for key, actual in [('sdk_overbitrate_after_encode', over),
                                        ('sdk_rate_limiter_after_encode', limiter),
                                        ('sdk_lastgopbitcount_after_encode', bits)]:
                        require(frame[key] == actual, 'manifest differs from raw trace')
                    rate = previous*8*30 if previous is not None else None
                    require(frame['previous_bitrate_30fps'] == rate, 'manifest bitrate mismatch')
                    report['state_trace'].append(dict(profile=profile, frame=index, bytes=len(data),
                        overbitrate_trace=over, fs_rate_limiter_trace=limiter,
                        lastgopbitcount_trace=bits, prior_bitrate_at_fallback_30=rate,
                        limit=session['limit_bits_per_second'], **order))
                report['frame_observations'].append(dict(group=group, profile=profile, sequence=sequence,
                    frame=index, input=frame['input'], bytes=len(data),
                    quant_scale=quant, **order))
                observed[(group, profile, sequence, index)] = (frame, packet)
                previous = len(data)
            sessions.append({'profile': profile, 'sequence': sequence, 'frames': len(frames),
                             'packet_ufrm_and_tag69_order_ok': True, 'manifest_input_hash_order_ok': True})
        report['windows_sessions'].append({'group': group, 'sessions': sessions})
    linux = roots['linux-sdk-gaps']
    lm = json.loads((linux/'manifest.json').read_text())
    frozen = lm['frozen_source_sha256']
    for name, digest in frozen.items():
        require(name.startswith('upstream/'), 'unexpected frozen source scope')
        require(file_sha(safe_file(repo, name)) == digest, 'frozen source SHA mismatch')
    require(file_sha(linux/'provenance/sdk_packet_capture.cpp') == lm['capture_source_sha256'],
            'Linux capture source SHA mismatch')
    tree = subprocess.check_output(['git', 'rev-parse', 'HEAD:upstream'], cwd=repo, text=True).strip()
    pristine = subprocess.check_output(['git', 'rev-parse',
        '11574d0295771edccadd17af14af74a539f924c7^{tree}'], cwd=repo, text=True).strip()
    require(tree == pristine, 'upstream tree differs from pristine SDK commit')
    for group in roots:
        if group == 'linux-sdk-gaps':
            continue
        wm = json.loads((roots[group]/'manifest.json').read_text())
        require(wm.get('upstream_tree', wm.get('sdk_upstream_tree')) == tree, 'declared SDK tree mismatch')
    report['provenance_verified'] = {'frozen_source_sha256': frozen, 'upstream_tree': tree,
        'linux_capture_source_sha256': lm['capture_source_sha256'],
        'capture_source_semantics': 'inspected separately; no execution of corpus source'}
    for profile in PROFILES:
        path = profile+'/sequence-120f/manifest.json'
        require(path in indexes['linux-sdk-gaps'], 'unindexed Linux session manifest')
        frames = json.loads((linux/path).read_text())['frames']
        require(len(frames) == 120, 'Linux frame count mismatch')
        summary = {'frames': 120, 'input_equal': 0, 'raw_packet_equal': 0,
                   'normalized_packet_equal': 0, 'quant_scale_equal': 0, 'size_equal': 0}
        previous = 0
        for index, frame in enumerate(frames):
            require(frame['frame_index'] == index, 'Linux manifest order mismatch')
            data, packet = checked_frame(linux, indexes['linux-sdk-gaps'], frame, 'packet')
            observe_order(packet, index, profile)
            require(frame['previous_sample_bitrate'] == previous*8*30, 'Linux previous bitrate mismatch')
            previous = len(data)
            wf, wp = observed[('group-a-sequences', profile, 'sequence-120f', index)]
            require(frame['input'] == wf['input'] and frame['input_sha256'] == wf['input_sha256'],
                    'Windows/Linux input/order differs')
            summary['input_equal'] += 1
            summary['raw_packet_equal'] += wf['sdk_sha256'] == frame['sdk_sha256']
            summary['size_equal'] += wp['bytes'] == packet['bytes']
            summary['normalized_packet_equal'] += wp['normalized_sha256'] == packet['normalized_sha256']
            volatile = []
            for name in sorted(key.decode('ascii') for key in VARIABLE):
                wvalues = [(typ, val) for n, typ, val in wp['metadata'] if n == name]
                lvalues = [(typ, val) for n, typ, val in packet['metadata'] if n == name]
                if wvalues != lvalues:
                    volatile.append({'field': name, 'windows_value_sha256': sha(repr(wvalues).encode()),
                                     'linux_value_sha256': sha(repr(lvalues).encode())})
            report['volatile_metadata_differences'].append({'profile': profile, 'frame': index,
                                                           'fields': volatile})
            fields = []
            wtags = [(t, value) for _, t, value in wp['tags']]
            ltags = [(t, value) for _, t, value in packet['tags']]
            if wtags != ltags:
                fields.append({'field': 'structural_tag_sequence', 'windows': wtags, 'linux': ltags})
            variable_names = {key.decode('ascii') for key in VARIABLE}
            wmeta = [m for m in wp['metadata'] if m[0] not in variable_names]
            lmeta = [m for m in packet['metadata'] if m[0] not in variable_names]
            if wmeta != lmeta:
                fields.append({'field': 'nonvolatile_metadata',
                               'windows_sha256': sha(repr(wmeta).encode()),
                               'linux_sha256': sha(repr(lmeta).encode())})
            for name, typ, val in packet['metadata']:
                if name == 'FRMR':
                    report['packet_frmr_records'].append({'group': 'linux-sdk-gaps', 'profile': profile,
                                                          'frame': index, 'type': typ, 'hex': val})
            if wp['bytes'] != packet['bytes']:
                fields.append({'field': 'packet_bytes', 'windows': wp['bytes'], 'linux': packet['bytes']})
            quant_equal = True
            for wb, lb in zip(wp['bands'], packet['bands']):
                for key in ('quant_53', 'scale_54', 'payload_bytes', 'payload_sha256'):
                    if wb[key] != lb[key]:
                        fields.append({'field': key, 'channel': wb['channel'], 'subband': wb['subband'],
                                       'windows': wb[key], 'linux': lb[key]})
                        if key in ('quant_53', 'scale_54'):
                            quant_equal = False
            summary['quant_scale_equal'] += quant_equal
            if fields or wp['normalized_sha256'] != packet['normalized_sha256']:
                report['differences'].append({'profile': profile, 'frame': index, 'fields': fields,
                    'windows_normalized_sha256': wp['normalized_sha256'],
                    'linux_normalized_sha256': packet['normalized_sha256']})
        report['comparison'][profile] = summary
    require(not report['packet_frmr_records'], 'unexpected FRMR: reassess cadence evidence')
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--golden-root', default=os.environ.get('HYPERCINE_GOLDEN_DIR'))
    parser.add_argument('--output', required=True, help='report destination outside the corpus')
    args = parser.parse_args()
    try:
        require(bool(args.golden_root), 'set HYPERCINE_GOLDEN_DIR or --golden-root')
        golden = Path(args.golden_root).resolve()
        output = Path(args.output).resolve()
        require(output != golden and golden not in output.parents, 'output must be outside corpus')
        require(not output.exists(), 'refuse existing report (avoid stale PASS ambiguity)')
        report = run(golden, Path(__file__).resolve().parents[1])
        output.write_text(json.dumps(report, indent=2, sort_keys=True)+chr(10), encoding='utf-8')
        print('PASS integrity: %d/%d indexed files; observations and limitations in report' %
              (sum(v['files_ok'] for v in report['indexes'].values()), sum(v['files_ok'] for v in report['indexes'].values())))
        return 0
    except (OSError, ValueError, KeyError, TypeError, struct.error, subprocess.SubprocessError):
        # Deliberately do not echo exceptions: OS diagnostics may contain private paths.
        print('FAIL corpus verification (missing/changed/unsupported evidence); no report written', file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
