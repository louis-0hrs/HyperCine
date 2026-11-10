#!/usr/bin/env python3
"""RCA #160, fourth pass — classify when the frame-plane reconstruction starts.

The output stage reads `transform[channel]->wavelet[frame]->band[0]`, the plane
produced by the reconstruction of wavelet index 1 from the band-0 plane of
wavelet index 2 (the lowpass). This tool answers one question per channel:

  When the reconstruction of index 1 was entered, had index 2 already published
  its band 0 — and what did the input plane contain at that moment?

Input is the probe log written by the private instrumentation copy
(`P0` entry records, `P1a` post-transform records, `P4` reuse/allocation
records). Output is a sanitised JSON summary: relative call ids, digests,
counts. No host paths, no raw pixels.

Stdlib only, per repository rule 3.
"""
import collections
import hashlib
import json
import os
import re
import sys

P0 = re.compile(
    r'^P0 ch=(\d+) idx=(\d+) ttype=\d+ alloc=(\d+) w=(\d+) h=(\d+) pitch=(\d+) '
    r'lvl=(\d+) nb=\d+ flags=(\w+) started=(\w+) img=(\w+) in0=(\w+)')
P1A = re.compile(
    r'^P1a ch=(\d+) idx=(\d+) out=(\d+) w=(\d+) h=(\d+) pitch=(\d+) lvl=(\d+) '
    r'flags=(\w+) img=(\w+) out0=(\w+)')
P4 = re.compile(
    r'^P4 reuse=(\d) img=(\w+) req=(\d+)x(\d+) reqlvl=(\d+) have=(-?\d+)x(-?\d+) '
    r'havelvl=(-?\d+) stale0=(\w+) flags=(\w+)')   # parsed, not attributed per channel


def read_call(path):
    """Return (ordered records, output digest). Records are tuples with a kind tag."""
    recs = []
    with open(path, errors='replace') as f:
        for pos, line in enumerate(f):
            m = P0.match(line)
            if m:
                recs.append(dict(kind='entry', pos=pos, ch=int(m[1]), idx=int(m[2]),
                                 alloc=int(m[3]), w=int(m[4]), h=int(m[5]),
                                 lvl=int(m[7]), flags=m[8], started=m[9],
                                 img=m[10], in0=m[11]))
                continue
            m = P1A.match(line)
            if m:
                recs.append(dict(kind='out', pos=pos, ch=int(m[1]), idx=int(m[2]),
                                 out=int(m[3]), w=int(m[4]), h=int(m[5]),
                                 lvl=int(m[7]), flags=m[8], img=m[9], dig=m[10]))
                continue
            m = P4.match(line)
            if m:
                recs.append(dict(kind='alloc', pos=pos, reuse=int(m[1]),
                                 req=(int(m[3]), int(m[4])), reqlvl=int(m[5]),
                                 stale=m[9], flags=m[10]))
    return recs


def channel_facts(recs, ch):
    """Facts for the reconstruction that produces the frame plane of `ch`."""
    entry = next((r for r in recs
                  if r['kind'] == 'entry' and r['ch'] == ch
                  and r['idx'] == 1 and r['alloc'] == 0), None)
    pub = next((r for r in recs
                if r['kind'] == 'out' and r['ch'] == ch
                and r['idx'] == 2 and r['out'] == 1), None)
    write = next((r for r in recs
                  if r['kind'] == 'out' and r['ch'] == ch
                  and r['idx'] == 1 and r['out'] == 0), None)
    if not entry or not write:
        return None

    # The P4 records carry no channel identity, so a stale-buffer attribution
    # cannot be made per channel here. Only the ordering fact is reported.
    return dict(
        entry_flags=entry['flags'],
        entry_started=entry['started'],
        band0_valid=bool(int(entry['flags'], 16) & 1),
        entry_pos=entry['pos'],
        publish_pos=pub['pos'] if pub else None,
        write_pos=write['pos'],
        publish_before_entry=bool(pub and pub['pos'] < entry['pos']),
        input_digest=entry['in0'],
        output_digest=write['dig'],
    )


def main(root):
    rows = []
    for name in sorted(os.listdir(root)):
        d = os.path.join(root, name)
        log = os.path.join(d, 'instr_call00.log')
        frame = os.path.join(d, 'frame00.bgra')
        if not (os.path.isdir(d) and os.path.exists(log) and os.path.exists(frame)):
            continue
        digest = hashlib.sha256(open(frame, 'rb').read()).hexdigest()[:16]
        recs = read_call(log)
        rows.append(dict(id=name, output=digest,
                         ch={str(c): channel_facts(recs, c) for c in (0, 1, 2)}))

    counts = collections.Counter(r['output'] for r in rows)
    modal = counts.most_common(1)[0][0]

    # A call is "premature" when a frame-plane reconstruction was entered
    # before its lowpass source was published.
    for r in rows:
        r['premature'] = sorted(
            int(c) for c, f in r['ch'].items()
            if f and not f['publish_before_entry'])
        r['corrupt'] = r['output'] != modal

    corrupt = [r for r in rows if r['corrupt']]
    clean = [r for r in rows if not r['corrupt']]
    tp = sum(1 for r in corrupt if r['premature'])
    fn = len(corrupt) - tp
    fp = sum(1 for r in clean if r['premature'])
    tn = len(clean) - fp

    per_channel = collections.Counter()
    for r in corrupt:
        for c in r['premature']:
            per_channel[c] += 1

    summary = dict(
        calls=len(rows),
        distinct_outputs=len(counts),
        output_counts=dict(counts),
        modal_output=modal,
        corrupt_calls=len(corrupt),
        rule=('frame-plane reconstruction (wavelet index 1) entered before the '
              'lowpass band 0 of wavelet index 2 was published'),
        contingency=dict(
            rule_true_and_corrupt=tp,
            rule_true_and_clean=fp,
            rule_false_and_clean=tn,
            rule_false_and_corrupt=fn,
        ),
        corrupt_with_premature_entry=tp,
        corrupt_channels=dict(sorted(per_channel.items())),
        corrupt_detail=[dict(
            id=r['id'], output=r['output'],
            premature_channels=r['premature'],
            channels={c: dict(band0_valid=f['band0_valid'],
                              entry_flags=f['entry_flags'],
                              input_digest=f['input_digest'],
                              output_digest=f['output_digest'])
                      for c, f in r['ch'].items()
                      if f and c in [str(x) for x in r['premature']]},
        ) for r in corrupt],
    )
    return summary


if __name__ == '__main__':
    data = main(sys.argv[1] if len(sys.argv) > 1 else '.')
    json.dump(data, sys.stdout, indent=1)
    sys.stdout.write('\n')
