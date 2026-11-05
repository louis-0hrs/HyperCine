# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE.
"""Serialized owner decisions. Never dispatches builds or edits required rules."""
import copy
import hashlib
import json
import math
import re
import time
from ci.premerge_controller.state_store import locked_state

REPOSITORY = 'louis-0hrs/HyperCine'
REPOSITORY_ID = 1402247945
OWNER = 'louis-0hrs'
OWNER_ID = 248477380
COMPUTE = {
    373401385: '.github/workflows/ci-cpu.yml',
    395280516: '.github/workflows/ci-cuda-build.yml',
    385851595: '.github/workflows/ci-standalone.yml',
    386265774: '.github/workflows/quality-state.yml',
}
PRIVACY_ID = 386331718
PRIVACY_BLOB = 'daab199d71ee447f035f3db629a08692cc30c354'
PREFIX = 'repos/' + REPOSITORY
PAUSED = 'paused/host-unavailable'
MODES = {PAUSED, 'normal', 'owner-authorized-hosted', 'recovering'}
SECURITY_CONTEXTS = {
    'Publication privacy (all new commits)': 15368,
    'Installed privacy hooks (Windows)': 15368,
    'Workflow routing contract (static)': 15368,
}
DISABLED = {'disabled_manually', 'disabled_inactivity', 'disabled_fork'}


def digest(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True, separators=(',', ':'), allow_nan=False).encode()).hexdigest()


def _inventory_shape(rows):
    if not isinstance(rows, dict) or set(rows) != {str(k) for k in COMPUTE}:
        raise RuntimeError('Incomplete workflow inventory')
    for key, row in rows.items():
        if (not isinstance(row, dict) or set(row) != {'id', 'path', 'state', 'updated_at'}
                or row['id'] != int(key) or row['path'] != COMPUTE[int(key)]
                or not isinstance(row['state'], str) or row['state'] not in DISABLED | {'active'}
                or row['updated_at'] is not None and not isinstance(row['updated_at'], str)):
            raise RuntimeError('Invalid expected workflow inventory')


def validate(value):
    if not isinstance(value, dict) or set(value) != {'schema', 'generation', 'mode', 'expected', 'baseline', 'plans', 'audit', 'pending', 'attention', 'security_digest', 'verified_at', 'host_session'}:
        raise RuntimeError('Invalid compute journal')
    if type(value['schema']) is not int or value['schema'] != 1 or type(value['generation']) is not int or value['generation'] < 0 or not isinstance(value['mode'], str) or value['mode'] not in MODES:
        raise RuntimeError('Invalid compute journal state')
    if not isinstance(value['plans'], dict) or not isinstance(value['audit'], list) or type(value['attention']) is not bool:
        raise RuntimeError('Invalid compute journal history')
    previous = ''
    for index, event in enumerate(value['audit']):
        if not isinstance(event, dict) or event.get('sequence') != index or event.get('previous') != previous:
            raise RuntimeError('Broken audit ordering')
        item = dict(event); claimed = item.pop('hash', None)
        if type(item.get('time')) not in {int, float} or not math.isfinite(item['time']):
            raise RuntimeError('Invalid audit timestamp')
        if claimed != digest(item):
            raise RuntimeError('Broken audit hash')
        previous = claimed
    for key, plan in value['plans'].items():
        if (not isinstance(plan, dict) or set(plan) != {'generation', 'target', 'selected', 'reconcile', 'inventory', 'security_digest', 'active_runs', 'quiet', 'new_hosted_grant', 'reason', 'created_at'}
                or type(plan.get('generation')) is not int or not 0 <= plan['generation'] <= value['generation']
                or not isinstance(plan.get('target'), str) or plan['target'] not in MODES
                or not isinstance(plan.get('selected'), list) or any(type(k) is not int or k not in COMPUTE for k in plan['selected'])
                or any(type(plan.get(f)) is not bool for f in ['reconcile', 'quiet', 'new_hosted_grant'])
                or type(plan.get('created_at')) not in {int, float} or not math.isfinite(plan['created_at'])
                or not isinstance(plan.get('active_runs'), list) or not isinstance(plan.get('reason'), str)):
            raise RuntimeError('Invalid owner snapshot')
        _inventory_shape(plan['inventory'])
        if key != digest(plan):
            raise RuntimeError('Snapshot changed')
    if value['baseline'] is not None and value['baseline'] not in value['plans']:
        raise RuntimeError('Missing baseline snapshot')
    if value['expected'] is not None:
        _inventory_shape(value['expected'])
    if value['pending'] is not None:
        pending = value['pending']
        if (not isinstance(pending, dict) or set(pending) != {'workflow', 'target'}
                or type(pending['workflow']) is not int or pending['workflow'] not in COMPUTE
                or pending['target'] not in {'active', 'disabled_manually'}):
            raise RuntimeError('Invalid unresolved intent')
    if value['verified_at'] is not None and (type(value['verified_at']) not in {int, float} or not math.isfinite(value['verified_at'])):
        raise RuntimeError('Invalid verification timestamp')
    if value['host_session'] is not None and (not isinstance(value['host_session'], str) or not re.fullmatch('[0-9a-f]{32}', value['host_session'])):
        raise RuntimeError('Invalid trusted host session')
    return value


