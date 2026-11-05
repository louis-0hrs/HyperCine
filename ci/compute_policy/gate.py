# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE.
"""Lock-serialized queue submission, never VM execution or hosted fallback."""
from . import policy as p

SUBMISSION_TIMEOUT = 10


def _pause_if_current(state_path, generation):
    with p.journal(state_path) as store:
        state = store.load()
        # A later owner's decision must not be overwritten by an old worker.
        if generation is not None and generation == state['generation']:
            state['generation'] += 1;state['mode'] = p.PAUSED
            state['attention'] = True;state['verified_at'] = None;state['fallback_sha'] = None
            p.event(state, 'private_relay_failed', automatic_hosted_fallback=False)
            store.save(state)


def admit_local(api, cfg, state_path, host_probe, dispatch_private):
    """Submit once under the owner lock through a trusted bounded queue adapter.

    dispatch_private(*, timeout) must synchronously submit ONLY the queue request,
    enforce its deadline, and never wait for a VM/job or detach a background retry.
    An uncertain response is an error, never permission to resubmit automatically.
    """
    if cfg.get('enabled') is not True:
        return {'allowed': False, 'reason': 'policy-disabled'}
    generation = None
    try:
        snapshot = p.watch(api, state_path, enabled=True, host_probe=host_probe)
        generation = snapshot['generation']
        # This is the same canonical lock held by every owner transition. The
        # snapshot is only a generation fence; final admission uses fresh state.
        with p.journal(state_path) as store:
            state = store.load()
            live = {'inventory': p.inventory(api), 'security_digest': p.security(api)}
            if not p.admit(state, 'local', host_probe, generation=generation, live=live):
                return {'allowed': False, 'reason': 'owner-resume-required'}
            # Keep the lock through bounded queue submission, NOT job execution.
            dispatch_private(timeout=SUBMISSION_TIMEOUT)
            return {'allowed': True, 'generation': generation, 'destination': 'local'}
    except Exception:
        _pause_if_current(state_path, generation)
        return {'allowed': False, 'reason': 'relay-or-policy-unavailable', 'owner_attention_required': True}
