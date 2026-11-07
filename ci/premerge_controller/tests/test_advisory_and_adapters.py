# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE.
import copy,hashlib,json,os,subprocess,tempfile,unittest
from pathlib import Path
from unittest.mock import Mock,patch
from ci.premerge_controller import controller as p,worker_hook,admin
from ci.premerge_controller.adapters import GitHubCLI,KeyReader
from ci.premerge_controller.tests import test_controller as baseline

class AdvisoryTests(unittest.TestCase):
    setUp=baseline.PremergeTests.setUp
    response=baseline.PremergeTests.response
    approve=baseline.PremergeTests.approve
    ledger=baseline.PremergeTests.ledger
    cycle=baseline.PremergeTests.cycle
    make_run=baseline.PremergeTests.make_run
    def advisory(self):self.cfg.update(required_gate=False,ruleset_id=None);self.strict=False;self.relay_required=False
    def test_advisory_needs_no_new_ruleset_or_strict_protection(self):
        self.advisory();record=self.approve()
        self.assertEqual('head-current-base-advisory',record['approval']['selection'])
        self.assertFalse(any('/rulesets/' in x.args[0] for x in self.api.call_args_list))
    def test_advisory_refuses_an_already_required_reporter_context(self):
        self.advisory();self.relay_required=True
        with self.assertRaisesRegex(RuntimeError,'already required'):self.approve()

    def test_advisory_preserves_existing_privacy(self):
        self.advisory();self.privacy=999
        with self.assertRaises(RuntimeError):self.approve()
    def test_draft_requires_explicit_advisory_permission(self):
        self.advisory();original=self.api.side_effect
        def draft(ep,*args):
            result=original(ep,*args)
            if ep.endswith('/pulls/1'):result['draft']=True
            return result
        self.api.side_effect=draft
        with self.assertRaises(RuntimeError):self.approve()
        record=p.approve(self.api,self.cfg,self.root,1,self.head,self.base,allow_draft=True)
        self.assertTrue(record['approval']['advisory_allow_draft'])
        self.cfg['required_gate']=True
        with self.assertRaises(RuntimeError):p.revalidate(self.api,self.cfg,record['approval'])
    def test_final_revalidation_prevents_success_after_mid_cycle_head_change(self):
        record=self.approve();self.runs=[self.make_run(record)];original=self.api.side_effect
        def changed(ep,*args):
            result=original(ep,*args)
            if '/jobs?' in ep:self.head='f'*40
            return result
        self.api.side_effect=changed
        with self.assertRaises(RuntimeError):self.cycle()
        self.assertFalse(any(x[0]=='update' and x[-1]=='success' for x in self.events))
    def test_preview_is_read_only_and_does_not_load_signing_adapter(self):
        self.advisory();self.cfg['enabled']=False
        config=self.root/'config.json';config.write_text(json.dumps({'premerge':self.cfg}))
        with patch('sys.stdout'),patch('ci.premerge_controller.admin.KeyReader') as reader:
            result=admin.main(['--config',str(config),'--preview','1','--allow-draft'],api_factory=lambda cfg:self.api)
        reader.assert_not_called();self.assertFalse(result['mutation_performed'])
        self.assertTrue(result['can_approve']);self.assertFalse(self.events)
        self.assertFalse((self.root/'premerge.json').exists())

    def test_approval_mode_cannot_change_without_reapproval(self):
        record=self.approve();self.cfg['required_gate']=False;self.relay_required=False;self.relay_required=False
        with self.assertRaisesRegex(RuntimeError,'Candidate approval changed'):
            p.revalidate(self.api,self.cfg,record['approval'])

    def test_approval_cannot_claim_unconfigured_workflow_or_queue(self):
        for field in ['relay_commit','repository']:
            with self.subTest(field=field):
                original=self.cfg[field];self.cfg[field]=None
                with self.assertRaises(RuntimeError):self.approve()
                self.assertFalse((self.root/'premerge.json').exists())
                self.cfg[field]=original
    def test_dedup_cannot_return_an_approval_from_a_different_mode(self):
        self.approve();self.cfg['required_gate']=False;self.relay_required=False
        with self.assertRaisesRegex(RuntimeError,'Existing approval differs'):self.approve()

    def test_wrong_job_attempt_or_unknown_extra_job_is_failure(self):
        run={'id':42,'status':'completed','conclusion':'success'}
        jobs=copy.deepcopy(self.jobs);jobs[0]['run_attempt']=2
        self.assertEqual('failure',p.outcome(jobs,run))
        jobs=copy.deepcopy(self.jobs);jobs.append(dict(jobs[0],name='unexpected'))
        self.assertEqual('failure',p.outcome(jobs,run))
    def test_required_relay_context_must_have_the_reporter_app(self):
        original=self.api.side_effect
        def wrong(ep,*args):
            result=original(ep,*args)
            if ep.endswith('/rules/branches/main'):
                result[-1]['parameters']['required_status_checks'][-1]['integration_id']=15368
            return result
        self.api.side_effect=wrong
        with self.assertRaises(RuntimeError):self.approve()
    def test_unprotected_main_or_wrong_repository_id_reject_approval(self):
        original=self.api.side_effect
        for endpoint,bad in [(f'repos/{p.SOURCE}/branches/main',{'protected':False,'commit':{'sha':self.base}}),
                             (f'repos/{p.SOURCE}',{'id':999,'private':False,'default_branch':'main'})]:
            self.api.side_effect=lambda ep,*args,_ep=endpoint,_bad=bad:_bad if ep==_ep else original(ep,*args)
            with self.assertRaises(RuntimeError):self.approve()
        self.api.side_effect=original

