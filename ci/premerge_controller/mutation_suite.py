# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE.
"""Controlled guard-removal mutations in isolated copies; no network or secrets.

A mutation counts as killed only when its designated regression test fails.
Syntax/import failures and unrelated test failures do not qualify as a kill.
"""
import argparse,json,os,re,shutil,subprocess,sys,tempfile,time
from pathlib import Path

MUTATIONS=[
 ('controller-disabled','controller.py',"if cfg.get('enabled') is not True:return","if False:return",'test_disabled_controller_does_not_even_read_github'),
 ('reporter-enabled','controller.py',"if cfg.get('enabled') is not True:\n", "if False:\n",'test_production_reporter_is_disabled_and_scope_restricted'),
 ('reporter-identity','controller.py',"if cfg.get('app_id')!=APP_ID or cfg.get('repository_id')!=SOURCE_ID:","if False:",'test_production_reporter_is_disabled_and_scope_restricted'),
 ('source-identity','controller.py',"if meta.get('id')!=SOURCE_ID or meta.get('private') is not False or meta.get('default_branch')!='main':","if False:",'test_unprotected_main_or_wrong_repository_id_reject_approval'),
 ('advisory-privacy','controller.py',"if {'context':'Publication privacy (all new commits)','app_id':15368} not in required:","if False:",'test_advisory_preserves_existing_privacy'),
 ('advisory-not-required','controller.py',"if any(c.get('context')==check_acceptance.RELAY_CHECK for c in required):","if False:",'test_advisory_refuses_an_already_required_reporter_context'),
 ('required-strict','controller.py',"checks[0].get('parameters',{}).get('strict_required_status_checks_policy') is not True","False",'test_privacy_strict_and_no_bypass_policy_are_mandatory'),
 ('required-bypass','controller.py',"or rule.get('bypass_actors')!=[]","or False",'test_privacy_strict_and_no_bypass_policy_are_mandatory'),
 ('required-app','controller.py',"if cfg.get('required_gate') and {'context':check_acceptance.RELAY_CHECK,'app_id':APP_ID} not in required:","if False:",'test_required_relay_context_must_have_the_reporter_app'),
 ('approval-revalidation','controller.py',"    if current!=approval:raise RuntimeError('Candidate approval changed')","    if False:raise RuntimeError('Candidate approval changed')",'test_approval_mode_cannot_change_without_reapproval'),
 ('approval-pin-ready','controller.py',"    relay.verify_private(api,cfg)\n    path=", "    pass # Reviewed pin intentionally not checked.\n    path=",'test_approval_cannot_claim_unconfigured_workflow_or_queue'),
 ('approval-dedup-binding','controller.py',"if key in ledger and (ledger[key]['approval']!=a or ledger[key]['relay_commit']!=cfg['relay_commit']):", "if False:",'test_dedup_cannot_return_an_approval_from_a_different_mode'),
 ('nonce-match','controller.py',"if r.get('display_title')==title","if True",'test_spoofed_nonce_never_emits_success'),
 ('workflow-pin','controller.py',"run.get('head_sha')!=record['relay_commit']","False",'test_changed_workflow_actor_or_run_attempt_rejects_success'),
 ('actor-match','controller.py',"run.get('actor',{}).get('login') not in cfg['actors']","False",'test_changed_workflow_actor_or_run_attempt_rejects_success'),
 ('run-attempt','controller.py',"run.get('run_attempt')!=1","False",'test_changed_workflow_actor_or_run_attempt_rejects_success'),
 ('job-attempt','controller.py',"j.get('run_attempt')!=1","False",'test_wrong_job_attempt_or_unknown_extra_job_is_failure'),
 ('unknown-lane','controller.py',"j.get('name') not in check_acceptance.LANES","False",'test_wrong_job_attempt_or_unknown_extra_job_is_failure'),
 ('run-complete','controller.py',"if result=='success' and (run.get('status')!='completed' or run.get('conclusion')!='success'):","if False:",'test_running_run_cannot_emit_terminal_success'),
 ('last-revalidation','controller.py',"            revalidate(api,cfg,a) # Last check immediately before any success.","            pass # Deliberately removed final validation.",'test_final_revalidation_prevents_success_after_mid_cycle_head_change'),
 ('no-redispatch','controller.py',"if record['state']=='approved':","if record['state'] in {'approved','dispatching'}:",'test_dispatch_uncertainty_does_not_redispatch'),
 ('public-proof-source','controller.py',"a.get('head_sha')!=source_sha","False",'test_public_proof_cannot_describe_a_different_candidate'),
 ('main-ref','candidate_binding.py',"if current_base.get('commit',{}).get('sha')!=expected_base:","if False:",'test_current_main_invalidates_approval_even_when_pr_base_is_cached'),
 ('main-protected','candidate_binding.py',"if current_base.get('protected') is not True:","if False:",'test_unprotected_main_or_wrong_repository_id_reject_approval'),
 ('draft-permission','candidate_binding.py',"(pr.get('draft') and not allow_draft)","False",'test_draft_requires_explicit_advisory_permission'),
 ('lane-run-id','check_protocol.py',"if job.get('run_id') != run_id:","if False:",'test_missing_duplicate_wrong_run_failed_cancelled_neutral_skipped_lanes'),
 ('duplicate-lane','check_protocol.py',"if len(hits) != 1:","if len(hits) == 0:",'test_missing_duplicate_wrong_run_failed_cancelled_neutral_skipped_lanes'),
 ('failed-lane','check_protocol.py',"elif job.get('conclusion') != 'success':\n            return 'failure'","elif job.get('conclusion') != 'success':\n            return 'success'",'test_missing_duplicate_wrong_run_failed_cancelled_neutral_skipped_lanes'),
 ('token-repository','checks_reporter.py',"'repository_ids': [cfg['repository_id']]","'repository_ids': []",'test_token_is_requested_for_only_hypercine_with_checks_write'),
 ('check-app-source','checks_reporter.py',"or check.get('app', {}).get('id') != self.cfg['app_id']","or False",'test_wrong_app_result_is_never_accepted'),
 ('token-excess-rights','checks_reporter.py',"set(permissions) - {'checks','metadata'}","set()",'test_wrong_installation_or_excess_token_permissions_reject_authentication'),
 ('queue-private','bridge.py',"meta.get('private') is not True","False",'test_private_queue_visibility_change_prevents_dispatch'),
 ('reader-digest','adapters.py',"if digest!=self.expected:","if False:",'test_reader_digest_and_permissions_before_execution'),
 ('cli-write-boundary','adapters.py',"if method!='GET':","if False:",'test_cli_cannot_mutate_rules_runners_checks_or_merge'),
 ('worker-disabled','worker_hook.py',"if cfg.get('enabled') is not True:return","if False:return",'test_worker_hook_missing_or_disabled_does_nothing'),
 ('example-disabled','config.example.json','"enabled": false','"enabled": true','test_example_is_disabled_and_has_no_host_values'),
 ('check-before-dispatch','controller.py',None,None,'test_pending_app_check_is_created_before_private_dispatch'),
]

