# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE.
"""Worker admission seam. No implicit passthrough and no hosted fallback."""
from . import policy as p


def admit_local(api, cfg, state_path, host_probe, dispatch_private):
    if cfg.get('enabled') is not True:
        return {'allowed': False, 'reason': 'policy-disabled'}
    generation = None
    try:
        state = p.watch(api, state_path, enabled=True, host_probe=host_probe)
        generation = state['generation']
        if not p.admit(state, 'local', host_probe, generation=generation, live=p.inspect(api)):
            return {'allowed': False, 'reason': 'owner-resume-required'}
        dispatch_private()
        return {'allowed': True, 'generation': generation, 'destination': 'local'}
    except Exception:
        with p.journal(state_path) as store:
            state = store.load()
            # A later owner's decision must not be overwritten by an old worker.
            if generation is not None and generation == state['generation']:
                state['generation'] += 1;state['mode'] = p.PAUSED
                state['attention'] = True;state['verified_at'] = None
                p.event(state, 'private_relay_failed', automatic_hosted_fallback=False)
                store.save(state)
        return {'allowed': False, 'reason': 'relay-or-policy-unavailable', 'owner_attention_required': True}
