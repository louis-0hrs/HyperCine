# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE.
# Historical ledger predicates, made explicit so python -O cannot remove them.

def require(condition, predicate):
    if not condition:
        raise ValueError(predicate)

def verify(calls, summary):
    require(len(calls) == len(summary) == 28, 'len(calls) == len(summary) == 28')
    require(len({c['name'] for c in calls}) == 28, "len({c['name'] for c in calls}) == 28")
    for (c, s) in zip(calls, summary):
        require(c['name'] == s['name'] and c['exit'] == 0 and c['waits_ok'] and c['guards_ok'], "c['name'] == s['name'] and c['exit'] == 0 and c['waits_ok'] and c['guards_ok']")
        es = s['comparisons']
        require(all((e['first_difference'] is None for e in es if e['index'] == 2)), "all((e['first_difference'] is None for e in es if e['index'] == 2))")
        if c['mode'] == 0 or c['mode'] == 2:
            require(s['pixel_equal'] and all((e['first_difference'] is None for e in es)), "s['pixel_equal'] and all((e['first_difference'] is None for e in es))")
        for ch in range(3):
            if not c['mask'] & 1 << ch:
                continue
            inputs = [e for e in es if e['phase'] == 0 and e['index'] == 1 and (e['ch'] == ch)]
            outputs = [e for e in es if e['phase'] == 1 and e['index'] == 1 and (e['ch'] == ch)]
            producers = [e for e in es if e['phase'] == 0 and e['index'] == 2 and (e['ch'] == ch)]
            require(inputs and outputs and producers, 'inputs and outputs and producers')
            first = inputs[0]
            pub = next((e for e in es if e['event'] == first['producer']))
            require(pub['ch'] == ch and pub['phase'] == 2 and (pub['index'] == 2) and pub['valid'] & 1, "pub['ch'] == ch and pub['phase'] == 2 and (pub['index'] == 2) and pub['valid'] & 1")
            if c['mode'] == 1:
                require(first['valid'] == first['started'] == 14 and first['first_difference'], "first['valid'] == first['started'] == 14 and first['first_difference']")
                require(first['first_difference']['coefficient'] == 0, "first['first_difference']['coefficient'] == 0")
                require(all((e['first_difference'] for e in outputs)), "all((e['first_difference'] for e in outputs))")
                require(first['event'] < outputs[0]['event'] < min((e['event'] for e in producers)) < pub['event'], "first['event'] < outputs[0]['event'] < min((e['event'] for e in producers)) < pub['event']")
                require(not s['pixel_equal'], "not s['pixel_equal']")
            if c['mode'] == 2:
                require(pub['event'] < first['event'] and first['valid'] & 1, "pub['event'] < first['event'] and first['valid'] & 1")
    return 'ROOT_CAUSE_CONFIRMED'