def new_journal():
    return {'schema': 1, 'generation': 0, 'mode': PAUSED, 'expected': None,
            'baseline': None, 'plans': {}, 'audit': [], 'pending': None, 'attention': True,
            'security_digest': None, 'verified_at': None, 'host_session': None}


def journal(path):
    return locked_state(path, create=True, validator=validate, initial_factory=new_journal,
                        ledger='compute-policy.json', lock_name='compute-policy.lock')


def event(state, kind, **fields):
    item = {'sequence': len(state['audit']), 'previous': state['audit'][-1]['hash'] if state['audit'] else '',
            'time': time.time(), 'generation': state['generation'], 'kind': kind, **fields}
    item['hash'] = digest(item); state['audit'].append(item)


def inventory(api):
    rows = {}
    for number, path in COMPUTE.items():
        value = api(f'{PREFIX}/actions/workflows/{number}')
        if value.get('id') != number or value.get('path') != path or value.get('state') not in DISABLED | {'active'}:
            raise RuntimeError('Workflow identity/state is unavailable or changed')
        rows[str(number)] = {key: value.get(key) for key in ['id', 'path', 'state', 'updated_at']}
    return rows


def security(api):
    meta = api(PREFIX)
    if meta.get('id') != REPOSITORY_ID or meta.get('default_branch') != 'main':
        raise RuntimeError('Wrong repository identity')
    privacy = api(f'{PREFIX}/actions/workflows/{PRIVACY_ID}')
    if privacy.get('id') != PRIVACY_ID or privacy.get('path') != '.github/workflows/privacy-guard.yml' or privacy.get('state') != 'active':
        raise RuntimeError('Mandatory privacy workflow must stay active')
    source = api(f'{PREFIX}/contents/.github/workflows/privacy-guard.yml?ref=main')
    if source.get('sha') != PRIVACY_BLOB:
        raise RuntimeError('Mandatory scanner definition changed; independent review required')
    rules = api(f'{PREFIX}/rules/branches/main')
    checks = [c for r in rules if r.get('type') == 'required_status_checks'
              for c in r.get('parameters', {}).get('required_status_checks', [])]
    if not any(r.get('type') == 'pull_request' for r in rules):
        raise RuntimeError('Required PR rule missing')
    if {'context': 'Publication privacy (all new commits)', 'integration_id': 15368} not in checks:
        raise RuntimeError('Required Actions15368 privacy missing')
    if any(c.get('context') not in SECURITY_CONTEXTS or SECURITY_CONTEXTS[c['context']] != c.get('integration_id') for c in checks):
        raise RuntimeError('Unknown required security context; do not pause builds')
    if any(c['context'] == 'Workflow routing contract (static)' for c in checks):
        routing = api(f'{PREFIX}/actions/workflows/ci-workflow-routing.yml')
        if routing.get('path') != '.github/workflows/ci-workflow-routing.yml' or routing.get('state') != 'active':
            raise RuntimeError('Required static security workflow unavailable')
    ids = sorted({r.get('ruleset_id') for r in rules if type(r.get('ruleset_id')) is int})
    if not ids or len(ids) > 50:
        raise RuntimeError('No complete applicable ruleset inventory')
    sources = []
    for rid in ids:
        row = api(f'{PREFIX}/rulesets/{rid}')
        if row.get('id') != rid or row.get('enforcement') != 'active' or row.get('bypass_actors') != []:
            raise RuntimeError('Required ruleset must be active without bypass actors')
        sources.append({k: row[k] for k in ['id', 'enforcement', 'bypass_actors']})
    return digest({'rules': rules, 'rulesets': sources, 'privacy_blob': PRIVACY_BLOB})


