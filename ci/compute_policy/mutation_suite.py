# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE.
"""Isolated policy mutations; only a designated regression failure kills one."""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

MUTATIONS = [('final-admission-state',
  'gate.py',
  '            state = store.load()\n            live =',
  '            state = snapshot\n            live =',
  'test_pause_after_watch_snapshot_prevents_private_dispatch'),
 ('dispatch-owner-lock',
  '../premerge_controller/state_store.py',
  '        fcntl.flock(lock_fd, fcntl.LOCK_EX)',
  '        pass',
  'test_dispatch_boundary_serializes_overlapping_owner_pause'),
 ('final-admission-check',
  'gate.py',
  "if not p.admit(state, 'local', host_probe, generation=generation, live=live):",
  'if False:',
  'test_final_admission_refreshes_health_session_and_live_inventory'),
 ('owner-identity',
  'policy.py',
  "if actor.get('id') != OWNER_ID or actor.get('login') != OWNER:",
  'if False:',
  'test_owner_identity_admin_required_before_transition'),
 ('owner-admin',
  'policy.py',
  "if api(PREFIX).get('permissions', {}).get('admin') is not True:",
  'if False:',
  'test_owner_identity_admin_required_before_transition'),
 ('privacy-active',
  'policy.py',
  "if privacy.get('id') != PRIVACY_ID or privacy.get('path') != '.github/workflows/privacy-guard.yml' or "
  "privacy.get('state') != 'active':",
  'if False:',
  'test_missing_privacy_wrong_app_or_unknown_required_gate_refuses'),
 ('privacy-blob',
  'policy.py',
  "if source.get('sha') != PRIVACY_BLOB:",
  'if False:',
  'test_missing_privacy_wrong_app_or_unknown_required_gate_refuses'),
 ('unknown-required-context',
  'policy.py',
  "if any(c.get('context') not in SECURITY_CONTEXTS or SECURITY_CONTEXTS[c['context']] != "
  "c.get('integration_id') for c in checks):",
  'if False:',
  'test_missing_privacy_wrong_app_or_unknown_required_gate_refuses'),
 ('rules-no-bypass',
  'policy.py',
  "or row.get('bypass_actors') != []",
  'or False',
  'test_missing_privacy_wrong_app_or_unknown_required_gate_refuses'),
 ('current-owner-plan',
  'policy.py',
  "if generation != state['generation']:raise RuntimeError('Stale owner generation')",
  'if False:raise RuntimeError("removed")',
  'test_plan_creation_requires_current_generation'),
 ('execute-generation',
  'policy.py',
  "if generation != state['generation'] or plan_id not in state['plans']:",
  "if plan_id not in state['plans']:",
  'test_stale_plan_cannot_override_newer_owner'),
 ('plan-expiry',
  'policy.py',
  "if plan['generation'] != generation or not -5 <= time.time()-plan['created_at'] <= 300:",
  "if plan['generation'] != generation:",
  'test_expired_owner_decision_is_rejected'),
 ('plan-live-inventory',
  'policy.py',
  "if facts['inventory'] != plan['inventory'] or facts['security_digest'] != plan['security_digest']:",
  'if False:',
  'test_changed_inventory_or_security_between_plan_execute_refuses'),
 ('enabled-deployment',
  'policy.py',
  "if enabled is not True:raise RuntimeError('Deployment is disabled; no Mac state transition')",
  'if False:raise RuntimeError("removed")',
  'test_disabled_config_prevents_local_state_change'),
 ('offline-local-only',
  'policy.py',
  "if state['mode']=='normal' and (not observed or observed['session_id'] != state['host_session']):",
  'if False:',
  'test_offline_mac_never_pauses_windows_linux_or_cuda'),
 ('health-freshness',
  'policy.py',
  'and -5 <= now - stamp <= 60',
  'and True',
  'test_stale_unhealthy_future_nan_health_cannot_resume'),
 ('no-healthy-auto-resume',
  'policy.py',
  "            event(state,'mac_watch_verified',mode=state['mode']);store.save(state)",
  "            if observed:state.update(mode='normal',attention=False,host_session=observed['session_id'])\n"
  "            event(state,'mac_watch_verified',mode=state['mode']);store.save(state)",
  'test_healthy_return_requires_new_owner_resume'),
 ('legacy-schema',
  'policy.py',
  "or value.get('schema') != 2",
  'or False',
  'test_legacy_global_policy_journal_is_not_imported'),
 ('admission-generation',
  'policy.py',
  "if generation != state['generation'] or live is None",
  'if live is None',
  'test_admission_requires_current_generation_live_state_and_freshness'),
 ('hosted-candidate-binding',
  'policy.py',
  "and candidate_sha==state['fallback_sha']",
  'and True',
  'test_hosted_mac_requires_explicit_exact_candidate_owner_plan'),
 ('no-workflow-toggle-in-runtime',
  'policy.py',
  "        state['generation']+=1;state['mode']='recovering'",
  "        api(f'{PREFIX}/actions/workflows/373401385/disable','PUT')\n"
  "        state['generation']+=1;state['mode']='recovering'",
  'test_every_mode_is_read_only_against_github'),
 ('transport-rejects-all-writes',
  'api.py',
  "        else:\n            raise ValueError('Mac runtime is read-only; no GitHub mutation allowed')",
  "        elif method != 'PUT':\n            raise ValueError('removed boundary')",
  'test_read_write_boundaries_never_touch_security_or_dispatch'),
 ('old-worker-fence',
  'gate.py',
  "if generation is not None and generation == state['generation']:",
  'if generation is not None:',
  'test_old_worker_error_cannot_overwrite_a_newer_owner_generation'),
 ('workflow-owner-approval',
  '../../.github/workflows/ci-standalone.yml',
  "github.actor_id == '248477380'",
  'true',
  'test_non_owner_rerun_missing_ack_and_stale_sha_cannot_start_mac'),
 ('workflow-rerun-owner',
  '../../.github/workflows/ci-standalone.yml',
  "github.triggering_actor == 'louis-0hrs'",
  'true',
  'test_non_owner_rerun_missing_ack_and_stale_sha_cannot_start_mac'),
 ('workflow-explicit-ack',
  '../../.github/workflows/ci-standalone.yml',
  'inputs.mac_hosted_fallback == true',
  'true',
  'test_non_owner_rerun_missing_ack_and_stale_sha_cannot_start_mac'),
 ('workflow-exact-sha',
  '../../.github/workflows/ci-standalone.yml',
  'inputs.mac_candidate_sha == github.sha',
  'true',
  'test_non_owner_rerun_missing_ack_and_stale_sha_cannot_start_mac'),
 ('offline-no-automatic-mac',
  '../../.github/workflows/ci-standalone.yml',
  "github.event_name == 'workflow_dispatch' && github.actor_id == '248477380' && github.triggering_actor == "
  "'louis-0hrs' && inputs.mac_hosted_fallback == true && inputs.mac_candidate_sha == github.sha",
  'true',
  'test_offline_has_no_hosted_mac_fallback_but_non_mac_jobs'),
 ('non-mac-job-preserved',
  '../../.github/workflows/ci-standalone.yml',
  '    runs-on: ${{ matrix.runner }}',
  '    if: false\n    runs-on: ${{ matrix.runner }}',
  'test_non_mac_jobs_equal_before_in_all_modes_and_events')]


