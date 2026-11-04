# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE.
"""Read-only identity inventory and offline policy evaluator for PR #352.

This tool never creates checks, changes rules, merges a branch or grants access.
An offline PASS is not proof that GitHub merge protection accepted a check.
"""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import re

RELAY_CHECK = 'HyperCI / Mac relay'
LANES = ('standalone-release', 'standalone-sanitizers', 'quality-state')
LEGACY_MAC_NAMES = (
    'Standalone full decode (macOS arm64 Release)',
    'Standalone full decode (macOS arm64 ASan UBSan)',
    'Quality state (macOS arm64)',
)


def required_checks(rules):
    result = []
    for rule in rules:
        if rule.get('type') == 'required_status_checks':
            for check in (rule.get('parameters') or {}).get('required_status_checks', []):
                result.append({'context': check.get('context'), 'app_id': check.get('integration_id')})
    return sorted(result, key=lambda c: (str(c['context']), str(c['app_id'])))


def external_id(source_sha, nonce, run_id):
    if not re.fullmatch('[0-9a-f]{40}', source_sha or ''):
        raise ValueError('Invalid source SHA')
    if not re.fullmatch('[0-9a-f]{32}', nonce or '') or type(run_id) is not int or run_id <= 0:
        raise ValueError('Invalid dispatch binding')
    return f'hyperci-v1:{source_sha}:{nonce}:{run_id}'


def lane_outcome(jobs, run_id, run_complete=False):
    """Reporter contract: only all three actual successes may emit success.

    In particular, never map a cancelled/skipped/neutral lane to a completed
    neutral/skipped aggregate check: GitHub can accept those conclusions.
    """
    pending = False
    for lane in LANES:
        hits = [job for job in jobs if job.get('name') == lane]
        if len(hits) != 1:
            if len(hits) > 1 or run_complete:
                return 'failure'
            pending = True
            continue
        job = hits[0]
        if job.get('run_id') != run_id:
            return 'failure'
        if job.get('status') != 'completed':
            if run_complete:
                return 'failure'
            pending = True
        elif job.get('conclusion') != 'success':
            return 'failure'
    return 'pending' if pending else 'success'


def evaluate(snapshot, name, app_id, source_sha, binding):
    """Conservative expected policy; does not simulate GitHub's merge engine."""
    if type(app_id) is not int or app_id <= 0:
        return {'accepted': False, 'reason': 'reporter_app_not_configured'}
    if not re.fullmatch('[0-9a-f]{40}', source_sha or '') or not binding:
        return {'accepted': False, 'reason': 'invalid_source_binding'}
    required = snapshot.get('required_checks', [])
    if not any(c.get('context') == name and c.get('app_id') == app_id for c in required):
        return {'accepted': False, 'reason': 'required_app_identity_missing'}
    # A commit status with matching display text is deliberately never consulted.
    checks = [c for c in snapshot.get('checks', []) if c.get('name') == name
              and c.get('head_sha') == source_sha and c.get('app', {}).get('id') == app_id
              and c.get('external_id') == binding]
    if len(checks) != 1:
        return {'accepted': False, 'reason': 'exact_app_sha_binding_missing_or_ambiguous'}
    check = checks[0]
    accepted = check.get('status') == 'completed' and check.get('conclusion') == 'success'
    return {'accepted': accepted, 'reason': 'exact_check_success' if accepted else 'check_not_success'}


def inventory(api, repository, source_sha):
    if not re.fullmatch('[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+', repository):
        raise ValueError('Invalid repository')
    if not re.fullmatch('[0-9a-f]{40}', source_sha):
        raise ValueError('Invalid source SHA')
    rules = required_checks(api(f'repos/{repository}/rules/branches/main'))
    checks = []
    for page in range(1, 11):
        batch = api(f'repos/{repository}/commits/{source_sha}/check-runs?filter=latest&per_page=100&page={page}')['check_runs']
        for c in batch:
            checks.append({k: c.get(k) for k in ['id','name','head_sha','external_id','status','conclusion']} |
                          {'app': {'id': c.get('app', {}).get('id'), 'slug': c.get('app', {}).get('slug')}})
        if len(batch) < 100:
            break
    else:
        raise RuntimeError('Incomplete check inventory')
    # No raw environment, raw logs, credentials, private topology or PR bodies.
    return {'repository': repository, 'source_sha': source_sha,
            'recorded_at': datetime.datetime.now(datetime.timezone.utc).isoformat(),
            'data_source': 'GitHub REST read-only', 'required_checks': rules, 'checks': checks,
            'required_mac_checks': [c for c in rules if c['context'] in LEGACY_MAC_NAMES],
            'rules_fingerprint': hashlib.sha256(json.dumps(rules, sort_keys=True).encode()).hexdigest(),
            'live_protected_merge_acceptance': 'NOT_RUN'}
