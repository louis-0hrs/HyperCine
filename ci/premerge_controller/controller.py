# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE.
"""Prepared, disabled-by-default HyperCine PR candidate controller.
No public event/text grants approval. No source code is executed on this host.
"""
import datetime
import hashlib
import json
import time
import uuid
from . import candidate_binding as candidate_approval
from . import check_protocol as check_acceptance
from . import checks_reporter
from . import bridge as relay
from .state_store import locked_state

SOURCE='louis-0hrs/HyperCine'
SOURCE_ID=1402247945
APP_ID=5612385

class ProductionReporter(checks_reporter._AppReporter):
    repository=SOURCE
    def __init__(self,cfg,**kwargs):
        if cfg.get('enabled') is not True:
            raise RuntimeError('Production reporter is not explicitly enabled')
        if cfg.get('app_id')!=APP_ID or cfg.get('repository_id')!=SOURCE_ID:
            raise ValueError('Production App/repository identity mismatch')
        super().__init__(cfg,**kwargs)

    def summary(self,source_sha,binding):
        a=getattr(self,'candidate',{})
        if a.get('repository')!=SOURCE or a.get('head_sha')!=source_sha:
            raise RuntimeError('Public check output does not match the approved candidate')
        for field in ['head_sha','base_sha','tree_sha','test_merge_sha']:
            if not relay.sha(a.get(field)):raise RuntimeError('Incomplete public candidate proof')
        return (f'Advisory or required mode: `{a.get("selection","unknown")}`.\n\n'\
                f'Approved PR #{a["pull_number"]} candidate.\n\nHead: `{source_sha}`\n\n'
                f'Base: `{a["base_sha"]}`\n\nTree: `{a["tree_sha"]}`\n\n'
                f'Test merge: `{a["test_merge_sha"]}`\n\nBinding: `{binding}`\n\n'
                'Private native Mac relay. Publication privacy is independently enforced by Actions.')

    def bind_run(self,check_id,source_sha,old,new):
        endpoint=f'/repos/{SOURCE}/check-runs/{check_id}'
        current=self.api(endpoint)
        if current.get('external_id')==new:
            self.validate(current,source_sha,new);return
        self.validate(current,source_sha,old)
        result=self.api(endpoint,'PATCH',{'external_id':new})
        self.validate(result,source_sha,new)


def policy(api,cfg):
    """Read existing PR/privacy policy for advisory; stronger policy for required.

    This function never writes rules. Advisory refuses an already-required
    reporter context; only required mode needs a reviewed no-bypass ruleset.
    """
    required_mode=cfg.get('required_gate',False)
    if type(required_mode) is not bool:raise RuntimeError('Invalid required-gate mode')
    if not required_mode:
        effective=api(f'repos/{SOURCE}/rules/branches/main')
        required=check_acceptance.required_checks(effective)
        if not any(r.get('type')=='pull_request' for r in effective):
            raise RuntimeError('Existing main PR rule is required')
        if {'context':'Publication privacy (all new commits)','app_id':15368} not in required:
            raise RuntimeError('Actions-owned privacy check must remain required')
        if any(c.get('context')==check_acceptance.RELAY_CHECK for c in required):
            raise RuntimeError('Advisory trial refused: reporter context is already required')
        strict=any(r.get('type')=='required_status_checks' and
                   r.get('parameters',{}).get('strict_required_status_checks_policy') is True for r in effective)
        return {'enforce_admins':{'enabled':False},'required_status_checks':{'strict':strict,'checks':required}}
    rid=cfg.get('ruleset_id')
    if type(rid) is not int or rid<=0:
        raise RuntimeError('Reviewed additive premerge ruleset not configured')
    rule=api(f'repos/{SOURCE}/rulesets/{rid}')
    if rule.get('id')!=rid or rule.get('enforcement')!='active' or rule.get('bypass_actors')!=[]:
        raise RuntimeError('Premerge ruleset must be active with no bypass actors')
    effective=api(f'repos/{SOURCE}/rules/branches/main')
    selected=[r for r in effective if r.get('ruleset_id')==rid]
    if not any(r.get('type')=='pull_request' for r in selected):
        raise RuntimeError('Reviewed premerge PR rule does not apply to main')
    checks=[r for r in selected if r.get('type')=='required_status_checks']
    if len(checks)!=1 or checks[0].get('parameters',{}).get('strict_required_status_checks_policy') is not True:
        raise RuntimeError('Strict current-base protection is required')
    required=check_acceptance.required_checks(selected)
    if {'context':'Publication privacy (all new commits)','app_id':15368} not in required:
        raise RuntimeError('Actions-owned privacy check must remain required')
    if cfg.get('required_gate') and {'context':check_acceptance.RELAY_CHECK,'app_id':APP_ID} not in required:
        raise RuntimeError('Required relay check has wrong or missing App source')
    return {'enforce_admins':{'enabled':True},'required_status_checks':{'strict':True,'checks':required}}