class AdapterTests(unittest.TestCase):
    def test_cli_cannot_mutate_rules_runners_checks_or_merge(self):
        api=GitHubCLI({'repository':'owner/private','workflow':'review.yml'},executable='unused')
        with patch('ci.premerge_controller.adapters.subprocess.run') as run:
            for ep in [f'repos/{p.SOURCE}/rulesets',f'repos/{p.SOURCE}/pulls/1/merge',
                       'repos/owner/private/actions/runners','repos/unapproved/other']:
                with self.assertRaises(ValueError):api(ep,'POST',{})
            run.assert_not_called()
    def test_cli_allows_only_reviewed_private_dispatch(self):
        api=GitHubCLI({'repository':'owner/private','workflow':'review.yml'},executable='unused')
        with patch('ci.premerge_controller.adapters.subprocess.run',return_value=subprocess.CompletedProcess([],0,b'',b'')) as run:
            api('repos/owner/private/actions/workflows/review.yml/dispatches','POST',{'ref':'main'})
            self.assertIn('--input',run.call_args.args[0]);self.assertNotIn('shell',run.call_args.kwargs)
    def test_cli_accepts_compare_selector_but_rejects_traversal(self):
        api=GitHubCLI({},executable='unused')
        with patch('ci.premerge_controller.adapters.subprocess.run',return_value=subprocess.CompletedProcess([],0,b'{}',b'')) as run:
            self.assertEqual({},api(f"repos/{p.SOURCE}/compare/{'b'*40}...{'a'*40}"))
            for tail in ['../other','%2e%2e/other','%2f..%2fother']:
                with self.assertRaises(ValueError):api(f'repos/{p.SOURCE}/'+tail)
            self.assertEqual(1,run.call_count)

    def test_reader_refuses_wrong_app_and_unconfigured_path(self):
        reader=KeyReader(None,None)
        with self.assertRaises(ValueError):reader(15368)
        with self.assertRaises(RuntimeError):reader(p.APP_ID)
    def make_reader(self):
        tmp=tempfile.TemporaryDirectory();self.addCleanup(tmp.cleanup)
        path=Path(tmp.name)/'reader';path.write_bytes(b'placeholder executable');path.chmod(0o700)
        return path,hashlib.sha256(path.read_bytes()).hexdigest()
    def test_reader_digest_and_permissions_before_execution(self):
        path,digest=self.make_reader()
        with patch('ci.premerge_controller.adapters.subprocess.run') as run:
            with self.assertRaises(RuntimeError):KeyReader(str(path),'0'*64)(p.APP_ID)
            path.chmod(0o722)
            with self.assertRaises(RuntimeError):KeyReader(str(path),digest)(p.APP_ID)
            run.assert_not_called()
    def test_reader_returns_only_to_capturing_caller(self):
        path,digest=self.make_reader();payload=b'-----BEGIN TEST-----'+b'x'*200
        with patch('ci.premerge_controller.adapters.subprocess.run',return_value=subprocess.CompletedProcess([],0,payload,b'')) as run:
            self.assertEqual(payload,KeyReader(str(path),digest)(p.APP_ID))
            self.assertTrue(run.call_args.kwargs['capture_output'])
    def test_failed_reader_does_not_disclose_stdout_or_stderr(self):
        path,digest=self.make_reader()
        with patch('ci.premerge_controller.adapters.subprocess.run',return_value=subprocess.CompletedProcess([],1,b'PRIVATE-DUMMY',b'PRIVATE-DUMMY')):
            with self.assertRaises(RuntimeError) as error:KeyReader(str(path),digest)(p.APP_ID)
            self.assertNotIn('PRIVATE-DUMMY',str(error.exception))
    def test_example_is_disabled_and_has_no_host_values(self):
        cfg=json.loads((Path(__file__).parents[1]/'config.example.json').read_text())['premerge']
        self.assertIs(cfg['enabled'],False);self.assertIs(cfg['reporter']['enabled'],False)
        self.assertIs(cfg['required_gate'],False)
        for key in ['repository','workflow','relay_commit','ruleset_id']:self.assertIsNone(cfg[key])
        self.assertIsNone(cfg['reporter']['installation_id']);self.assertIsNone(cfg['key_reader']['program'])
    def test_worker_hook_missing_or_disabled_does_nothing(self):
        for cfg in [{},{'premerge':{'enabled':False}}]:
            with patch('ci.premerge_controller.worker_hook.controller.cycle') as cycle:
                worker_hook.poll(Mock(),cfg,Path('unused'));cycle.assert_not_called()
    def test_worker_hook_calls_only_explicitly_enabled_configuration(self):
        cfg={'premerge':{'enabled':True}}
        with patch('ci.premerge_controller.worker_hook.controller.cycle') as cycle:
            worker_hook.poll(Mock(),cfg,Path('unused'));cycle.assert_called_once()
    def test_cli_cycle_requires_execute_before_creating_api_adapter(self):
        with tempfile.TemporaryDirectory() as d:
            config=Path(d)/'config.json';config.write_text('{"premerge":{"enabled":false}}')
            factory=Mock()
            with patch('sys.stderr'):
                with self.assertRaises(SystemExit):admin.main(['--config',str(config),'--cycle','--state',d],api_factory=factory)
            factory.assert_not_called()
