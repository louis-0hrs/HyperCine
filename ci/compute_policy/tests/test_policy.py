# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE.
import copy
import json
import tempfile
import time
import unittest
from pathlib import Path
from unittest.mock import patch
from ci.compute_policy import policy as p


class FakeAPI:
    def __init__(self):
        self.rows = {str(n): {'id': n, 'path': path, 'state': 'active', 'updated_at': '0'} for n, path in p.COMPUTE.items()}
        self.calls = [];self.puts = [];self.version = 0;self.actor = p.OWNER_ID;self.admin = True;self.capability = True
        self.privacy_active = True;self.privacy_app = 15368;self.unknown_check = False;self.pr_required = True
        self.privacy_blob = p.PRIVACY_BLOB;self.bypass = [];self.routing_required = False;self.routing_active = True
        self.runs = [];self.failures = {};self.after_failures = {};self.read_fail = False
        self.signal_at = None;self.intent_path = None;self.require_intent = False;self.no_apply = False
        self.hook = None;self.persistent_failures = set()

    def workflow_write_verified(self):
        return self.capability

    def __call__(self, endpoint, method='GET', body=None):
        self.calls.append((endpoint, method))
        if method == 'PUT':
            number = endpoint.split('/')[-2];target = 'active' if endpoint.endswith('/enable') else 'disabled_manually'
            self.puts.append((int(number), target));index = len(self.puts)
            if self.require_intent:
                value = json.loads(self.intent_path.read_text())
                if value['pending'] != {'workflow': int(number), 'target': target} or value['audit'][-1]['kind'] != 'write_intent':
                    raise AssertionError('Remote write without durable matching intent')
            if (int(number), target) in self.persistent_failures:
                raise RuntimeError('persistent injected failure')
            if self.signal_at == index:
                raise KeyboardInterrupt()
            if index in self.failures:
                raise RuntimeError(self.failures.pop(index))
            if not self.no_apply:
                self.version += 1;self.rows[number]['state'] = target;self.rows[number]['updated_at'] = str(self.version)
            if self.hook:self.hook(self, int(number), target)
            if index in self.after_failures:
                raise RuntimeError(self.after_failures.pop(index))
            return None
        if endpoint == 'user':return {'id': self.actor, 'login': p.OWNER}
        if endpoint == p.PREFIX:return {'id': p.REPOSITORY_ID, 'default_branch': 'main', 'permissions': {'admin': self.admin}}
        if endpoint == f'{p.PREFIX}/contents/.github/workflows/privacy-guard.yml?ref=main':return {'sha': self.privacy_blob}
        if endpoint == f'{p.PREFIX}/rulesets/7':return {'id': 7, 'enforcement': 'active', 'bypass_actors': self.bypass}
        if endpoint == f'{p.PREFIX}/actions/workflows/ci-workflow-routing.yml':return {'path': '.github/workflows/ci-workflow-routing.yml', 'state': 'active' if self.routing_active else 'disabled_manually'}
        if endpoint == f'{p.PREFIX}/rules/branches/main':
            checks = [{'context': 'Publication privacy (all new commits)', 'integration_id': self.privacy_app}]
            if self.routing_required:checks.append({'context': 'Workflow routing contract (static)', 'integration_id': 15368})
            if self.unknown_check:checks.append({'context': 'Unreviewed compute gate', 'integration_id': 15368})
            return ([{'type': 'pull_request', 'ruleset_id': 7}] if self.pr_required else []) + [{'type': 'required_status_checks', 'ruleset_id': 7, 'parameters': {'required_status_checks': checks}}]
        if '/actions/runs?' in endpoint:return {'workflow_runs': copy.deepcopy(self.runs)}
        number = endpoint.rsplit('/', 1)[1]
        if number == str(p.PRIVACY_ID):return {'id': p.PRIVACY_ID, 'path': '.github/workflows/privacy-guard.yml', 'state': 'active' if self.privacy_active else 'disabled_manually'}
        if self.read_fail:raise RuntimeError('API inventory unavailable')
        if number in self.rows:return copy.deepcopy(self.rows[number])
        raise AssertionError(endpoint)