def authorize(api):
    actor = api('user')
    if actor.get('id') != OWNER_ID or actor.get('login') != OWNER:
        raise RuntimeError('Authenticated repository owner is required')
    if api(PREFIX).get('permissions', {}).get('admin') is not True:
        raise RuntimeError('Owner administration permission unavailable')
    if not api.workflow_write_verified():
        raise RuntimeError('Workflow write grant unverified; no transition allowed')
    return actor['id']


def active_runs(api):
    result = {}
    for status in ['queued', 'in_progress', 'waiting', 'pending', 'requested']:
        seen = 0
        for page in range(1, 11):
            data = api(f'{PREFIX}/actions/runs?status={status}&per_page=100&page={page}')
            rows = data['workflow_runs']
            seen += len(rows)
            for row in rows:
                if type(row.get('id')) is not int or row['id'] <= 0:
                    raise RuntimeError('Invalid active-run identity')
                result[row['id']] = {k: row.get(k) for k in ['id', 'head_sha', 'event', 'status', 'workflow_id', 'html_url']}
            if len(rows) < 100:
                if data.get('total_count', seen) > seen:
                    raise RuntimeError('Active-run page inventory incomplete')
                break
        else:
            raise RuntimeError('Active-run inventory incomplete')
    return [result[k] for k in sorted(result)]


def inspect(api):
    return {'inventory': inventory(api), 'security_digest': security(api), 'active_runs': active_runs(api)}


def make_plan(api, path, generation, target, selected=(), reconcile=False, *, quiet=True, new_hosted_grant=False, reason="owner-decision"):
    if target not in MODES:
        raise ValueError('Unknown mode')
    if type(quiet) is not bool or type(new_hosted_grant) is not bool or not isinstance(reason, str) or not 0 < len(reason) <= 256:
        raise ValueError('Invalid explicit owner scope')
    if not quiet and target != PAUSED:
        raise ValueError('Busy override is pause-only')
    if new_hosted_grant and target != 'owner-authorized-hosted':
        raise ValueError('New hosted grant is hosted-only')
    selected = sorted(set(selected))
    if any(type(k) is not int or k not in COMPUTE for k in selected):
        raise ValueError('Unknown compute workflow')
    if (target == 'owner-authorized-hosted') != bool(selected):
        raise ValueError('Hosted resumption needs an explicit nonempty workflow selection')
    with journal(path) as store:
        state = store.load()
        authorize(api)
        if generation != state['generation']:
            raise RuntimeError('Stale owner generation')
        facts = inspect(api)
        drift = state['expected'] is not None and facts['inventory'] != state['expected']
        if (drift or state['pending']) and not (reconcile and target == PAUSED):
            raise RuntimeError('External/uncertain state change; explicit fresh pause reconciliation required')
        if quiet and facts['active_runs']:
            raise RuntimeError('Quiet owner-coordinated window required; active runs are not cancelled')
        baseline = state['plans'][state['baseline']]['inventory'] if state['baseline'] else facts['inventory']
        if not new_hosted_grant and any(baseline[str(k)]['state'] != 'active' for k in selected):
            raise RuntimeError('Cannot restore a workflow that was originally disabled')
        plan = {'generation': generation, 'target': target, 'selected': selected, 'reconcile': reconcile,
                'inventory': facts['inventory'], 'security_digest': facts['security_digest'], 'active_runs': facts['active_runs'],
                'quiet': quiet, 'new_hosted_grant': new_hosted_grant, 'reason': reason, 'created_at': time.time()}
        key = digest(plan)
        state['plans'][key] = copy.deepcopy(plan)
        if state['baseline'] is None or reconcile:
            state['baseline'] = key
        event(state, 'plan_recorded', plan_id=key)
        store.save(state)
        return {'plan_id': key, **plan}