def tests(root):
    return subprocess.run([sys.executable, '-m', 'unittest', 'discover', '-s', 'ci/compute_policy/tests'],
                          cwd=root, env=dict(os.environ, PYTHONDONTWRITEBYTECODE='1', PYTHONPATH=str(root)),
                          capture_output=True, text=True, timeout=90)


def copy(root):
    for name in ['compute_policy', 'premerge_controller']:
        source = Path(__file__).parents[1] / name
        dest = root / 'ci' / name;dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copytree(source, dest, ignore=shutil.ignore_patterns('__pycache__', '*.pyc'))
    source=Path(__file__).parents[2]
    shutil.copytree(source/'.github/workflows',root/'.github/workflows')
    shutil.copyfile(source/'ci/workflow_event_matrix.py',root/'ci/workflow_event_matrix.py')


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__);parser.add_argument('--output', type=Path)
    a = parser.parse_args(argv);results = []
    with tempfile.TemporaryDirectory(prefix='compute-policy-mutations-') as tmp:
        root = Path(tmp) / 'baseline';copy(root);baseline = tests(root)
        if baseline.returncode:raise RuntimeError('Policy baseline failed; receipt invalid')
        count = re.search(r'Ran (\d+) tests', baseline.stderr)
        for name, file, old, new, target in MUTATIONS:
            root = Path(tmp) / name;copy(root);path = root / 'ci/compute_policy' / file
            text = path.read_text()
            if old not in text:raise RuntimeError('Stale mutation marker: ' + name)
            text = text.replace(old, new, 1)
            if file.endswith('.py'):compile(text, file, 'exec')
            path.write_text(text)
            run = tests(root)
            failures = re.findall(r'(?:FAIL|ERROR): (test_\w+)', run.stderr)
            killed = run.returncode != 0 and target in failures and 'Failed to import test module' not in run.stderr
            results.append({'id': name, 'module': file, 'designated_test': target, 'killed': killed})
            print(name + ': ' + ('KILLED' if killed else 'INVALID_OR_SURVIVED'), flush=True)
    receipt = {'baseline_tests': int(count[1]), 'mutations': len(results), 'killed': sum(x['killed'] for x in results),
               'all_killed': all(x['killed'] for x in results), 'results': results}
    if a.output:
        a.output.parent.mkdir(parents=True, exist_ok=True);a.output.write_text(json.dumps(receipt, indent=2) + '\n')
    print(json.dumps({k: v for k, v in receipt.items() if k != 'results'}))
    if not receipt['all_killed']:raise SystemExit(1)


if __name__ == '__main__':main()