def candidate_api(api,cfg):
    def scoped(endpoint,method='GET',body=None):
        if method!='GET':raise ValueError('Candidate resolution is read-only')
        if endpoint==f'repos/{SOURCE}/branches/main/protection':return policy(api,cfg)
        if endpoint=='user' or endpoint.startswith(f'repos/{SOURCE}/'):
            return api(endpoint)
        raise ValueError('Candidate resolution outside approved source')
    return scoped


def capture(api,cfg,number,head,base,allow_draft=False):
    meta=api(f'repos/{SOURCE}')
    if meta.get('id')!=SOURCE_ID or meta.get('private') is not False or meta.get('default_branch')!='main':
        raise RuntimeError('Production source repository identity mismatch')
    if allow_draft and cfg.get('required_gate'):
        raise RuntimeError('Draft permission is advisory-only')
    approval=candidate_approval.capture_repository_head(candidate_api(api,cfg),SOURCE,number,'louis-0hrs',head,base,
        require_strict=cfg.get('required_gate',False),allow_draft=allow_draft)
    approval['advisory_allow_draft']=allow_draft
    return approval


def revalidate(api,cfg,approval):
    if approval.get('repository')!=SOURCE:raise RuntimeError('Approval repository mismatch')
    current=capture(api,cfg,approval['pull_number'],approval['head_sha'],approval['base_sha'],allow_draft=approval.get('advisory_allow_draft',False))
    if current!=approval:raise RuntimeError('Candidate approval changed')
    return current


def binding(record,run_id=None):
    a=record['approval']
    value={'repository':SOURCE,'pull':a['pull_number'],'head':a['head_sha'],'base':a['base_sha'],
           'tree':a['tree_sha'],'selection':a['selection'],'nonce':record['nonce'],'workflow':record['relay_commit'],
           'run_id':run_id,'attempt':1}
    return 'hyperci-pr-v1:'+hashlib.sha256(json.dumps(value,sort_keys=True).encode()).hexdigest()


def approve(api,cfg,state,number,head,base,allow_draft=False):
    # Caller must be an explicit LOCAL operator command, never an issue poller.
    with locked_state(state,create=True) as store:
        ledger=store.load()
        a=capture(api,cfg,number,head,base,allow_draft=allow_draft)
        relay.verify_private(api,cfg)
        key=f'{number}:{head}:{base}'
        if key in ledger and ledger[key]['state']=='invalidated':
            previous=ledger.pop(key)
            ledger[key+':retired:'+previous['nonce']]=previous
        if key in ledger and (ledger[key]['approval']!=a or ledger[key]['relay_commit']!=cfg['relay_commit']):
            raise RuntimeError('Existing approval differs; explicit invalidation/review is required')
        if key not in ledger:
            ledger[key]={'approval':a,'nonce':uuid.uuid4().hex,'relay_commit':cfg['relay_commit'],
                         'state':'approved','created':time.time()}
            store.save(ledger)
        return ledger[key]


def recover_check(api,record):
    wanted={binding(record)}
    if record.get('run_id'):wanted.add(binding(record,record['run_id']))
    found=[]
    for page in range(1,11):
        data=api(f'repos/{SOURCE}/commits/{record["approval"]["head_sha"]}/check-runs?filter=all&per_page=100&page={page}')
        for c in data['check_runs']:
            if c.get('name')==check_acceptance.RELAY_CHECK and c.get('app',{}).get('id')==APP_ID and c.get('external_id') in wanted:
                found.append(c)
        if len(data['check_runs'])<100:break
    else:raise RuntimeError('Incomplete check recovery inventory')
    if len(found)>1:raise RuntimeError('Ambiguous candidate check; no new dispatch')
    return found[0] if found else None