def health(probe, now=None):
    try:
        data = probe()
        stamp = data.get('observed_at');now = time.time() if now is None else now
        session = data.get('session_id')
        if (data.get('healthy') is True and type(stamp) in {int, float} and math.isfinite(stamp)
                and -5 <= now - stamp <= 60 and isinstance(session, str) and re.fullmatch('[0-9a-f]{32}', session)):
            return {'observed_at': stamp, 'session_id': session}
    except Exception:
        pass
    return None


def healthy(probe, now=None):
    return health(probe, now) is not None


def _set(api, store, state, number, target, security_digest):
    if number not in COMPUTE or target not in {'active', 'disabled_manually'}:
        raise ValueError('Only reviewed compute workflows are writable')
    if security(api) != security_digest or inventory(api) != state['expected']:
        raise RuntimeError('Live inventory/security changed during transition')
    if state['expected'][str(number)]['state'] == target:
        return
    state['pending'] = {'workflow': number, 'target': target}
    event(state, 'write_intent', workflow=number, target=target)
    store.save(state)  # Durable intent precedes every remote mutation.
    try:
        api(f'{PREFIX}/actions/workflows/{number}/' + ('enable' if target == 'active' else 'disable'), 'PUT')
    finally:
        # An API error may follow an applied write. Re-read independently before
        # any compensation; never blindly repeat an uncertain enable.
        observed = inventory(api)
        expected = copy.deepcopy(state['expected'])
        expected[str(number)] = observed[str(number)]
        if observed != expected:
            raise RuntimeError('Unrelated workflow changed during write')
        state['expected'] = observed
        event(state, 'write_readback', workflow=number, remote_state=observed[str(number)]['state'])
        store.save(state)
    if state['expected'][str(number)]['state'] != target:
        raise RuntimeError('Workflow write did not converge')
    state['pending'] = None;store.save(state)


def _pause(api, store, state, rule, keep=()):
    for number in COMPUTE:
        if number not in keep and state['expected'][str(number)]['state'] == 'active':
            _set(api, store, state, number, 'disabled_manually', rule)


