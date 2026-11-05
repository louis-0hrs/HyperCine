# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE.
"""Mac-only admission state. No GitHub write, workflow toggle or hosted dispatch."""
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
    'HyperCI / Mac relay': 5612385,
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
    fields = {'schema','generation','mode','expected','plans','audit','attention',
              'security_digest','verified_at','host_session','fallback_sha'}
    if not isinstance(value, dict) or set(value) != fields or value.get('schema') != 2:
        raise RuntimeError('Mac-only schema2 journal required; no legacy workflow-policy import')
    if (type(value['generation']) is not int or value['generation'] < 0
            or not isinstance(value['mode'],str) or value['mode'] not in MODES or type(value['attention']) is not bool
            or not isinstance(value['plans'], dict) or not isinstance(value['audit'], list)):
        raise RuntimeError('Invalid Mac policy journal')
    previous = ''
    for index,event in enumerate(value['audit']):
        if not isinstance(event,dict):raise RuntimeError('Invalid audit event')
        if event.get('sequence') != index or event.get('previous') != previous:
            raise RuntimeError('Broken audit ordering')
        item=dict(event);claimed=item.pop('hash',None)
        if claimed != digest(item):raise RuntimeError('Broken audit hash')
        previous=claimed
    for key,plan in value['plans'].items():
        if (not isinstance(plan,dict) or set(plan) != {'generation','target','inventory','security_digest','created_at','reason','fallback_sha'}
                or key != digest(plan) or type(plan['generation']) is not int
                or not 0 <= plan['generation'] <= value['generation'] or not isinstance(plan['target'],str) or plan['target'] not in MODES
                or type(plan['created_at']) not in {int,float} or not math.isfinite(plan['created_at'])):
            raise RuntimeError('Invalid owner plan')
        _inventory_shape(plan['inventory'])
        _fallback(plan['target'],plan['fallback_sha'])
    if value['expected'] is not None:_inventory_shape(value['expected'])
    for field,pattern in [('host_session','[0-9a-f]{32}'),('fallback_sha','[0-9a-f]{40}')]:
        if value[field] is not None and (not isinstance(value[field],str) or not re.fullmatch(pattern,value[field])):
            raise RuntimeError('Invalid Mac session/candidate')
    if value['verified_at'] is not None and (type(value['verified_at']) not in {int,float} or not math.isfinite(value['verified_at'])):
        raise RuntimeError('Invalid verification time')
    return value


def _fallback(target, sha):
    if target == 'owner-authorized-hosted':
        if not isinstance(sha,str) or not re.fullmatch('[0-9a-f]{40}',sha):
            raise ValueError('Hosted Mac approval requires one exact candidate SHA')
    elif sha is not None:raise ValueError('Candidate grant is hosted-Mac only')


def new_journal():
    return {'schema':2,'generation':0,'mode':PAUSED,'expected':None,'plans':{},'audit':[],
            'attention':True,'security_digest':None,'verified_at':None,'host_session':None,'fallback_sha':None}


def journal(path):
    return locked_state(path,create=True,validator=validate,initial_factory=new_journal,
                        ledger='compute-policy.json',lock_name='compute-policy.lock')


def non_mac_active(rows):
    _inventory_shape(rows)
    if not all(row['state'] == 'active' for row in rows.values()):
        raise RuntimeError('Hosted workflow unavailable; owner attention, never automatic enable/disable')


def inspect(api):
    return {'inventory':inventory(api),'security_digest':security(api),'active_runs':active_runs(api)}


def make_plan(api,path,generation,target,*,reconcile=False,reason='owner-decision',fallback_sha=None):
    if target not in MODES or not isinstance(reason,str) or not 0 < len(reason) <= 256:
        raise ValueError('Invalid owner choice')
    _fallback(target,fallback_sha)
    with journal(path) as store:
        state=store.load();authorize(api)
        if generation != state['generation']:raise RuntimeError('Stale owner generation')
        facts=inspect(api)
        if state['expected'] is not None and facts['inventory'] != state['expected'] and not (reconcile and target == PAUSED):
            raise RuntimeError('External workflow drift; owner pause reconciliation required')
        if target != PAUSED:non_mac_active(facts['inventory'])
        plan={'generation':generation,'target':target,'inventory':facts['inventory'],
              'security_digest':facts['security_digest'],'created_at':time.time(),'reason':reason,'fallback_sha':fallback_sha}
        key=digest(plan);state['plans'][key]=copy.deepcopy(plan)
        event(state,'mac_plan_recorded',plan_id=key);store.save(state)
        return {'plan_id':key,**plan}