def actual_run(api,cfg,record):
    if record.get('run_id'):
        runs=[api(f'repos/{cfg["repository"]}/actions/runs/{record["run_id"]}')]
    else:
        runs=api(f'repos/{cfg["repository"]}/actions/workflows/{cfg["workflow"]}/runs?event=workflow_dispatch&per_page=100')['workflow_runs']
    title=f'HyperCine {record["approval"]["head_sha"]} {record["nonce"]}'
    matches=[r for r in runs if r.get('display_title')==title]
    if not matches:return None
    if len(matches)!=1:raise RuntimeError('Ambiguous private candidate run')
    run=matches[0]
    if (run.get('head_sha')!=record['relay_commit'] or run.get('event')!='workflow_dispatch'
        or run.get('head_branch')!='main' or run.get('run_attempt')!=1
        or run.get('repository',{}).get('full_name')!=cfg['repository']
        or run.get('actor',{}).get('login') not in cfg['actors']
        or run.get('triggering_actor',{}).get('login') not in cfg['actors']):
        raise RuntimeError('Private candidate run provenance mismatch')
    if record.get('run_id') and record['run_id']!=run['id']:
        raise RuntimeError('Private candidate run changed')
    return run


def outcome(jobs,run):
    if any(j.get('name') not in check_acceptance.LANES or j.get('run_attempt')!=1 for j in jobs):return 'failure'
    result=check_acceptance.lane_outcome(jobs,run['id'],run_complete=run.get('status')=='completed')
    if result=='success' and (run.get('status')!='completed' or run.get('conclusion')!='success'):
        return 'pending' if run.get('status')!='completed' else 'failure'
    return result


def cycle(api,cfg,state,reporter_factory=ProductionReporter):
    # No worker deployment or enabling is performed by this module.
    if cfg.get('enabled') is not True:return
    if cfg.get('source')!=SOURCE or cfg.get('actors')!=['louis-0hrs']:
        raise RuntimeError('Premerge source/operator configuration mismatch')
    with locked_state(state) as store:
        ledger=store.load()
        if not ledger:return
        client=reporter_factory(cfg['reporter'])
        for record in ledger.values():
            if record['state'] in {'invalidated','archived'}:continue
            a=record['approval'];source=a['head_sha']
            client.candidate=a
            pr=api(f'repos/{SOURCE}/pulls/{a["pull_number"]}')
            if pr.get('merged') is True:
                record['state']='archived';store.save(ledger);continue
            try:revalidate(api,cfg,a)
            except RuntimeError as exc:
                # Unresolved GitHub metadata is neither approval nor failure.
                if 'not resolved' in str(exc):continue
                if record.get('check_id'):
                    client.update(record['check_id'],source,record['check_binding'],'failure')
                record['state']='invalidated';record['reason']='candidate_or_policy_changed'
                store.save(ledger);continue
            if record['relay_commit']!=cfg['relay_commit']:
                raise RuntimeError('Approved workflow pin changed')
            relay.verify_private(api,cfg)
            if not record.get('check_id'):
                check=recover_check(api,record) or client.create(source,binding(record))
                record['check_id']=check['id'];record['check_binding']=check.get('external_id',binding(record))
                store.save(ledger)
            if record['state']=='approved':
                revalidate(api,cfg,a)
                record['state']='dispatching';store.save(ledger)
                api(f'repos/{cfg["repository"]}/actions/workflows/{cfg["workflow"]}/dispatches','POST',
                    {'ref':'main','inputs':{'source_sha':source,'request_id':record['nonce']}})
                record['state']='queued';store.save(ledger)
            run=actual_run(api,cfg,record)
            if run is None:
                if time.time()-record['created']>900:
                    client.update(record['check_id'],source,record['check_binding'],'failure')
                    record['state']='invalidated';record['reason']='dispatch_missing_no_redispatch'
                    store.save(ledger)
                continue
            record['run_id']=run['id'];store.save(ledger)
            new=binding(record,run['id'])
            client.bind_run(record['check_id'],source,record['check_binding'],new)
            record['check_binding']=new;store.save(ledger)
            jobs=api(f'repos/{cfg["repository"]}/actions/runs/{run["id"]}/jobs?filter=latest&per_page=100')['jobs']
            result=outcome(jobs,run)
            revalidate(api,cfg,a) # Last check immediately before any success.
            if result!=record.get('reported'):
                client.update(record['check_id'],source,new,result)
                record['reported']=result
            if run.get('status')=='completed':record['state']='completed'
            store.save(ledger)