def apply_mutation(text,item):
    if item[0]=='check-before-dispatch':
        a=text.index("            if not record.get('check_id'):")
        b=text.index("            if record['state']=='approved':",a)
        c=text.index('            run=actual_run(',b)
        return text[:a]+text[b:c]+text[a:b]+text[c:]
    old,new=item[2:4]
    if old not in text:raise RuntimeError('Mutation marker no longer matches: '+item[0])
    return text.replace(old,new,1)

def run_tests(root):
    env=dict(os.environ,PYTHONPATH=str(root),PYTHONDONTWRITEBYTECODE='1')
    return subprocess.run([sys.executable,'-m','unittest','discover','-s','ci/premerge_controller/tests'],
                          cwd=root,env=env,capture_output=True,text=True,timeout=90)

def copy_package(destination):
    package=destination/'ci/premerge_controller';package.parent.mkdir(parents=True)
    shutil.copytree(Path(__file__).parent,package,ignore=shutil.ignore_patterns('__pycache__','*.pyc'))
    return package

def main(argv=None):
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--output',type=Path)
    args=parser.parse_args(argv);results=[]
    with tempfile.TemporaryDirectory(prefix='premerge-mutations-') as tmp:
        root=Path(tmp)/'baseline';copy_package(root);baseline=run_tests(root)
        if baseline.returncode:raise RuntimeError('Baseline failed; mutation evidence invalid (details suppressed)')
        count=re.search(r'Ran (\d+) tests',baseline.stderr)
        for item in MUTATIONS:
            root=Path(tmp)/item[0];package=copy_package(root);path=package/item[1]
            mutated=apply_mutation(path.read_text(),item)
            if path.suffix=='.py':compile(mutated,path.name,'exec')
            elif path.suffix=='.json':json.loads(mutated)
            path.write_text(mutated);start=time.monotonic();run=run_tests(root)
            failures=re.findall(r'(?:FAIL|ERROR): (test_\w+)',run.stderr)
            expected=item[-1]
            killed=run.returncode!=0 and expected in failures and 'Failed to import test module' not in run.stderr
            results.append({'id':item[0],'module':item[1],'expected_test':expected,
                            'killed':killed,'seconds':round(time.monotonic()-start,3)})
            print(item[0]+': '+('KILLED' if killed else 'INVALID_OR_SURVIVED'),flush=True)
    report={'baseline_tests':int(count[1]) if count else None,'mutations':len(results),
            'killed':sum(r['killed'] for r in results),'all_killed':all(r['killed'] for r in results),'results':results}
    if args.output:args.output.parent.mkdir(parents=True,exist_ok=True);args.output.write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps({k:v for k,v in report.items() if k!='results'}))
    if not report['all_killed']:raise SystemExit(1)
if __name__=='__main__':main()