def execute(api, path, plan_id, generation, enabled=False, host_probe=lambda: {}):
    if enabled is not True:
        raise RuntimeError('Deployment is disabled; no workflow mutation')
    with journal(path) as store:
        state = store.load()
        actor = authorize(api)
        if generation != state['generation'] or plan_id not in state['plans']:
            raise RuntimeError('Stale or unknown owner decision')
        plan = state['plans'][plan_id]
        if not -5 <= time.time() - plan['created_at'] <= 300:
            raise RuntimeError('Owner plan expired; capture a fresh decision')
        if plan['generation'] != generation:
            raise RuntimeError('Snapshot belongs to an older owner generation')
        facts = inspect(api)
        if facts['inventory'] != plan['inventory'] or facts['security_digest'] != plan['security_digest']:
            raise RuntimeError('Live inventory/security changed since owner plan')
        if plan['quiet'] and facts['active_runs']:
            raise RuntimeError('Quiet window ended; no transition')
        initial_health = health(host_probe)
        if plan['target'] == 'normal' and not initial_health:
            raise RuntimeError('Fresh trusted Mac health is required for local resumption')
        state['generation'] += 1
        state['mode'] = 'recovering';state['attention'] = True
        state['expected'] = facts['inventory'];state['security_digest'] = facts['security_digest'];state['verified_at'] = None
        event(state, 'owner_decision', actor=actor, plan_id=plan_id, target=plan['target'], active_runs=len(facts['active_runs']))
        store.save(state)
        try:
            _pause(api, store, state, facts['security_digest'], keep=plan['selected'])
            if plan['target'] == 'owner-authorized-hosted':
                for number in plan['selected']:
                    _set(api, store, state, number, 'active', facts['security_digest'])
            if security(api) != facts['security_digest'] or inventory(api) != state['expected']:
                raise RuntimeError('Final readback failed')
            final_health = health(host_probe)
            if plan['target'] == 'normal' and (not final_health or final_health['session_id'] != initial_health['session_id']):
                raise RuntimeError('Mac health/session changed during resumption')
            state['host_session'] = final_health['session_id'] if plan['target'] == 'normal' else None
            state['mode'] = plan['target'];state['attention'] = False;state['pending'] = None;state['verified_at'] = time.time()
            event(state, 'transition_verified', mode=state['mode']);store.save(state)
        except BaseException:
            state['mode'] = PAUSED;state['attention'] = True;state['verified_at'] = None
            # Only compensate toward OFF. Never restore/enable from finally.
            try:
                _pause(api, store, state, facts['security_digest'])
            except BaseException:
                pass  # The durable unresolved intent + attention remain visible.
            event(state, 'transition_failed', automatic_enables=False)
            store.save(state)
            raise
        return copy.deepcopy(state)


def watch(api, path, enabled=False, host_probe=lambda: {}):
    """One watchdog tick: can only disable; never resumes after health recovery."""
    if enabled is not True:
        return {'disabled': True}
    with journal(path) as store:
        state = store.load()
        if state['expected'] is None:
            raise RuntimeError('Owner must initialize the independent control plane')
        try:
            authorize(api)
            rule = security(api);current = inventory(api)
            if state['pending'] or current != state['expected']:
                raise RuntimeError('Uncertain/external inventory requires owner reconciliation')
            observed = health(host_probe)
            if state['mode'] == 'normal' and (not observed or observed['session_id'] != state['host_session']):
                state['generation'] += 1;state['mode'] = PAUSED;state['attention'] = True;state['verified_at'] = None
                event(state, 'host_unavailable');store.save(state)
            if state['mode'] != 'owner-authorized-hosted':
                if any(v['state'] == 'active' for v in current.values()):
                    raise RuntimeError('Local/paused mode has unexpected active public compute')
            state['security_digest'] = rule;state['verified_at'] = time.time()
            event(state, 'watch_verified', mode=state['mode']);store.save(state)
        except BaseException:
            if state['mode'] != PAUSED or not state['attention']:
                state['generation'] += 1
            state['mode'] = PAUSED;state['attention'] = True;state['verified_at'] = None
            event(state, 'watch_failed', automatic_enables=False);store.save(state)
            raise
        return copy.deepcopy(state)


def admit(state, destination, host_probe=lambda: {}, *, generation=None, live=None, now=None):
    validate(state)
    now = time.time() if now is None else now
    if generation != state['generation'] or live is None or live.get('inventory') != state['expected'] or live.get('security_digest') != state['security_digest']:
        return False
    verified = state['verified_at']
    if type(verified) not in {int, float} or not math.isfinite(verified) or not -5 <= now - verified <= 60:
        return False
    if state['attention'] or state['pending'] or state['expected'] is None:
        return False
    if destination == 'local':
        observed = health(host_probe)
        return state['mode'] == 'normal' and observed is not None and observed['session_id'] == state['host_session'] and all(v['state'] != 'active' for v in state['expected'].values())
    if destination == 'hosted':
        return state['mode'] == 'owner-authorized-hosted' and any(v['state'] == 'active' for v in state['expected'].values())
    return False