class PolicyTests(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory();self.addCleanup(self.tmp.cleanup)
        self.state=Path(self.tmp.name).resolve();self.api=FakeAPI()
        self.probe=lambda:{'healthy':True,'observed_at':time.time(),'session_id':'a'*32}
    def load(self):
        with p.journal(self.state) as store:return store.load()
    def plan(self,target=p.PAUSED,**kwargs):
        return p.make_plan(self.api,self.state,self.load()['generation'],target,**kwargs)
    def execute(self,plan):
        return p.execute(self.api,self.state,plan['plan_id'],plan['generation'],enabled=True,host_probe=self.probe)
    def transition(self,target=p.PAUSED,**kwargs):return self.execute(self.plan(target,**kwargs))
    def admit(self,state,destination,**kwargs):
        return p.admit(state,destination,self.probe,generation=state['generation'],live=p.inspect(self.api),**kwargs)
    def all_on(self):return all(row['state']=='active' for row in self.api.rows.values())

    def test_default_pauses_only_mac_without_touching_workflows(self):
        state=self.load();self.assertEqual(p.PAUSED,state['mode']);self.assertTrue(state['attention'])
        self.assertFalse(p.admit(state,'local'));self.assertFalse(self.api.puts);self.assertTrue(self.all_on())
    def test_normal_retains_all_hosted_non_mac_workflows(self):
        state=self.transition('normal');self.assertTrue(self.admit(state,'local'))
        self.assertTrue(self.all_on());self.assertFalse(self.api.puts);self.assertFalse(self.admit(state,'hosted'))
    def test_every_mode_is_read_only_against_github(self):
        for mode in ['normal',p.PAUSED,'recovering','owner-authorized-hosted']:
            self.transition(mode,**({'fallback_sha':'b'*40} if mode=='owner-authorized-hosted' else {}))
            self.assertTrue(self.all_on());self.assertTrue(all(method=='GET' for _,method in self.api.calls))
    def test_offline_mac_never_pauses_windows_linux_or_cuda(self):
        old=self.transition('normal');state=p.watch(self.api,self.state,enabled=True,host_probe=lambda:{})
        self.assertEqual(p.PAUSED,state['mode']);self.assertGreater(state['generation'],old['generation'])
        self.assertTrue(self.all_on());self.assertFalse(self.api.puts)
        self.assertFalse(self.admit(state,'local'));self.assertFalse(self.admit(state,'hosted',candidate_sha='b'*40))
    def test_healthy_return_requires_new_owner_resume(self):
        self.transition('normal');p.watch(self.api,self.state,enabled=True,host_probe=lambda:{})
        state=p.watch(self.api,self.state,enabled=True,host_probe=self.probe)
        self.assertEqual(p.PAUSED,state['mode']);self.assertFalse(self.admit(state,'local'))
        self.assertTrue(self.admit(self.transition('normal'),'local'));self.assertFalse(self.api.puts)
    def test_hosted_mac_requires_explicit_exact_candidate_owner_plan(self):
        with self.assertRaises(ValueError):self.plan('owner-authorized-hosted')
        state=self.transition('owner-authorized-hosted',fallback_sha='b'*40)
        self.assertTrue(self.admit(state,'hosted',candidate_sha='b'*40))
        self.assertFalse(self.admit(state,'hosted',candidate_sha='c'*40))
        self.assertFalse(self.admit(state,'hosted'));self.assertFalse(self.admit(state,'local'))
        self.assertTrue(self.all_on());self.assertFalse(self.api.puts)
    def test_hosted_choice_is_not_created_by_health_failure(self):
        state=self.transition('owner-authorized-hosted',fallback_sha='b'*40)
        state=p.watch(self.api,self.state,enabled=True,host_probe=lambda:{})
        self.assertEqual('owner-authorized-hosted',state['mode']);self.assertFalse(self.api.puts)
    def test_recovering_does_not_admit_mac_or_change_non_mac(self):
        state=self.transition('recovering');self.assertFalse(self.admit(state,'local'));self.assertTrue(self.all_on())
    def test_owner_identity_admin_required_before_transition(self):
        for field,value in [('actor',99),('admin',False)]:
            previous=getattr(self.api,field);setattr(self.api,field,value)
            with self.assertRaises(RuntimeError):self.plan()
            setattr(self.api,field,previous)
        self.assertFalse(self.api.puts)
    def test_no_workflow_write_grant_needed_for_read_only_mac_state(self):
        self.api.capability=False;self.transition('normal');self.assertFalse(self.api.puts)
    def test_missing_privacy_wrong_app_or_unknown_required_gate_refuses(self):
        for field,value in [('privacy_active',False),('privacy_app',99),('unknown_check',True),('pr_required',False),('privacy_blob','f'*40),('bypass',[{'actor_id':99}])]:
            previous=getattr(self.api,field);setattr(self.api,field,value)
            with self.subTest(field=field),self.assertRaises(RuntimeError):self.plan()
            setattr(self.api,field,previous)
        self.assertFalse(self.api.puts)
    def test_busy_non_mac_jobs_do_not_block_mac_pause(self):
        self.api.runs=[{'id':7,'status':'in_progress','workflow_id':next(iter(p.COMPUTE))}]
        self.transition('normal');self.transition();self.assertFalse(self.api.puts);self.assertTrue(self.all_on())
    def test_workflow_drift_requires_manual_reconciliation_without_toggling(self):
        self.transition('normal');first=str(next(iter(p.COMPUTE)));self.api.rows[first]['state']='disabled_manually'
        with self.assertRaises(RuntimeError):p.watch(self.api,self.state,enabled=True,host_probe=self.probe)
        self.assertEqual(p.PAUSED,self.load()['mode'])
        self.transition(reconcile=True)
        with self.assertRaises(RuntimeError):self.transition('normal')
        self.assertEqual('disabled_manually',self.api.rows[first]['state']);self.assertFalse(self.api.puts)
    def test_stale_plan_cannot_override_newer_owner(self):
        old=self.plan('normal');self.transition()
        with self.assertRaises(RuntimeError):self.execute(old)
    def test_plan_creation_requires_current_generation(self):
        self.transition()
        with self.assertRaises(RuntimeError):p.make_plan(self.api,self.state,0,'normal')
    def test_expired_owner_decision_is_rejected(self):
        plan=self.plan('normal')
        with patch('ci.compute_policy.policy.time.time',return_value=plan['created_at']+301):
            with self.assertRaises(RuntimeError):self.execute(plan)
    def test_changed_inventory_or_security_between_plan_execute_refuses(self):
        plan=self.plan('normal');self.api.rows[str(next(iter(p.COMPUTE)))]['updated_at']='changed'
        with self.assertRaises(RuntimeError):self.execute(plan)
    def test_disabled_config_prevents_local_state_change(self):
        plan=self.plan()
        with self.assertRaises(RuntimeError):p.execute(self.api,self.state,plan['plan_id'],0,enabled=False)
        self.assertEqual({'disabled':True},p.watch(self.api,self.state,enabled=False))
    def test_stale_unhealthy_future_nan_health_cannot_resume(self):
        for observation in [{},{'healthy':False,'observed_at':time.time(),'session_id':'a'*32},
                {'healthy':True,'observed_at':time.time()-61,'session_id':'a'*32},
                {'healthy':True,'observed_at':time.time()+10,'session_id':'a'*32},
                {'healthy':True,'observed_at':float('nan'),'session_id':'a'*32}]:
            with self.subTest(observation=observation):
                plan=self.plan('normal')
                with self.assertRaises(RuntimeError):p.execute(self.api,self.state,plan['plan_id'],plan['generation'],enabled=True,host_probe=lambda:observation)
    def test_new_health_epoch_requires_explicit_resume(self):
        state=self.transition('normal');old=state['generation']
        self.probe=lambda:{'healthy':True,'observed_at':time.time(),'session_id':'b'*32}
        state=p.watch(self.api,self.state,enabled=True,host_probe=self.probe)
        self.assertGreater(state['generation'],old);self.assertFalse(self.admit(state,'local'))
        self.assertTrue(self.admit(self.transition('normal'),'local'))
    def test_admission_requires_current_generation_live_state_and_freshness(self):
        state=self.transition('normal');facts=p.inspect(self.api)
        self.assertFalse(p.admit(state,'local',self.probe))
        self.assertFalse(p.admit(state,'local',self.probe,generation=state['generation']-1,live=facts))
        self.assertFalse(p.admit(state,'local',self.probe,generation=state['generation'],live=facts,now=state['verified_at']+61))
        facts['inventory']={};self.assertFalse(p.admit(state,'local',self.probe,generation=state['generation'],live=facts))
    def test_api_or_auth_outage_invalidates_old_resume_plans(self):
        for target in ['inventory','authorize']:
            self.transition('normal');old=self.plan('normal')
            with patch('ci.compute_policy.policy.'+target,side_effect=RuntimeError('unavailable')):
                with self.assertRaises(RuntimeError):p.watch(self.api,self.state,enabled=True,host_probe=self.probe)
            self.assertGreater(self.load()['generation'],old['generation'])
            with self.assertRaises(RuntimeError):self.execute(old)
    def test_health_change_during_resume_latches_pause(self):
        observations=iter([{'healthy':True,'observed_at':time.time(),'session_id':'a'*32},{}])
        plan=self.plan('normal')
        with self.assertRaises(RuntimeError):p.execute(self.api,self.state,plan['plan_id'],plan['generation'],enabled=True,host_probe=lambda:next(observations))
        self.assertEqual(p.PAUSED,self.load()['mode']);self.assertTrue(self.all_on())
    def test_audit_and_plan_tampering_rejected_before_api(self):
        self.plan();path=self.state/'compute-policy.json';original=path.read_text()
        for part in ['plans','audit']:
            data=json.loads(original)
            if part=='plans':next(iter(data['plans'].values()))['target']='normal'
            else:data['audit'][0]['kind']='forged'
            path.write_text(json.dumps(data));self.api.calls.clear()
            with self.assertRaises(RuntimeError):self.plan()
            self.assertFalse(self.api.calls)
        path.write_text(original)
    def test_legacy_global_policy_journal_is_not_imported(self):
        self.plan();path=self.state/'compute-policy.json';data=json.loads(path.read_text());data['schema']=1
        path.write_text(json.dumps(data));self.api.calls.clear()
        with self.assertRaises(RuntimeError):self.plan()
        self.assertFalse(self.api.calls)
    def test_unsafe_state_directory_or_symlink_rejected(self):
        self.state.chmod(0o777)
        with self.assertRaises(RuntimeError):self.plan()
        self.state.chmod(0o700);self.plan();path=self.state/'compute-policy.json';path.unlink();path.symlink_to(self.state/'missing')
        with self.assertRaises(RuntimeError):self.plan()
    def test_incomplete_run_inventory_fails_closed(self):
        original=self.api.__call__
        def truncated(endpoint,method='GET',body=None):
            if '/actions/runs?' in endpoint:return {'total_count':2,'workflow_runs':[{'id':1}]}
            return original(endpoint,method,body)
        with self.assertRaises(RuntimeError):p.active_runs(truncated)
