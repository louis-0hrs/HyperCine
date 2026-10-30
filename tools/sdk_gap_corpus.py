#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
"""Capture original SDK packets; Python hashlib is the portable SHA-256 layer.

No HyperCine writer is imported or invoked. Inputs are deterministic replays of
16 verified source archetypes, with a frame identity stamp, plus one predeclared
noise-amplitude ramp for the hold-band experiment (never a synthetic packet).
"""
import argparse
import hashlib
import json
import struct
import subprocess
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tests"))
import reference_sample as sample
sys.path.insert(0, str(Path(__file__).resolve().parent))
from p6_writer_parity import load_transfer_index, require_pinned, sha_file

PROFILES = ("low", "medium", "high", "filmscan1")
FROZEN = "11574d0295771edccadd17af14af74a539f924c7"


def packet_tables(data):
    """Read actual tag 34/53/54 values, not computed expected tables.

    Packet subbands 1..3 are level 3, 4..6 level 2, 7..9 level 1.
    Intermediate lowpass slots (not encoded) are deliberately left null.
    """
    parsed = sample.parse_sample(data)
    quant, scale, tags = [None]*36, [None]*36, []
    for band in parsed['bands']:
        fields = {}
        for at in range(band['frame_start'], band['chunk_offset'], 4):
            tag, value = struct.unpack_from('>hH', data, at)
            if tag in (34, 53, 54):
                fields[tag] = value
                tags.append({'offset': at, 'tag': tag, 'value': value,
                             'channel': band['channel'], 'subband': band['subband']})
        c, s = band['channel'], band['subband']
        if not s:
            if 34 not in fields: raise ValueError('missing lowpass quant tag')
            quant[c*12+8] = fields[34]
        else:
            if 53 not in fields or 54 not in fields: raise ValueError('missing quant/scale tag')
            slot = c*12 + (2-(s-1)//3)*4 + 1+(s-1)%3
            quant[slot], scale[slot] = fields[53], fields[54]
    return quant, scale, tags, parsed['metadata_mask_ranges']


def write_json(path, obj):
    path.write_bytes((json.dumps(obj, indent=1)+'\n').encode('utf-8'))


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--source', type=Path, required=True)
    p.add_argument('--capture', type=Path, required=True)
    p.add_argument('--out', type=Path, required=True)
    args = p.parse_args()
    generator_source=Path(__file__).read_bytes()
    capture_source=Path(__file__).with_name('sdk_packet_capture.cpp').read_bytes()
    generator_sha256=hashlib.sha256(generator_source).hexdigest()
    capture_source_sha256=hashlib.sha256(capture_source).hexdigest()
    if args.out.exists(): raise SystemExit('output must not exist')
    root = args.source.resolve(); index = load_transfer_index(root)
    base = root/'low'/'1080p-16f'
    rel='low/1080p-16f/manifest.json'
    if rel not in index: raise ValueError('source manifest absent from inventory')
    size,digest=index[rel]
    require_pinned(base/'manifest.json',size,digest,index,root,'source manifest')
    manifest = json.loads((base/'manifest.json').read_text())
    if [f['frame_index'] for f in manifest['frames']]!=list(range(16)):
        raise ValueError('source archetype count/order mismatch')
    archetypes = []
    for f in manifest['frames'][:16]:
        path = next(base.glob('frames/*/frame_%04d/input.bgra' % f['frame_index']))
        require_pinned(path, f['input_byte_length'], f['input_sha256'], index, root, 'archetype')
        archetypes.append(path.read_bytes())
    args.out.mkdir(parents=True)
    provenance=args.out/'provenance';provenance.mkdir()
    (provenance/'sdk_packet_capture.cpp').write_bytes(capture_source)
    (provenance/'sdk_gap_corpus.py').write_bytes(generator_source)
    sequences = {}
    for kind, count in [('sequence-120f',120), ('hold-ramp',40)]:
        folder = args.out/'inputs'/kind; folder.mkdir(parents=True)
        paths = []
        for f in range(count):
            if kind == 'sequence-120f':
                data = bytearray(archetypes[f % 16])
            else:
                # Full-field deterministic grayscale noise, decreasing amplitude
                # in fixed steps after entry. A negative hold result stays negative.
                amplitude = [128,112,96,80,64,56,48,40,32,24][f//4]
                row = bytearray(1920*4); data = bytearray()
                rng = (0x1337BEEF ^ ((f+1)*0x9e3779b9)) & 0xffffffff
                for y in range(1080):
                    for x in range(1920):
                        rng ^= (rng << 13)&0xffffffff; rng ^= rng >> 17; rng ^= (rng << 5)&0xffffffff
                        value = max(16,min(235,128+((rng&255)-128)*amplitude//128))
                        row[x*4:x*4+4] = bytes((value,value,value,255))
                    data.extend(row)
            # Frame-unique 4:2:2 aligned binary stamp; no source generator claims.
            for bit in range(16):
                value = 216 if f & (1<<bit) else 40
                for y in range(16):
                    at = (y*1920+bit*4)*4
                    data[at:at+16] = bytes((value,value,value,255))*4
            path = folder/('frame_%04d.bgra'%f); path.write_bytes(data); paths.append(path)
        sequences[kind] = paths
    for profile in PROFILES:
        for kind, inputs in sequences.items():
            if kind == 'hold-ramp' and profile == 'filmscan1': continue
            target = args.out/profile/kind; target.mkdir(parents=True)
            packets = target/'packets'
            subprocess.run([str(args.capture.resolve()),str(PROFILES.index(profile)+1),str(packets)]+[str(x) for x in inputs],check=True)
            frames=[]
            for f,path in enumerate(inputs):
                packet=packets/('frame_%d.cfhd'%f); data=packet.read_bytes()
                q,s,tags,masks=packet_tables(data)
                frames.append({'frame_index':f,'input':path.relative_to(args.out).as_posix(),
                    'input_sha256':sha_file(path),'input_byte_length':path.stat().st_size,
                    'packet':packet.relative_to(args.out).as_posix(),'sdk_sha256':sha_file(packet),
                    'sdk_byte_length':len(data),'packet_quant':q,'packet_scale':s,
                    'packet_tags':tags,'metadata_mask_ranges':masks,
                    'previous_sample_bitrate':0 if not frames else frames[-1]['sdk_byte_length']*240})
            write_json(target/'manifest.json',{'schema_version':1,'profile':profile,'kind':kind,
                'sdk':{'implementation':'original CFHD_EncodeSample','frozen_ref':FROZEN},
                'geometry':[1920,1080],'timebase':'SDK default 30 fps','frames':frames})
    write_json(args.out/'manifest.json',{'schema_version':1,'profiles':list(PROFILES),
        'sequences':{'sequence-120f':120,'hold-ramp':40},'source_transfer_sha256':sha_file(root/'TRANSFER_SHA256.tsv'),
        'capture_sha256':sha_file(args.capture),'generator_sha256':generator_sha256,
        'capture_source_sha256':capture_source_sha256,
        'frozen_source_sha256':{p:sha_file(Path(__file__).resolve().parents[1]/p) for p in
            ['upstream/EncoderSDK/CFHDEncoder.cpp','upstream/EncoderSDK/SampleEncoder.cpp',
             'upstream/Codec/encoder.c','upstream/Codec/quantize.c','upstream/Codec/config.h']}})
    records=[]
    for path in sorted(args.out.rglob('*')):
        if path.is_file(): records.append('%s	%d	%s' % (path.relative_to(args.out).as_posix(),path.stat().st_size,sha_file(path)))
    (args.out/'TRANSFER_SHA256.tsv').write_bytes(('\n'.join(records)+'\n').encode('ascii'))
    print('transfer_sha256='+sha_file(args.out/'TRANSFER_SHA256.tsv'))

if __name__ == '__main__': main()
