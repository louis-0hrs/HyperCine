# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE.
import copy,json,tempfile,time,unittest
from pathlib import Path
from unittest.mock import Mock
from ci.premerge_controller import controller as p
from ci.premerge_controller import check_protocol as c

class FakeReporter:
    def __init__(self,events):self.events=events
    def create(self,sha,binding):
        self.events.append(('create',sha,binding));return {'id':99,'external_id':binding}
    def update(self,*args):self.events.append(('update',*args))
    def bind_run(self,*args):self.events.append(('bind',*args))

class PremergeTests(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory();self.addCleanup(self.tmp.cleanup);self.root=Path(self.tmp.name).resolve()
        self.head='a'*40;self.base='b'*40;self.pin='d'*40;self.events=[];self.runs=[]
        self.cfg={'enabled':True,'source':p.SOURCE,'repository':'owner/private',
                  'workflow':'mac.yml','relay_commit':self.pin,'actors':['louis-0hrs'],
                  'ruleset_id':7,'required_gate':True,'reporter':{'enabled':True}}
        self.strict=True;self.bypass=[];self.privacy=15368;self.private=True;self.merged=False;self.relay_required=True
        self.jobs=[{'name':n,'run_id':42,'run_attempt':1,'status':'completed','conclusion':'success'} for n in c.LANES]
        self.api=Mock(side_effect=self.response);self.client=FakeReporter(self.events)
    def response(self,ep,method='GET',body=None):
        if method=='POST':
            self.events.append(('dispatch',body));return None
        if ep=='user':return {'login':'louis-0hrs'}
        if ep==f'repos/{p.SOURCE}':return {'id':p.SOURCE_ID,'private':False,'default_branch':'main'}
        if ep==f'repos/{p.SOURCE}/branches/main':return {'protected':True,'commit':{'sha':self.base}}
        if ep.endswith('/pulls/1'):
            return {'state':'open','merged':self.merged,'draft':False,'mergeable':True,
                    'head':{'sha':'a'*40,'ref':'feature','repo':{'full_name':p.SOURCE}},
                    'base':{'sha':'b'*40,'ref':'main','repo':{'full_name':p.SOURCE}},'merge_commit_sha':'c'*40}
        if ep.endswith('/rulesets/7'):return {'id':7,'enforcement':'active','bypass_actors':self.bypass}
        if ep.endswith('/rules/branches/main'):
            return [{'ruleset_id':7,'type':'pull_request'}, {'ruleset_id':7,'type':'required_status_checks',
                     'parameters':{'strict_required_status_checks_policy':self.strict,'required_status_checks':[
                         {'context':'Publication privacy (all new commits)','integration_id':self.privacy},
                         *([{'context':c.RELAY_CHECK,'integration_id':p.APP_ID}] if self.relay_required else [])]}}]
        if '/git/ref/heads/feature' in ep:return {'object':{'sha':self.head}}
        if '/compare/' in ep:return {'merge_base_commit':{'sha':self.base},'behind_by':0}
        if '/git/commits/' in ep:return {'parents':[{'sha':self.base},{'sha':self.head}],'tree':{'sha':'e'*40}}
        if ep=='repos/owner/private':return {'private':self.private,'default_branch':'main'}
        if ep.endswith('/git/ref/heads/main'):return {'object':{'sha':self.pin}}
        if '/check-runs?' in ep:return {'check_runs':[]}
        if '/runs?' in ep:return {'workflow_runs':copy.deepcopy(self.runs)}
        if ep.endswith('/actions/runs/42'):return copy.deepcopy(self.runs[0])
        if '/jobs?' in ep:return {'jobs':copy.deepcopy(self.jobs)}
        raise AssertionError(ep)
    def approve(self):return p.approve(self.api,self.cfg,self.root,1,self.head,self.base)
    def ledger(self):return json.loads((self.root/'premerge.json').read_text())
    def cycle(self):p.cycle(self.api,self.cfg,self.root,reporter_factory=lambda cfg:self.client)
    def make_run(self,record,**changes):
        result={'id':42,'display_title':f'HyperCine {record["approval"]["head_sha"]} {record["nonce"]}',
                'head_sha':self.pin,'head_branch':'main','event':'workflow_dispatch','run_attempt':1,
                'repository':{'full_name':'owner/private'},
                'actor':{'login':'louis-0hrs'},'triggering_actor':{'login':'louis-0hrs'},
                'status':'completed','conclusion':'success'}
        result.update(changes);return result
    def test_disabled_controller_does_not_even_read_github(self):
        self.approve();self.api.reset_mock();self.cfg['enabled']=False;self.cycle();self.api.assert_not_called()
    def test_explicit_approval_is_durable_and_deduplicated(self):
        first=self.approve();self.assertEqual(first,self.approve())
        self.assertEqual(1,len(self.ledger()));self.assertFalse(self.events)
    def test_pending_app_check_is_created_before_private_dispatch(self):
        self.approve();self.cycle();self.assertEqual(['create','dispatch'],[x[0] for x in self.events])
        self.cycle();self.assertEqual(1,sum(x[0]=='dispatch' for x in self.events))
    def test_three_actual_successes_produce_success_on_bound_head(self):
        record=self.approve();self.runs=[self.make_run(record)];self.cycle()
        self.assertEqual('success',self.events[-1][-1]);self.assertEqual(self.head,self.events[-1][2])
        self.assertEqual('completed',next(iter(self.ledger().values()))['state'])
    def test_cached_pr_head_and_base_do_not_preserve_old_approval(self):
        for field in ['head','base']:
            with self.subTest(field=field):
                self.setUp();self.approve();self.cycle();setattr(self,field,'f'*40);self.cycle()
                self.assertEqual('invalidated',next(iter(self.ledger().values()))['state'])
                self.assertEqual('failure',self.events[-1][-1])
    def test_success_is_revoked_when_open_pr_base_moves(self):
        record=self.approve();self.runs=[self.make_run(record)];self.cycle();self.base='f'*40;self.cycle()
        self.assertEqual('failure',self.events[-1][-1])
    def test_spoofed_nonce_never_emits_success(self):
        record=self.approve();self.runs=[self.make_run(record,display_title='wrong nonce')];self.cycle()
        self.assertFalse(any(x[0]=='update' and x[-1]=='success' for x in self.events))
    def test_changed_workflow_actor_or_run_attempt_rejects_success(self):
        for changes in [{'head_sha':'f'*40},{'actor':{'login':'untrusted'}},{'run_attempt':2}]:
            with self.subTest(changes=changes):
                self.setUp();record=self.approve();self.runs=[self.make_run(record,**changes)]
                with self.assertRaises(RuntimeError):self.cycle()
                self.assertFalse(any(x[0]=='update' and x[-1]=='success' for x in self.events))
    def test_missing_duplicate_wrong_run_failed_cancelled_neutral_skipped_lanes(self):
        run={'id':42,'status':'completed','conclusion':'success'}
        for conclusion in ['failure','cancelled','neutral','skipped','timed_out']:
            jobs=copy.deepcopy(self.jobs);jobs[-1]['conclusion']=conclusion
            self.assertEqual('failure',p.outcome(jobs,run))
        self.assertEqual('failure',p.outcome(self.jobs[:-1],run))
        self.assertEqual('failure',p.outcome(self.jobs+[self.jobs[0]],run))
        jobs=copy.deepcopy(self.jobs);jobs[-1]['run_id']=43
        self.assertEqual('failure',p.outcome(jobs,run))
    def test_running_run_cannot_emit_terminal_success(self):
        self.assertEqual('pending',p.outcome(self.jobs,{'id':42,'status':'in_progress'}))
    def test_privacy_strict_and_no_bypass_policy_are_mandatory(self):
        for field,value in [('privacy',123),('strict',False),('bypass',[{'actor_id':5}])]:
            with self.subTest(field=field):
                old=getattr(self,field);setattr(self,field,value)
                with self.assertRaises(RuntimeError):self.approve()
                setattr(self,field,old)
        self.assertFalse(self.events)
    def test_private_queue_visibility_change_prevents_dispatch(self):
        self.approve();self.private=False
        with self.assertRaises(RuntimeError):self.cycle()
        self.assertFalse(self.events)
    def test_dispatch_uncertainty_does_not_redispatch(self):
        self.approve();original=self.api.side_effect
        def failed(ep,method='GET',body=None):
            if method=='POST':raise RuntimeError('uncertain transport')
            return original(ep,method,body)
        self.api.side_effect=failed
        with self.assertRaises(RuntimeError):self.cycle()
        self.api.side_effect=original;self.cycle()
        self.assertFalse(any(x[0]=='dispatch' for x in self.events))
    def test_different_approvals_have_distinct_sha_base_nonce_binding(self):
        record=self.approve();other=copy.deepcopy(record);other['nonce']='f'*32
        self.assertNotEqual(p.binding(record),p.binding(other))
        self.assertNotEqual(p.binding(record),p.binding(record,42))
    def test_production_reporter_is_disabled_and_scope_restricted(self):
        with self.assertRaises(RuntimeError):p.ProductionReporter({'enabled':False})
        valid={'enabled':True,'repository':p.SOURCE,'app_id':p.APP_ID,'repository_id':p.SOURCE_ID,'installation_id':4242}
        for field,value in [('app_id',15368),('repository_id',999)]:
            bad=dict(valid);bad[field]=value
            with self.assertRaises(ValueError):p.ProductionReporter(bad)
    def test_no_public_runner_registration_or_automatic_merge(self):
        self.approve();self.cycle()
        self.assertFalse(any('/runners' in c.args[0] or c.args[0].endswith('/merge') for c in self.api.call_args_list))

class ProductionClientTests(unittest.TestCase):
    def setUp(self):
        self.cfg={'enabled':True,'repository':p.SOURCE,'repository_id':p.SOURCE_ID,
                  'app_id':p.APP_ID,'installation_id':4242}
    def test_public_proof_cannot_describe_a_different_candidate(self):
        client=p.ProductionReporter(self.cfg)
        client.candidate={'repository':p.SOURCE,'pull_number':1,'head_sha':'a'*40,
                          'base_sha':'b'*40,'tree_sha':'c'*40,'test_merge_sha':'d'*40}
        summary=client.summary('a'*40,'binding')
        self.assertIn('b'*40,summary)
        with self.assertRaises(RuntimeError):client.summary('f'*40,'binding')
    def test_token_is_requested_for_only_hypercine_with_checks_write(self):
        import datetime
        from unittest.mock import patch
        calls=[]
        def transport(ep,token,method='GET',body=None):
            calls.append((ep,method,body))
            if ep=='/app':return {'id':p.APP_ID}
            if ep.endswith('/installation'):return {'id':4242,'app_id':p.APP_ID}
            return {'token':'unit-test-only','expires_at':(datetime.datetime.now(datetime.timezone.utc)+datetime.timedelta(hours=1)).isoformat(),
                    'permissions':{'checks':'write','metadata':'read'}}
        with patch('ci.premerge_controller.checks_reporter.app_jwt',return_value='unit-test-jwt'):
            client=p.ProductionReporter(self.cfg,key_provider=lambda app:b'unit-test',transport=transport)
            client.access_token()
        self.assertEqual({'repository_ids':[p.SOURCE_ID],'permissions':{'checks':'write'}},calls[-1][2])
    def test_wrong_installation_or_excess_token_permissions_reject_authentication(self):
        import datetime
        from unittest.mock import patch
        for bad in ['installation','permissions']:
            def transport(ep,token,method='GET',body=None):
                if ep=='/app':return {'id':p.APP_ID}
                if ep.endswith('/installation'):return {'id':999 if bad=='installation' else 4242,'app_id':p.APP_ID}
                return {'token':'unit-test-only','expires_at':(datetime.datetime.now(datetime.timezone.utc)+datetime.timedelta(hours=1)).isoformat(),
                        'permissions':{'checks':'write','contents':'write'}}
            with patch('ci.premerge_controller.checks_reporter.app_jwt',return_value='unit-test-jwt'):
                client=p.ProductionReporter(self.cfg,key_provider=lambda app:b'unit-test',transport=transport)
                with self.assertRaises(RuntimeError):client.access_token()
    def test_other_repository_request_never_reaches_transport(self):
        transport=Mock();client=p.ProductionReporter(self.cfg,transport=transport)
        with self.assertRaises(ValueError):client.api('/repos/other/repository/check-runs')
        transport.assert_not_called()
    def test_run_rebinding_rejects_a_check_with_wrong_sha_or_app(self):
        for field,value in [('head_sha','b'*40),('app',{'id':15368})]:
            check={'id':99,'name':c.RELAY_CHECK,'head_sha':'a'*40,'app':{'id':p.APP_ID},'external_id':'old'}
            check[field]=value;transport=Mock(return_value=check)
            client=p.ProductionReporter(self.cfg,transport=transport);client.token='unit-test-only';client.expires=10**12
            with self.assertRaises(RuntimeError):client.bind_run(99,'a'*40,'old','new')
            self.assertFalse(any(x.args[2:] and x.args[2]=='PATCH' for x in transport.call_args_list))
