# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE.
# Historical ledger predicates, made explicit so python -O cannot remove them.

def require(condition, predicate):
    if not condition:
        raise ValueError(predicate)

def verify(plan, calls):
    require(plan['repetitions'] == 8 and plan['packet_size'] == 900396, "plan['repetitions'] == 8 and plan['packet_size'] == 900396")
    require(len(calls) == 96 and len({c['name'] for c in calls}) == 96, "len(calls) == 96 and len({c['name'] for c in calls}) == 96")
    require(all((c['exit'] == 0 and c['sha256'] for c in calls)), "all((c['exit'] == 0 and c['sha256'] for c in calls))")
    report = []
    for resolution in ['full', 'half']:

        def group(kind, workers, mode):
            result = [c for c in calls if (c['resolution'], c['kind'], c['workers'], c['delay']) == (resolution, kind, workers, mode)]
            require(len(result) == 8, 'len(result) == 8')
            return result
        stock = group('stock', '1', '0')
        reference = group('probe', '1', '0')
        require(len({c['sha256'] for c in stock + reference}) == 1, "len({c['sha256'] for c in stock + reference}) == 1")
        planes = {e['ch']: e['hash'] for e in reference[0]['events'] if e['after'] == '1' and e['index'] == '1'}
        require(set(planes) == {'0', '1', '2'}, "set(planes) == {'0', '1', '2'}")
        for mode in ['0', '1', '2']:
            arm = group('probe', '8', mode)
            bad = {ch: 0 for ch in planes}
            for c in arm:
                events = c['events']
                require(len(events) < 128, 'len(events) < 128')
                require(all((e['type'] == '0' for e in events if e['after'] == '0')), "all((e['type'] == '0' for e in events if e['after'] == '0'))")
                for (ch, correct) in planes.items():
                    hashes = [e['hash'] for e in events if e['after'] == '1' and e['index'] == '1' and (e['ch'] == ch)]
                    require(hashes, 'hashes')
                    bad[ch] += any((h != correct for h in hashes))
                if mode == '1':
                    require(c['wait_signalled'], "c['wait_signalled']")
                    consumer = next((e for e in events if e['after'] == '0' and e['ch'] == '1' and (e['index'] == '1')))
                    consumed = next((e for e in events if e['after'] == '1' and e['ch'] == '1' and (e['index'] == '1')))
                    producer = next((e for e in events if e['after'] == '0' and e['ch'] == '1' and (e['index'] == '2')))
                    require(consumer['valid'] == consumer['started'] == '14', "consumer['valid'] == consumer['started'] == '14'")
                    require(int(consumer['event']) < int(consumed['event']) < int(producer['event']), "int(consumer['event']) < int(consumed['event']) < int(producer['event'])")
                    require(int(consumer['producer_event']) > int(producer['event']), "int(consumer['producer_event']) > int(producer['event'])")
                elif mode == '2':
                    require(c['wait_signalled'], "c['wait_signalled']")
            if mode == '1':
                require(bad['1'] == 8, "bad['1'] == 8")
            if mode == '2':
                require(bad['1'] == 0, "bad['1'] == 0")
            report.append(dict(resolution=resolution, mode=mode, bad_lowpass_planes=bad, differing_pixels=sum((c['sha256'] != stock[0]['sha256'] for c in arm))))
    return report