def execute(api,path,plan_id,generation,enabled=False,host_probe=lambda: {}):
    if enabled is not True:raise RuntimeError('Deployment is disabled; no Mac state transition')
    with journal(path) as store:
        state=store.load();actor=authorize(api)
        if generation != state['generation'] or plan_id not in state['plans']:
            raise RuntimeError('Stale or unknown owner decision')
        plan=state['plans'][plan_id]
        if plan['generation'] != generation or not -5 <= time.time()-plan['created_at'] <= 300:
            raise RuntimeError('Stale/expired owner plan')
        facts=inspect(api)
        if facts['inventory'] != plan['inventory'] or facts['security_digest'] != plan['security_digest']:
            raise RuntimeError('Inventory/security changed since plan')
        if plan['target'] != PAUSED:non_mac_active(facts['inventory'])
        observed=health(host_probe)
        if plan['target'] == 'normal' and not observed:raise RuntimeError('Fresh Mac health required')
        state['generation']+=1;state['mode']='recovering';state['attention']=True;state['verified_at']=None
        event(state,'owner_mac_decision',actor=actor,plan_id=plan_id);store.save(state)
        try:
            if security(api) != facts['security_digest'] or inventory(api) != facts['inventory']:
                raise RuntimeError('Final readback changed')
            final=health(host_probe)
            if plan['target']=='normal' and (not final or final['session_id'] != observed['session_id']):
                raise RuntimeError('Mac health changed during resumption')
            state.update(mode=plan['target'],attention=False,expected=facts['inventory'],
                         security_digest=facts['security_digest'],verified_at=time.time(),
                         host_session=final['session_id'] if plan['target']=='normal' else None,
                         fallback_sha=plan['fallback_sha'])
            event(state,'mac_transition_verified',mode=state['mode']);store.save(state)
        except BaseException:
            state.update(mode=PAUSED,attention=True,verified_at=None,fallback_sha=None)
            event(state,'mac_transition_failed',workflow_mutations=False);store.save(state);raise
        return copy.deepcopy(state)


def watch(api,path,enabled=False,host_probe=lambda: {}):
    if enabled is not True:return {'disabled':True}
    with journal(path) as store:
        state=store.load()
        if state['expected'] is None:raise RuntimeError('Owner initialization required')
        try:
            authorize(api);rule=security(api);current=inventory(api);non_mac_active(current)
            if current != state['expected']:raise RuntimeError('External inventory drift')
            observed=health(host_probe)
            if state['mode']=='normal' and (not observed or observed['session_id'] != state['host_session']):
                state['generation']+=1;state.update(mode=PAUSED,attention=True,verified_at=None,fallback_sha=None)
                event(state,'mac_unavailable');store.save(state)
            state['security_digest']=rule;state['verified_at']=time.time()
            event(state,'mac_watch_verified',mode=state['mode']);store.save(state)
        except BaseException:
            if state['mode'] != PAUSED or not state['attention']:state['generation']+=1
            state.update(mode=PAUSED,attention=True,verified_at=None,fallback_sha=None)
            event(state,'mac_watch_failed',workflow_mutations=False);store.save(state);raise
        return copy.deepcopy(state)


def admit(state,destination,host_probe=lambda: {},*,generation=None,live=None,now=None,candidate_sha=None):
    validate(state);now=time.time() if now is None else now
    if generation != state['generation'] or live is None or live.get('inventory') != state['expected'] or live.get('security_digest') != state['security_digest']:
        return False
    stamp=state['verified_at']
    if type(stamp) not in {int,float} or not math.isfinite(stamp) or not -5 <= now-stamp <= 60:
        return False
    if state['attention'] or state['expected'] is None:return False
    if not all(v['state']=='active' for v in state['expected'].values()):return False
    if destination=='local':
        observed=health(host_probe)
        return state['mode']=='normal' and observed is not None and observed['session_id']==state['host_session']
    if destination=='hosted':
        return state['mode']=='owner-authorized-hosted' and candidate_sha is not None and candidate_sha==state['fallback_sha']
    return False


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
