# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE.
"""Explicit local owner CLI. Nothing is installed or periodically scheduled."""
import argparse
import json
import os
from pathlib import Path
import signal
import stat
from . import policy as p
from .api import GitHub


def owned_json(path):
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    try:
        info = os.fstat(fd)
        if (not stat.S_ISREG(info.st_mode) or info.st_uid != os.geteuid()
                or stat.S_IMODE(info.st_mode) != 0o600 or info.st_nlink != 1 or info.st_size > 65536):
            raise RuntimeError('Owned private regular JSON file required')
        with os.fdopen(fd, 'r') as stream:
            fd = None
            return json.load(stream)
    finally:
        if fd is not None:
            os.close(fd)


def main(argv=None, api_factory=GitHub):
    os.umask(0o077)
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--config', type=Path, required=True)
    parser.add_argument('--state', type=Path, required=True)
    sub = parser.add_subparsers(dest='action', required=True)
    sub.add_parser('inspect')
    plan = sub.add_parser('plan')
    plan.add_argument('--generation', type=int, required=True)
    plan.add_argument('--target', choices=sorted(p.MODES), required=True)
    plan.add_argument('--candidate-sha')
    plan.add_argument('--reconcile', action='store_true')
    plan.add_argument('--reason', required=True)
    execute = sub.add_parser('execute')
    execute.add_argument('--generation', type=int, required=True)
    execute.add_argument('--plan', required=True)
    execute.add_argument('--execute', action='store_true')
    execute.add_argument('--allow-hosted', action='store_true')
    watch = sub.add_parser('watch-once');watch.add_argument('--execute', action='store_true')
    admission = sub.add_parser('admit')
    admission.add_argument('--generation', type=int, required=True)
    admission.add_argument('--destination', choices=['local', 'hosted'], required=True)
    admission.add_argument('--candidate-sha')
    a = parser.parse_args(argv)
    cfg = owned_json(a.config)
    if set(cfg) != {'enabled', 'host_status'} or type(cfg['enabled']) is not bool:
        raise RuntimeError('Invalid policy configuration')
    writing = a.action in {'execute', 'watch-once'}
    if writing and (not a.execute or cfg['enabled'] is not True):
        raise RuntimeError('Explicit execution and enabled reviewed deployment required')
    api = api_factory()  # This adapter is GET-only, even for local state changes.
    probe = lambda: owned_json(Path(cfg['host_status'])) if cfg.get('host_status') else {}
    if a.action == 'inspect':
        with p.journal(a.state) as store:
            state = store.load()
        facts = p.inspect(api)
        active = all(v['state'] == 'active' for v in facts['inventory'].values())
        result = {'generation': state['generation'], 'mode': state['mode'], 'attention': state['attention'],
                  'live': facts, 'last_verified': state['verified_at'],
                  'hosted_non_mac_workflows_active': active, 'inventory_matches_journal': facts['inventory'] == state['expected']}
    elif a.action == 'plan':
        result = p.make_plan(api, a.state, a.generation, a.target, reconcile=a.reconcile,reason=a.reason,fallback_sha=a.candidate_sha)
    elif a.action == 'execute':
        with p.journal(a.state) as store:
            state = store.load();plan = state['plans'].get(a.plan)
        if not plan or (plan['target'] == 'owner-authorized-hosted' and not a.allow_hosted):
            raise RuntimeError('Unknown plan or missing explicit hosted acknowledgment')
        result = p.execute(api, a.state, a.plan, a.generation, enabled=True, host_probe=probe)
    elif a.action == 'watch-once':
        result = p.watch(api, a.state, enabled=True, host_probe=probe)
    else:
        with p.journal(a.state) as store:
            state = store.load()
        result = {'allowed': p.admit(state, a.destination, probe, generation=a.generation, live=p.inspect(api),candidate_sha=a.candidate_sha),
                  'generation': state['generation'], 'mode': state['mode']}
    print(json.dumps(result, indent=2))
    return result


def interrupted(signum, frame):
    raise InterruptedError('Operator signal; no automatic hosted resume')


if __name__ == '__main__':
    signal.signal(signal.SIGTERM, interrupted);signal.signal(signal.SIGINT, interrupted)
    try:
        result = main()
        if result.get('allowed') is False:
            raise SystemExit(3)
    except (RuntimeError, ValueError, OSError, KeyboardInterrupt) as exc:
        # Avoid tracebacks containing host paths, argv or credentials.
        print(json.dumps({'stopped': True, 'error_type': type(exc).__name__, 'owner_attention_required': True}))
        raise SystemExit(2)
